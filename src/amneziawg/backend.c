#include "amneziawg/backend.h"

#include "amneziawg/deadline.h"
#include "core/config.h"

#include <gio/gio.h>
#include <polkit/polkit.h>
#include <unistd.h>

#define AWG_AUTHORIZATION_RETRY_SECONDS 2

typedef struct {
  gint ref_count;
  NetworkSidebarAwgBackend *backend;
  NetworkSidebarAwgBackendOperation operation;
  char *name;
  GBytes *config;
  GCancellable *cancellable;
  GCancellable *caller_cancellable;
  gulong caller_cancel_handler;
  GCancellable *lifecycle_cancellable;
  gulong lifecycle_cancel_handler;
  gint64 deadline_msec;
  GSource *deadline_source;
  gboolean completed;
  NetworkSidebarAwgBackendResultCallback callback;
  gpointer user_data;
} BackendCall;

struct _NetworkSidebarAwgBackend {
  gint ref_count;
  NetworkSidebarAwgBackendCallbacks callbacks;
  gpointer user_data;

  GCancellable *lifecycle_cancellable;
  PolkitAuthority *authority;
  gboolean authority_get_pending;
  gboolean authorization_check_pending;
  gboolean authorization_recheck;
  guint authorization_retry_source;

  gboolean started;
  gboolean stopping;
};

static void request_authority(NetworkSidebarAwgBackend *backend);
static void start_authorization_check(NetworkSidebarAwgBackend *backend);
static void schedule_authorization_retry(NetworkSidebarAwgBackend *backend);

static gboolean
backend_is_running(NetworkSidebarAwgBackend *backend)
{
  return backend != NULL && backend->started && !backend->stopping;
}

NetworkSidebarAwgBackend *
network_sidebar_awg_backend_ref(NetworkSidebarAwgBackend *backend)
{
  if (backend == NULL)
    return NULL;
  g_atomic_int_inc(&backend->ref_count);
  return backend;
}

static void
backend_stop_internal(NetworkSidebarAwgBackend *backend)
{
  if (backend->stopping)
    return;
  backend->stopping = TRUE;
  backend->started = FALSE;

  if (backend->authorization_retry_source != 0) {
    g_source_remove(backend->authorization_retry_source);
    backend->authorization_retry_source = 0;
  }
  if (backend->lifecycle_cancellable != NULL)
    g_cancellable_cancel(backend->lifecycle_cancellable);
  if (backend->authority != NULL)
    g_signal_handlers_disconnect_by_data(backend->authority, backend);
  g_clear_object(&backend->authority);
  g_clear_object(&backend->lifecycle_cancellable);

  backend->callbacks = (NetworkSidebarAwgBackendCallbacks) { 0 };
  backend->user_data = NULL;
  backend->authorization_recheck = FALSE;
}

void
network_sidebar_awg_backend_unref(NetworkSidebarAwgBackend *backend)
{
  if (backend == NULL)
    return;
  if (!g_atomic_int_dec_and_test(&backend->ref_count))
    return;
  backend_stop_internal(backend);
  g_free(backend);
}

NetworkSidebarAwgBackend *
network_sidebar_awg_backend_new(
  const NetworkSidebarAwgBackendCallbacks *callbacks,
  gpointer user_data)
{
  NetworkSidebarAwgBackend *backend = g_new0(NetworkSidebarAwgBackend, 1);

  backend->ref_count = 1;
  if (callbacks != NULL)
    backend->callbacks = *callbacks;
  backend->user_data = user_data;
  return backend;
}

static void
notify_authorization(NetworkSidebarAwgBackend *backend, gboolean authorized)
{
  void (*callback)(gboolean, gpointer);
  gpointer user_data;

  if (!backend_is_running(backend))
    return;
  if (authorized && backend->authorization_retry_source != 0) {
    g_source_remove(backend->authorization_retry_source);
    backend->authorization_retry_source = 0;
  }
  callback = backend->callbacks.authorization_changed;
  user_data = backend->user_data;
  if (callback == NULL)
    return;

  network_sidebar_awg_backend_ref(backend);
  callback(authorized, user_data);
  network_sidebar_awg_backend_unref(backend);
}

static void
finish_authorization_check(NetworkSidebarAwgBackend *backend,
                           gboolean authorized)
{
  backend->authorization_check_pending = FALSE;
  if (!backend_is_running(backend)) {
    backend->authorization_recheck = FALSE;
    return;
  }
  if (backend->authorization_recheck) {
    backend->authorization_recheck = FALSE;
    notify_authorization(backend, FALSE);
    if (backend_is_running(backend))
      start_authorization_check(backend);
  } else {
    notify_authorization(backend, authorized);
  }
}

static void
authorization_check_cb(GObject *source,
                       GAsyncResult *result,
                       gpointer user_data)
{
  NetworkSidebarAwgBackend *backend = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(PolkitAuthorizationResult) authorization = NULL;
  gboolean authorized;

  authorization = polkit_authority_check_authorization_finish(
    POLKIT_AUTHORITY(source), result, &error);
  authorized = authorization != NULL &&
               polkit_authorization_result_get_is_authorized(authorization) &&
               polkit_authorization_result_get_temporary_authorization_id(
                 authorization) == NULL;
  if (backend_is_running(backend)) {
    finish_authorization_check(backend, authorized);
    if (error != NULL)
      schedule_authorization_retry(backend);
  } else {
    backend->authorization_check_pending = FALSE;
  }
  network_sidebar_awg_backend_unref(backend);
}

static void
start_authorization_check(NetworkSidebarAwgBackend *backend)
{
  PolkitSubject *subject;

  if (!backend_is_running(backend) || backend->authority == NULL ||
      backend->authorization_check_pending)
    return;
  backend->authorization_check_pending = TRUE;
  subject = polkit_unix_process_new_for_owner(getpid(), 0, getuid());
  polkit_authority_check_authorization(
    backend->authority,
    subject,
    NETWORK_SIDEBAR_AMNEZIAWG_ACTION_ID,
    NULL,
    POLKIT_CHECK_AUTHORIZATION_FLAGS_NONE,
    backend->lifecycle_cancellable,
    authorization_check_cb,
    network_sidebar_awg_backend_ref(backend));
  g_object_unref(subject);
}

void
network_sidebar_awg_backend_invalidate_authorization(
  NetworkSidebarAwgBackend *backend)
{
  g_autoptr(NetworkSidebarAwgBackend) backend_ref =
    network_sidebar_awg_backend_ref(backend);

  backend = backend_ref;
  if (!backend_is_running(backend))
    return;
  notify_authorization(backend, FALSE);
  if (!backend_is_running(backend))
    return;
  if (backend->authorization_check_pending)
    backend->authorization_recheck = TRUE;
  else
    start_authorization_check(backend);
}

static void
authority_changed_cb(PolkitAuthority *authority, gpointer user_data)
{
  (void) authority;
  network_sidebar_awg_backend_invalidate_authorization(user_data);
}

static void
authority_owner_changed_cb(PolkitAuthority *authority,
                           GParamSpec *pspec,
                           gpointer user_data)
{
  (void) authority;
  (void) pspec;
  network_sidebar_awg_backend_invalidate_authorization(user_data);
}

static gboolean
authorization_retry_cb(gpointer user_data)
{
  NetworkSidebarAwgBackend *backend = user_data;

  backend->authorization_retry_source = 0;
  if (!backend_is_running(backend))
    return G_SOURCE_REMOVE;
  if (backend->authority == NULL)
    request_authority(backend);
  else
    start_authorization_check(backend);
  return G_SOURCE_REMOVE;
}

static void
schedule_authorization_retry(NetworkSidebarAwgBackend *backend)
{
  if (!backend_is_running(backend) ||
      backend->authorization_retry_source != 0)
    return;
  backend->authorization_retry_source = g_timeout_add_seconds_full(
    G_PRIORITY_DEFAULT,
    AWG_AUTHORIZATION_RETRY_SECONDS,
    authorization_retry_cb,
    network_sidebar_awg_backend_ref(backend),
    (GDestroyNotify) network_sidebar_awg_backend_unref);
}

static void
authority_ready_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  NetworkSidebarAwgBackend *backend = user_data;
  g_autoptr(GError) error = NULL;
  (void) source;

  backend->authority_get_pending = FALSE;
  if (backend_is_running(backend)) {
    backend->authority = polkit_authority_get_finish(result, &error);
    if (backend->authority != NULL) {
      g_signal_connect(backend->authority,
                       "changed",
                       G_CALLBACK(authority_changed_cb),
                       backend);
      g_signal_connect(backend->authority,
                       "sessions-changed",
                       G_CALLBACK(authority_changed_cb),
                       backend);
      g_signal_connect(backend->authority,
                       "notify::owner",
                       G_CALLBACK(authority_owner_changed_cb),
                       backend);
      start_authorization_check(backend);
    } else if (error != NULL &&
               !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      schedule_authorization_retry(backend);
    }
  } else {
    PolkitAuthority *ignored = polkit_authority_get_finish(result, &error);

    g_clear_object(&ignored);
  }
  network_sidebar_awg_backend_unref(backend);
}

static void
request_authority(NetworkSidebarAwgBackend *backend)
{
  if (!backend_is_running(backend) || backend->authority != NULL ||
      backend->authority_get_pending)
    return;
  backend->authority_get_pending = TRUE;
  polkit_authority_get_async(backend->lifecycle_cancellable,
                             authority_ready_cb,
                             network_sidebar_awg_backend_ref(backend));
}

void
network_sidebar_awg_backend_start(NetworkSidebarAwgBackend *backend)
{
  if (backend == NULL || backend->started || backend->stopping)
    return;
  backend->started = TRUE;
  backend->lifecycle_cancellable = g_cancellable_new();
  request_authority(backend);
}

void
network_sidebar_awg_backend_stop(NetworkSidebarAwgBackend *backend)
{
  if (backend != NULL)
    backend_stop_internal(backend);
}

static const char *
operation_verb(NetworkSidebarAwgBackendOperation operation)
{
  switch (operation) {
  case NETWORK_SIDEBAR_AWG_BACKEND_IMPORT:
    return NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_IMPORT;
  case NETWORK_SIDEBAR_AWG_BACKEND_REPLACE:
    return NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_REPLACE;
  case NETWORK_SIDEBAR_AWG_BACKEND_UP:
    return NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_UP;
  case NETWORK_SIDEBAR_AWG_BACKEND_DOWN:
    return NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_DOWN;
  case NETWORK_SIDEBAR_AWG_BACKEND_DELETE:
    return NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_DELETE;
  case NETWORK_SIDEBAR_AWG_BACKEND_LIST_PROFILES:
    return NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_LIST_PROFILES;
  default:
    return "";
  }
}

static void
cancel_call(GCancellable *parent, gpointer user_data)
{
  (void) parent;
  g_cancellable_cancel(user_data);
}

static BackendCall *
backend_call_new(NetworkSidebarAwgBackend *backend,
                 NetworkSidebarAwgBackendOperation operation,
                 const char *name,
                 GBytes *config,
                 GCancellable *cancellable,
                 NetworkSidebarAwgBackendResultCallback callback,
                 gpointer user_data)
{
  BackendCall *call = g_new0(BackendCall, 1);

  call->ref_count = 1;
  call->backend = network_sidebar_awg_backend_ref(backend);
  call->operation = operation;
  call->name = g_strdup(name);
  call->config = config != NULL ? g_bytes_ref(config) : NULL;
  call->cancellable = g_cancellable_new();
  if (cancellable != NULL) {
    call->caller_cancellable = g_object_ref(cancellable);
    call->caller_cancel_handler = g_cancellable_connect(
      call->caller_cancellable,
      G_CALLBACK(cancel_call),
      g_object_ref(call->cancellable),
      g_object_unref);
  }
  if (backend->lifecycle_cancellable != NULL) {
    call->lifecycle_cancellable = g_object_ref(backend->lifecycle_cancellable);
    call->lifecycle_cancel_handler = g_cancellable_connect(
      call->lifecycle_cancellable,
      G_CALLBACK(cancel_call),
      g_object_ref(call->cancellable),
      g_object_unref);
  }
  call->callback = callback;
  call->user_data = user_data;
  return call;
}

static BackendCall *
backend_call_ref(BackendCall *call)
{
  g_atomic_int_inc(&call->ref_count);
  return call;
}

static void
backend_call_unref(BackendCall *call)
{
  NetworkSidebarAwgBackend *backend;

  if (call == NULL || !g_atomic_int_dec_and_test(&call->ref_count))
    return;
  backend = call->backend;
  g_cancellable_disconnect(call->caller_cancellable,
                            call->caller_cancel_handler);
  if (call->lifecycle_cancel_handler != 0)
    g_cancellable_disconnect(call->lifecycle_cancellable,
                              call->lifecycle_cancel_handler);
  g_clear_object(&call->lifecycle_cancellable);
  g_clear_object(&call->caller_cancellable);
  g_clear_object(&call->cancellable);
  g_clear_pointer(&call->config, g_bytes_unref);
  g_free(call->name);
  g_free(call);
  network_sidebar_awg_backend_unref(backend);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(BackendCall, backend_call_unref)

static void
deliver_result(BackendCall *call,
               const NetworkSidebarAwgBackendResult *result)
{
  g_autoptr(BackendCall) guard = backend_call_ref(call);
  NetworkSidebarAwgBackendResultCallback callback;
  gpointer user_data;

  if (call->completed)
    return;
  call->completed = TRUE;
  callback = call->callback;
  user_data = call->user_data;
  call->callback = NULL;
  call->user_data = NULL;
  if (call->deadline_source != NULL) {
    g_source_destroy(call->deadline_source);
    g_clear_pointer(&call->deadline_source, g_source_unref);
  }
  g_cancellable_disconnect(call->caller_cancellable,
                            call->caller_cancel_handler);
  call->caller_cancel_handler = 0;
  g_cancellable_disconnect(call->lifecycle_cancellable,
                            call->lifecycle_cancel_handler);
  call->lifecycle_cancel_handler = 0;
  if (result->kind == NETWORK_SIDEBAR_AWG_BACKEND_RESULT_TIMED_OUT)
    g_cancellable_cancel(call->cancellable);
  if (callback != NULL)
    callback(result, user_data);

  /* Late transport callbacks retain only the completed call and its I/O token. */
  g_clear_object(&call->caller_cancellable);
  g_clear_object(&call->lifecycle_cancellable);
  g_clear_pointer(&call->config, g_bytes_unref);
  g_clear_pointer(&call->name, g_free);
  g_clear_pointer(&call->backend, network_sidebar_awg_backend_unref);
}

static gint
backend_call_remaining(BackendCall *call)
{
  NetworkSidebarAwgBackendResult result = { 0 };

  if (call->completed)
    return 0;
  if (!backend_is_running(call->backend) ||
      g_cancellable_is_cancelled(call->cancellable)) {
    result.kind = NETWORK_SIDEBAR_AWG_BACKEND_RESULT_CANCELLED;
  } else {
    gint remaining = network_sidebar_amneziawg_deadline_remaining_msec(
      g_get_monotonic_time() / 1000, call->deadline_msec, G_MAXINT);

    if (remaining > 0)
      return remaining;
    result.kind = NETWORK_SIDEBAR_AWG_BACKEND_RESULT_TIMED_OUT;
  }
  deliver_result(call, &result);
  return 0;
}

static gboolean
call_deadline_cb(gpointer user_data)
{
  g_autoptr(BackendCall) call = backend_call_ref(user_data);

  backend_call_remaining(call);
  return G_SOURCE_REMOVE;
}

static gboolean
error_is_timeout(const GError *error)
{
  return error != NULL &&
    (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
     g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_TIMEOUT) ||
     g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_TIMED_OUT) ||
     g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NO_REPLY));
}

static GPtrArray *
profiles_from_variant(GVariant *profiles)
{
  GPtrArray *records = g_ptr_array_new_with_free_func(
    (GDestroyNotify) network_sidebar_awg_profile_record_free);
  GVariantIter iter;
  const char *name;
  guint status;

  if (profiles == NULL ||
      !g_variant_is_of_type(profiles, G_VARIANT_TYPE("a(su)")) ||
      g_variant_n_children(profiles) >
        NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_REPORT_MAX_RECORDS) {
    g_ptr_array_unref(records);
    return NULL;
  }
  g_variant_iter_init(&iter, profiles);
  while (g_variant_iter_next(&iter, "(&su)", &name, &status))
    g_ptr_array_add(records,
                    network_sidebar_awg_profile_record_new(name, status));
  return records;
}

static void
service_call_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(BackendCall) call = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  NetworkSidebarAwgBackendResult service_result = { 0 };

  reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (backend_call_remaining(call) == 0)
    return;
  if (reply == NULL) {
    g_autofree char *remote_error = error != NULL ?
      g_dbus_error_get_remote_error(error) : NULL;

    service_result.service_replied = remote_error != NULL &&
      g_str_has_prefix(remote_error,
                       NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE ".Error.");
    if (!backend_is_running(call->backend) ||
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      service_result.kind = NETWORK_SIDEBAR_AWG_BACKEND_RESULT_CANCELLED;
    } else if (error_is_timeout(error)) {
      service_result.kind = NETWORK_SIDEBAR_AWG_BACKEND_RESULT_TIMED_OUT;
    } else if (g_strcmp0(remote_error,
                         NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE
                           ".Error.NotAuthorized") == 0) {
      service_result.kind = NETWORK_SIDEBAR_AWG_BACKEND_RESULT_NOT_AUTHORIZED;
    } else if (g_strcmp0(remote_error,
                         NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE
                           ".Error.Busy") == 0) {
      service_result.kind = NETWORK_SIDEBAR_AWG_BACKEND_RESULT_BUSY;
    } else {
      service_result.kind = NETWORK_SIDEBAR_AWG_BACKEND_RESULT_FAILED;
    }
    deliver_result(call, &service_result);
    return;
  }

  service_result.kind = NETWORK_SIDEBAR_AWG_BACKEND_RESULT_REPLY;
  service_result.service_replied = TRUE;
  if (call->operation == NETWORK_SIDEBAR_AWG_BACKEND_LIST_PROFILES) {
    g_autoptr(GVariant) profiles = NULL;
    g_autoptr(GPtrArray) records = NULL;

    g_variant_get(reply,
                  "(uu@a(su)b)",
                  &service_result.status,
                  &service_result.capabilities,
                  &profiles,
                  &service_result.inventory_complete);
    records = profiles_from_variant(profiles);
    service_result.profiles = records;
    deliver_result(call, &service_result);
    return;
  }

  g_variant_get(reply, "(u)", &service_result.status);
  deliver_result(call, &service_result);
}

static void
service_bus_ready_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(BackendCall) call = user_data;
  g_autoptr(GDBusConnection) connection = NULL;
  g_autoptr(GError) error = NULL;
  NetworkSidebarAwgBackendResult service_result = { 0 };
  GVariant *config;
  gint remaining;
  (void) source;

  connection = g_bus_get_finish(result, &error);
  remaining = backend_call_remaining(call);
  if (remaining == 0)
    return;
  if (connection == NULL) {
    service_result.kind = !backend_is_running(call->backend) ||
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ?
      NETWORK_SIDEBAR_AWG_BACKEND_RESULT_CANCELLED :
      NETWORK_SIDEBAR_AWG_BACKEND_RESULT_BUS_UNAVAILABLE;
    deliver_result(call, &service_result);
    return;
  }
  if (call->operation == NETWORK_SIDEBAR_AWG_BACKEND_LIST_PROFILES) {
    g_dbus_connection_call(
      connection,
      NETWORK_SIDEBAR_AMNEZIAWG_DBUS_NAME,
      NETWORK_SIDEBAR_AMNEZIAWG_DBUS_PATH,
      NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE,
      NETWORK_SIDEBAR_AMNEZIAWG_DBUS_LIST_PROFILES,
      NULL,
      G_VARIANT_TYPE("(uua(su)b)"),
      G_DBUS_CALL_FLAGS_NONE,
      remaining,
      call->cancellable,
      service_call_cb,
      backend_call_ref(call));
    return;
  }

  config = call->config != NULL ?
    g_variant_new_from_bytes(G_VARIANT_TYPE("ay"), call->config, TRUE) :
    g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, NULL, 0, sizeof(guint8));
  g_dbus_connection_call(
    connection,
    NETWORK_SIDEBAR_AMNEZIAWG_DBUS_NAME,
    NETWORK_SIDEBAR_AMNEZIAWG_DBUS_PATH,
    NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE,
    NETWORK_SIDEBAR_AMNEZIAWG_DBUS_EXECUTE,
    g_variant_new("(ss@ay)",
                  operation_verb(call->operation),
                  call->name,
                  config),
    G_VARIANT_TYPE("(u)"),
    G_DBUS_CALL_FLAGS_NONE,
    remaining,
    call->cancellable,
    service_call_cb,
    backend_call_ref(call));
}

void
network_sidebar_awg_backend_execute(
  NetworkSidebarAwgBackend *backend,
  NetworkSidebarAwgBackendOperation operation,
  const char *name,
  GBytes *config,
  GCancellable *cancellable,
  NetworkSidebarAwgBackendResultCallback callback,
  gpointer user_data)
{
  g_autoptr(BackendCall) call = NULL;
  gint64 started_at = g_get_monotonic_time() / 1000;
  guint budget_ms = network_sidebar_amneziawg_client_timeout_msec(
    operation_verb(operation));

  if (backend == NULL || callback == NULL)
    return;
  call = backend_call_new(backend,
                          operation,
                          name,
                          config,
                          cancellable,
                          callback,
                          user_data);
  if (!backend_is_running(backend) ||
      g_cancellable_is_cancelled(call->cancellable)) {
    NetworkSidebarAwgBackendResult result = {
      .kind = NETWORK_SIDEBAR_AWG_BACKEND_RESULT_CANCELLED,
    };

    deliver_result(call, &result);
    return;
  }
  if (budget_ms == 0) {
    NetworkSidebarAwgBackendResult result = {
      .kind = NETWORK_SIDEBAR_AWG_BACKEND_RESULT_FAILED,
    };

    deliver_result(call, &result);
    return;
  }
  call->deadline_msec = started_at + budget_ms;
  if (backend_call_remaining(call) == 0)
    return;
  call->deadline_source = g_timeout_source_new(budget_ms);
  g_source_set_ready_time(call->deadline_source, call->deadline_msec * 1000);
  g_source_set_callback(call->deadline_source,
                        call_deadline_cb,
                        backend_call_ref(call),
                        (GDestroyNotify) backend_call_unref);
  g_source_attach(call->deadline_source, g_main_context_get_thread_default());
  g_bus_get(G_BUS_TYPE_SYSTEM,
            call->cancellable,
            service_bus_ready_cb,
            backend_call_ref(call));
}
