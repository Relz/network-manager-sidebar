#include "amneziawg/amneziawg.h"
#include "amneziawg/deadline.h"
#include "core/config.h"
#include "helper/amneziawg_helper_runner.h"
#include "helper/amneziawg_policy.h"

#include <gio/gio.h>
#include <glib-unix.h>
#include <polkit/polkit.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#define AUTHORIZATION_TIMEOUT_SECONDS 15
#define IDLE_TIMEOUT_SECONDS 30
#define MAX_PENDING_AUTHORIZATIONS 8

typedef struct _AwgServiceRequest AwgServiceRequest;

typedef enum {
  AWG_SENDER_INVENTORY = 1u << 0,
  AWG_SENDER_ACTION = 1u << 1,
} AwgSenderOperation;

typedef struct {
  GMainLoop *loop;
  GDBusNodeInfo *introspection;
  GDBusConnection *connection;
  PolkitAuthority *authority;
  GCancellable *policy_cancellable;
  guint policy_timeout_source;
  gint64 policy_deadline;
  guint64 policy_generation;
  guint64 policy_check_generation;
  gboolean policy_pending;
  gboolean policy_ready;
  GHashTable *pending_senders;
  GHashTable *pending_requests;
  NetworkSidebarAwgHelperRunner *runner;
  guint registration_id;
  guint owner_id;
  guint idle_source;
  guint sigterm_source;
  guint sigint_source;
  guint pending_authorizations;
  AwgServiceRequest *active_request;
  /* authorization_pending distinguishes authorizing from authorized-waiting. */
  AwgServiceRequest *pending_action;
  gboolean failed;
  gboolean stopping;
} AwgService;

struct _AwgServiceRequest {
  AwgService *service;
  GDBusMethodInvocation *invocation;
  char *operation;
  char *name;
  char *sender;
  gboolean profile_report;
  GVariant *config_value;
  GCancellable *authorization_cancellable;
  guint authorization_timeout_source;
  gint64 authorization_deadline;
  guint64 authorization_generation;
  guint preempt_timeout_source;
  gint64 preempt_deadline;
  gboolean authorization_pending;
  gboolean reply_sent;
};

static const char introspection_xml[] =
  "<node>"
  "  <interface name='" NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE "'>"
  "    <method name='" NETWORK_SIDEBAR_AMNEZIAWG_DBUS_EXECUTE "'>"
  "      <arg name='operation' type='s' direction='in'/>"
  "      <arg name='name' type='s' direction='in'/>"
  "      <arg name='config' type='ay' direction='in'/>"
  "      <arg name='status' type='u' direction='out'/>"
  "    </method>"
  "    <method name='" NETWORK_SIDEBAR_AMNEZIAWG_DBUS_LIST_PROFILES "'>"
  "      <arg name='status' type='u' direction='out'/>"
  "      <arg name='capabilities' type='u' direction='out'/>"
  "      <arg name='profiles' type='a(su)' direction='out'/>"
  "      <arg name='complete' type='b' direction='out'/>"
  "    </method>"
  "  </interface>"
  "</node>";

static void service_update_idle_state(AwgService *service);
static void begin_policy_validation(AwgService *service);
static void cancel_pending_authorizations(AwgService *service);
static void progress_pending_action(AwgService *service);

static gint64
monotonic_msec(void)
{
  return g_get_monotonic_time() / 1000;
}

static gboolean
operation_is_valid(const char *operation,
                    gboolean *needs_name,
                    gboolean *needs_config)
{
  *needs_name = TRUE;
  if (g_strcmp0(operation, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_IMPORT) == 0 ||
      g_strcmp0(operation, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_REPLACE) == 0) {
    *needs_config = TRUE;
    return TRUE;
  }
  if (g_strcmp0(operation, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_UP) == 0 ||
      g_strcmp0(operation, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_DOWN) == 0 ||
      g_strcmp0(operation, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_DELETE) == 0) {
    *needs_config = FALSE;
    return TRUE;
  }
  return FALSE;
}

static guint
sender_operation(const char *operation)
{
  if (g_strcmp0(operation, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_LIST_PROFILES) == 0)
    return AWG_SENDER_INVENTORY;
  return AWG_SENDER_ACTION;
}

static gboolean
sender_can_admit(AwgService *service, const char *sender, const char *operation)
{
  guint pending = GPOINTER_TO_UINT(g_hash_table_lookup(service->pending_senders,
                                                        sender));
  guint incoming = sender_operation(operation);

  return pending == 0 ||
         (pending == AWG_SENDER_INVENTORY && incoming == AWG_SENDER_ACTION) ||
         (pending == AWG_SENDER_ACTION && incoming == AWG_SENDER_INVENTORY);
}

static void
reply_error(AwgServiceRequest *request, const char *name, const char *message)
{
  if (request->reply_sent)
    return;
  request->reply_sent = TRUE;
  g_dbus_method_invocation_return_dbus_error(request->invocation, name, message);
}

static void
cancel_authorization(AwgServiceRequest *request,
                     const char *name,
                     const char *message)
{
  reply_error(request, name, message);
  if (request->service->pending_action == request)
    request->service->pending_action = NULL;
  /* The async callback still owns the request, including its sender slot. */
  g_cancellable_cancel(request->authorization_cancellable);
}

static void
service_fail_unsafe_policy(AwgService *service)
{
  service->failed = TRUE;
  if (service->stopping)
    return;
  service->stopping = TRUE;
  if (service->idle_source != 0) {
    g_source_remove(service->idle_source);
    service->idle_source = 0;
  }
  cancel_pending_authorizations(service);
  service_update_idle_state(service);
}

static void
authorization_changed_cb(PolkitAuthority *authority, gpointer user_data)
{
  AwgService *service = user_data;
  (void) authority;

  if (service->stopping)
    return;
  if (service->policy_generation == G_MAXUINT64) {
    service_fail_unsafe_policy(service);
    return;
  }
  service->policy_generation++;
  service->policy_ready = FALSE;
  /* A previous caller grant cannot cross an observed policy or session change.
   * This also revokes an authorized action waiting for inventory cleanup. */
  cancel_pending_authorizations(service);
  begin_policy_validation(service);
}

static void
authorization_owner_changed_cb(PolkitAuthority *authority,
                                GParamSpec *pspec,
                                gpointer user_data)
{
  (void) pspec;
  authorization_changed_cb(authority, user_data);
}

static void
finish_authorization(AwgServiceRequest *request)
{
  if (!request->authorization_pending)
    return;
  request->authorization_pending = FALSE;
  g_hash_table_remove(request->service->pending_requests, request);
  if (request->service->pending_authorizations > 0)
    request->service->pending_authorizations--;
  if (request->authorization_timeout_source != 0) {
    g_source_remove(request->authorization_timeout_source);
    request->authorization_timeout_source = 0;
  }
}

static void
request_free(AwgServiceRequest *request)
{
  guint pending;

  if (request == NULL)
    return;
  finish_authorization(request);
  if (request->service->active_request == request)
    request->service->active_request = NULL;
  if (request->service->pending_action == request)
    request->service->pending_action = NULL;
  if (request->preempt_timeout_source != 0)
    g_source_remove(request->preempt_timeout_source);
  pending = GPOINTER_TO_UINT(g_hash_table_lookup(request->service->pending_senders,
                                                  request->sender));
  pending &= ~sender_operation(request->operation);
  if (pending == 0)
    g_hash_table_remove(request->service->pending_senders, request->sender);
  else
    g_hash_table_replace(request->service->pending_senders,
                         g_strdup(request->sender), GUINT_TO_POINTER(pending));
  g_clear_object(&request->invocation);
  g_clear_object(&request->authorization_cancellable);
  g_clear_pointer(&request->config_value, g_variant_unref);
  g_free(request->operation);
  g_free(request->name);
  g_free(request->sender);
  service_update_idle_state(request->service);
  g_free(request);
}

static void
reply_status(AwgServiceRequest *request, NetworkSidebarAmneziaWGHelperExit status)
{
  if (request->reply_sent)
    return;
  request->reply_sent = TRUE;
  g_dbus_method_invocation_return_value(request->invocation,
                                        g_variant_new("(u)", (guint) status));
}

static void
return_status(AwgServiceRequest *request, NetworkSidebarAmneziaWGHelperExit status)
{
  reply_status(request, status);
  request_free(request);
}

static void
helper_result_cb(const NetworkSidebarAwgHelperResult *result, gpointer user_data)
{
  AwgService *service = user_data;
  AwgServiceRequest *request = service->active_request;
  const NetworkSidebarAwgProfileReport *report = result->profiles;
  GVariantBuilder profiles;

  g_return_if_fail(request != NULL);
  if (request->reply_sent)
    return;
  if (!request->profile_report) {
    reply_status(request, result->status);
    return;
  }
  g_variant_builder_init(&profiles, G_VARIANT_TYPE("a(su)"));
  for (guint i = 0; report != NULL && i < report->records->len; i++) {
    const NetworkSidebarAwgProfileReportEntry *entry = g_ptr_array_index(report->records, i);

    g_variant_builder_add(&profiles, "(su)", entry->name, (guint) entry->status);
  }
  request->reply_sent = TRUE;
  g_dbus_method_invocation_return_value(
    request->invocation,
    g_variant_new("(uu@a(su)b)", (guint) result->status, result->capabilities,
                  g_variant_builder_end(&profiles), report != NULL && report->complete));
}

static void
helper_runner_changed_cb(gpointer user_data)
{
  AwgService *service = user_data;

  if (network_sidebar_awg_helper_runner_failed(service->runner) && !service->failed) {
    service->failed = TRUE;
    service->stopping = TRUE;
    cancel_pending_authorizations(service);
  }
  /* An early inventory reply never releases the active request. The runner
   * retains execution ownership until helper/group cleanup is complete. */
  if (service->active_request != NULL &&
      !network_sidebar_awg_helper_runner_is_running(service->runner))
    request_free(service->active_request);
  progress_pending_action(service);
  service_update_idle_state(service);
}

static void
start_helper(AwgServiceRequest *request)
{
  g_autoptr(GVariant) config_value = g_steal_pointer(&request->config_value);
  const guint8 *config_data = NULL;
  gsize config_length = 0;

  if (config_value != NULL)
    config_data = g_variant_get_fixed_array(config_value, &config_length, sizeof(guint8));
  if (!network_sidebar_awg_helper_runner_start(request->service->runner,
                                                request->operation,
                                                request->name,
                                                config_data,
                                                config_length)) {
    reply_error(request,
                NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE ".Error.Busy",
                "Another AmneziaWG operation is in progress");
    request_free(request);
  }
}

static void
progress_pending_action(AwgService *service)
{
  AwgServiceRequest *request = service->pending_action;

  if (request == NULL || request->authorization_pending)
    return;
  if (service->stopping) {
    reply_error(request,
                NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE ".Error.Busy",
                "AmneziaWG management is stopping");
    request_free(request);
    return;
  }
  if (monotonic_msec() >= request->preempt_deadline) {
    return_status(request, NETWORK_SIDEBAR_AMNEZIAWG_HELPER_TIMED_OUT);
    return;
  }
  if (service->active_request != NULL ||
      network_sidebar_awg_helper_runner_is_busy(service->runner))
    return;
  if (monotonic_msec() >= request->preempt_deadline) {
    return_status(request, NETWORK_SIDEBAR_AMNEZIAWG_HELPER_TIMED_OUT);
    return;
  }
  /* Policy/session changes invalidate the grant and cancel this waiting slot.
   * Runner readiness includes retired-child reaping and the inventory lock's
   * release; an early result cannot manufacture this handoff. */
  service->pending_action = NULL;
  if (request->preempt_timeout_source != 0) {
    g_source_remove(request->preempt_timeout_source);
    request->preempt_timeout_source = 0;
  }
  service->active_request = request;
  start_helper(request);
}

static gboolean
preempt_timeout_cb(gpointer user_data)
{
  AwgServiceRequest *request = user_data;

  request->preempt_timeout_source = 0;
  progress_pending_action(request->service);
  return G_SOURCE_REMOVE;
}

static gboolean
authorization_is_current(AwgServiceRequest *request)
{
  return request->authorization_pending && !request->reply_sent &&
         !request->service->stopping &&
         request->service->policy_ready &&
         request->authorization_generation == request->service->policy_generation &&
         !g_cancellable_is_cancelled(request->authorization_cancellable) &&
         monotonic_msec() < request->authorization_deadline;
}

static void
continue_authorized_request(AwgServiceRequest *request)
{
  AwgService *service = request->service;

  finish_authorization(request);
  if (service->stopping ||
      (service->active_request != NULL &&
       !(service->pending_action == request && service->active_request->profile_report)) ||
      (service->pending_action != NULL && service->pending_action != request &&
       !service->pending_action->authorization_pending) ||
      (service->pending_action != request &&
       network_sidebar_awg_helper_runner_is_busy(service->runner))) {
    reply_error(request,
                NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE ".Error.Busy",
                "Another AmneziaWG operation is in progress");
    request_free(request);
    progress_pending_action(service);
    return;
  }

  if (service->pending_action == request) {
    GHashTableIter iter;
    gpointer key;

    g_hash_table_iter_init(&iter, service->pending_requests);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
      AwgServiceRequest *inventory = key;

      if (inventory->profile_report)
        cancel_authorization(
          inventory,
          NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE ".Error.Busy",
          "AmneziaWG inventory was superseded by a user action");
    }
    request->preempt_deadline = network_sidebar_amneziawg_deadline_cap(
      monotonic_msec(), G_MAXINT64,
      NETWORK_SIDEBAR_AMNEZIAWG_PREEMPT_TIMEOUT_MSEC);
    request->preempt_timeout_source = g_timeout_add(
      NETWORK_SIDEBAR_AMNEZIAWG_PREEMPT_TIMEOUT_MSEC,
      preempt_timeout_cb, request);
    if (service->active_request != NULL)
      network_sidebar_awg_helper_runner_stop(service->runner,
                                              NETWORK_SIDEBAR_AWG_HELPER_STOP_PREEMPTED);
    progress_pending_action(service);
    return;
  }

  service->active_request = request;
  start_helper(request);
  progress_pending_action(service);
}

static void
reject_authorization(AwgServiceRequest *request)
{
  AwgService *service = request->service;

  reply_error(request,
              NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE ".Error.NotAuthorized",
              "AmneziaWG management is not authorized");
  request_free(request);
  progress_pending_action(service);
}

static void
request_policy_checked_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  AwgServiceRequest *request = user_data;
  g_autoptr(GError) error = NULL;
  gboolean safe = network_sidebar_awg_policy_check_finish(
    POLKIT_AUTHORITY(source), result, &error);

  /* Always finish the async call, but obsolete/cancelled results cannot either
   * admit work or turn a cancelled request into a service-wide policy failure. */
  if (!authorization_is_current(request)) {
    reject_authorization(request);
    return;
  }
  if (!safe) {
    service_fail_unsafe_policy(request->service);
    request_free(request);
    return;
  }
  continue_authorized_request(request);
}

static void
authorization_checked_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  AwgServiceRequest *request = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(PolkitAuthorizationResult) authorization = NULL;

  authorization = polkit_authority_check_authorization_finish(
    POLKIT_AUTHORITY(source), result, &error);
  if (authorization == NULL ||
      !polkit_authorization_result_get_is_authorized(authorization) ||
      polkit_authorization_result_get_temporary_authorization_id(authorization) != NULL ||
      !authorization_is_current(request)) {
    reject_authorization(request);
    return;
  }
  /* Transfer callback ownership of the request to the policy stage. Its sender
   * slot, pending count, cancellable, and original deadline remain in force. */
  network_sidebar_awg_policy_check_async(request->service->authority,
                                         request->authorization_cancellable,
                                         request_policy_checked_cb,
                                         request);
}

static gboolean
authorization_timeout_cb(gpointer user_data)
{
  AwgServiceRequest *request = user_data;

  request->authorization_timeout_source = 0;
  cancel_authorization(
    request,
    NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE ".Error.NotAuthorized",
    "AmneziaWG authorization expired");
  progress_pending_action(request->service);
  return G_SOURCE_REMOVE;
}

static void
begin_authorization(AwgServiceRequest *request)
{
  AwgService *service = request->service;
  guint pending = GPOINTER_TO_UINT(g_hash_table_lookup(service->pending_senders,
                                                        request->sender));
  g_autoptr(PolkitSubject) subject = polkit_system_bus_name_new(request->sender);

  request->authorization_cancellable = g_cancellable_new();
  request->authorization_pending = TRUE;
  request->authorization_deadline = network_sidebar_amneziawg_deadline_cap(
    monotonic_msec(), G_MAXINT64, AUTHORIZATION_TIMEOUT_SECONDS * 1000u);
  request->authorization_generation = service->policy_generation;
  service->pending_authorizations++;
  g_hash_table_insert(service->pending_senders, g_strdup(request->sender),
                      GUINT_TO_POINTER(pending | sender_operation(request->operation)));
  g_hash_table_add(service->pending_requests, request);
  if (sender_operation(request->operation) == AWG_SENDER_ACTION)
    service->pending_action = request;
  if (service->idle_source != 0) {
    g_source_remove(service->idle_source);
    service->idle_source = 0;
  }
  request->authorization_timeout_source = g_timeout_add(
    AUTHORIZATION_TIMEOUT_SECONDS * 1000u, authorization_timeout_cb, request);
  polkit_authority_check_authorization(
    service->authority,
    subject,
    NETWORK_SIDEBAR_AMNEZIAWG_ACTION_ID,
    NULL,
    POLKIT_CHECK_AUTHORIZATION_FLAGS_NONE,
    request->authorization_cancellable,
    authorization_checked_cb,
    request);
}

static void
handle_execute(AwgService *service,
               GVariant *parameters,
               GDBusMethodInvocation *invocation)
{
  g_autoptr(GVariant) config_value = NULL;
  const char *operation;
  const char *name;
  const char *sender;
  gsize config_length = 0;
  gboolean needs_config = FALSE;
  gboolean needs_name = FALSE;
  AwgServiceRequest *request;

  g_variant_get(parameters, "(&s&s@ay)", &operation, &name, &config_value);
  if (service->stopping || !service->policy_ready ||
      (service->active_request != NULL && !service->active_request->profile_report) ||
      service->pending_action != NULL) {
    g_dbus_method_invocation_return_dbus_error(
      invocation,
      NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE ".Error.Busy",
      "Another AmneziaWG operation is in progress");
    return;
  }

  g_variant_get_fixed_array(config_value, &config_length, sizeof(guint8));
  if (!operation_is_valid(operation, &needs_name, &needs_config) ||
      (needs_name ? !network_sidebar_amneziawg_name_is_valid(name) : name[0] != '\0') ||
      config_length > NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE ||
      (needs_config && config_length == 0) ||
      (!needs_config && config_length != 0)) {
    g_dbus_method_invocation_return_dbus_error(
      invocation,
      NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE ".Error.InvalidRequest",
      "The AmneziaWG request is invalid");
    return;
  }

  sender = g_dbus_method_invocation_get_sender(invocation);
  if (sender == NULL) {
    g_dbus_method_invocation_return_dbus_error(
      invocation,
      NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE ".Error.NotAuthorized",
      "The D-Bus caller is unavailable");
    return;
  }
  if (service->pending_authorizations >= MAX_PENDING_AUTHORIZATIONS ||
      !sender_can_admit(service, sender, operation)) {
    g_dbus_method_invocation_return_dbus_error(
      invocation,
      NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE ".Error.Busy",
      "Another authorization request is in progress");
    return;
  }

  request = g_new0(AwgServiceRequest, 1);
  request->service = service;
  request->invocation = g_object_ref(invocation);
  request->operation = g_strdup(operation);
  request->name = g_strdup(name);
  request->sender = g_strdup(sender);
  request->config_value = g_variant_ref(config_value);
  begin_authorization(request);
}

static void
handle_list_profiles(AwgService *service, GDBusMethodInvocation *invocation)
{
  const char *sender;
  AwgServiceRequest *request;

  if (service->stopping || !service->policy_ready || service->active_request != NULL ||
      (service->pending_action != NULL &&
       !service->pending_action->authorization_pending)) {
    g_dbus_method_invocation_return_dbus_error(
      invocation,
      NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE ".Error.Busy",
      "Another AmneziaWG operation is in progress");
    return;
  }
  sender = g_dbus_method_invocation_get_sender(invocation);
  if (sender == NULL) {
    g_dbus_method_invocation_return_dbus_error(
      invocation,
      NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE ".Error.NotAuthorized",
      "The D-Bus caller is unavailable");
    return;
  }
  if (service->pending_authorizations >= MAX_PENDING_AUTHORIZATIONS ||
      !sender_can_admit(service, sender,
                        NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_LIST_PROFILES)) {
    g_dbus_method_invocation_return_dbus_error(
      invocation,
      NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE ".Error.Busy",
      "Another authorization request is in progress");
    return;
  }

  request = g_new0(AwgServiceRequest, 1);
  request->service = service;
  request->invocation = g_object_ref(invocation);
  request->operation = g_strdup(NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_LIST_PROFILES);
  request->name = g_strdup("");
  request->sender = g_strdup(sender);
  request->profile_report = TRUE;
  begin_authorization(request);
}

static void
method_call_cb(GDBusConnection *connection,
               const char *sender,
               const char *object_path,
               const char *interface_name,
               const char *method_name,
               GVariant *parameters,
               GDBusMethodInvocation *invocation,
               gpointer user_data)
{
  AwgService *service = user_data;
  (void) connection;
  (void) sender;
  (void) object_path;
  (void) interface_name;

  if (g_strcmp0(method_name, NETWORK_SIDEBAR_AMNEZIAWG_DBUS_EXECUTE) == 0) {
    handle_execute(service, parameters, invocation);
    return;
  }
  if (g_strcmp0(method_name, NETWORK_SIDEBAR_AMNEZIAWG_DBUS_LIST_PROFILES) == 0) {
    handle_list_profiles(service, invocation);
    return;
  }
  g_dbus_method_invocation_return_error(invocation,
                                        G_DBUS_ERROR,
                                        G_DBUS_ERROR_UNKNOWN_METHOD,
                                        "Unknown AmneziaWG service method");
}

static const GDBusInterfaceVTable interface_vtable = {
  .method_call = method_call_cb,
};

static gboolean
service_has_pending_work(AwgService *service)
{
  return service->active_request != NULL || service->pending_action != NULL ||
         service->pending_authorizations != 0 || service->policy_pending ||
         network_sidebar_awg_helper_runner_is_busy(service->runner);
}

static gboolean
idle_timeout_cb(gpointer user_data)
{
  AwgService *service = user_data;

  service->idle_source = 0;
  if (!service_has_pending_work(service))
    g_main_loop_quit(service->loop);
  return G_SOURCE_REMOVE;
}

static void
service_update_idle_state(AwgService *service)
{
  if (service_has_pending_work(service)) {
    if (service->idle_source != 0) {
      g_source_remove(service->idle_source);
      service->idle_source = 0;
    }
    return;
  }
  if (service->stopping) {
    g_main_loop_quit(service->loop);
    return;
  }
  if (service->idle_source == 0)
    service->idle_source = g_timeout_add_seconds(IDLE_TIMEOUT_SECONDS,
                                                 idle_timeout_cb,
                                                 service);
}

static void
cancel_pending_authorizations(AwgService *service)
{
  GHashTableIter iter;
  gpointer key;

  if (service->stopping && service->policy_cancellable != NULL)
    g_cancellable_cancel(service->policy_cancellable);
  if (service->pending_action != NULL &&
      !service->pending_action->authorization_pending) {
    reply_error(service->pending_action,
                NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE ".Error.NotAuthorized",
                "AmneziaWG authorization was cancelled");
    request_free(service->pending_action);
  }
  g_hash_table_iter_init(&iter, service->pending_requests);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    AwgServiceRequest *request = key;

    cancel_authorization(
      request,
      NETWORK_SIDEBAR_AMNEZIAWG_DBUS_INTERFACE ".Error.NotAuthorized",
      "AmneziaWG authorization was cancelled");
  }
}

static gboolean
shutdown_signal_cb(gpointer user_data)
{
  AwgService *service = user_data;

  service->stopping = TRUE;
  if (service->idle_source != 0) {
    g_source_remove(service->idle_source);
    service->idle_source = 0;
  }
  cancel_pending_authorizations(service);
  network_sidebar_awg_helper_runner_stop(service->runner,
                                          NETWORK_SIDEBAR_AWG_HELPER_STOP_SHUTDOWN);
  service_update_idle_state(service);
  return G_SOURCE_CONTINUE;
}

static void
bus_acquired_cb(GDBusConnection *connection, const char *name, gpointer user_data)
{
  AwgService *service = user_data;
  g_autoptr(GError) error = NULL;
  (void) name;

  if (service->stopping)
    return;
  service->connection = g_object_ref(connection);
  service->registration_id = g_dbus_connection_register_object(
    connection,
    NETWORK_SIDEBAR_AMNEZIAWG_DBUS_PATH,
    service->introspection->interfaces[0],
    &interface_vtable,
    service,
    NULL,
    &error);
  if (service->registration_id == 0) {
    service->failed = TRUE;
    service->stopping = TRUE;
    cancel_pending_authorizations(service);
    service_update_idle_state(service);
  }
}

static void
name_acquired_cb(GDBusConnection *connection, const char *name, gpointer user_data)
{
  (void) connection;
  (void) name;
  service_update_idle_state(user_data);
}

static void
name_lost_cb(GDBusConnection *connection, const char *name, gpointer user_data)
{
  AwgService *service = user_data;
  (void) connection;
  (void) name;

  service->failed = TRUE;
  service->stopping = TRUE;
  if (service->idle_source != 0) {
    g_source_remove(service->idle_source);
    service->idle_source = 0;
  }
  cancel_pending_authorizations(service);
  network_sidebar_awg_helper_runner_stop(service->runner,
                                          NETWORK_SIDEBAR_AWG_HELPER_STOP_SHUTDOWN);
  service_update_idle_state(service);
}

static void
finish_policy_validation(AwgService *service)
{
  service->policy_pending = FALSE;
  g_clear_handle_id(&service->policy_timeout_source, g_source_remove);
  g_clear_object(&service->policy_cancellable);
  service_update_idle_state(service);
}

static void enumerate_service_policy(AwgService *service);

static void
service_policy_checked_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  AwgService *service = user_data;
  g_autoptr(GError) error = NULL;
  gboolean safe = network_sidebar_awg_policy_check_finish(
    POLKIT_AUTHORITY(source), result, &error);

  if (service->stopping) {
    finish_policy_validation(service);
    return;
  }
  if (monotonic_msec() >= service->policy_deadline) {
    service_fail_unsafe_policy(service);
    finish_policy_validation(service);
    return;
  }
  if (service->policy_check_generation != service->policy_generation) {
    /* Coalesce changes into one new snapshot, without restarting the budget. */
    enumerate_service_policy(service);
    return;
  }
  if (!safe) {
    service_fail_unsafe_policy(service);
    finish_policy_validation(service);
    return;
  }
  service->policy_ready = TRUE;
  if (service->owner_id == 0) {
    service->owner_id = g_bus_own_name(G_BUS_TYPE_SYSTEM,
                                       NETWORK_SIDEBAR_AMNEZIAWG_DBUS_NAME,
                                       G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                                       bus_acquired_cb,
                                       name_acquired_cb,
                                       name_lost_cb,
                                       service,
                                       NULL);
  }
  finish_policy_validation(service);
}

static void
enumerate_service_policy(AwgService *service)
{
  service->policy_check_generation = service->policy_generation;
  network_sidebar_awg_policy_check_async(service->authority,
                                         service->policy_cancellable,
                                         service_policy_checked_cb,
                                         service);
}

static void
authority_ready_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  AwgService *service = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(PolkitAuthority) authority = polkit_authority_get_finish(result, &error);
  (void) source;

  if (service->stopping) {
    finish_policy_validation(service);
    return;
  }
  if (authority == NULL || monotonic_msec() >= service->policy_deadline) {
    service_fail_unsafe_policy(service);
    finish_policy_validation(service);
    return;
  }
  service->authority = g_steal_pointer(&authority);
  g_signal_connect(service->authority, "changed",
                    G_CALLBACK(authorization_changed_cb), service);
  g_signal_connect(service->authority, "sessions-changed",
                    G_CALLBACK(authorization_changed_cb), service);
  g_signal_connect(service->authority, "notify::owner",
                    G_CALLBACK(authorization_owner_changed_cb), service);
  enumerate_service_policy(service);
}

static gboolean
policy_validation_timeout_cb(gpointer user_data)
{
  AwgService *service = user_data;

  service->policy_timeout_source = 0;
  service_fail_unsafe_policy(service);
  return G_SOURCE_REMOVE;
}

static void
begin_policy_validation(AwgService *service)
{
  if (service->stopping || service->policy_pending)
    return;
  service->policy_pending = TRUE;
  service->policy_ready = FALSE;
  service->policy_cancellable = g_cancellable_new();
  service->policy_deadline = network_sidebar_amneziawg_deadline_cap(
    monotonic_msec(), G_MAXINT64, AUTHORIZATION_TIMEOUT_SECONDS * 1000u);
  service->policy_timeout_source = g_timeout_add(
    AUTHORIZATION_TIMEOUT_SECONDS * 1000u, policy_validation_timeout_cb, service);
  service_update_idle_state(service);
  if (service->authority == NULL)
    polkit_authority_get_async(service->policy_cancellable, authority_ready_cb, service);
  else
    enumerate_service_policy(service);
}

int
main(int argc, char **argv)
{
  static const NetworkSidebarAwgHelperRunnerCallbacks runner_callbacks = {
    .result = helper_result_cb,
    .changed = helper_runner_changed_cb,
  };
  AwgService service = { 0 };
  g_autoptr(GError) error = NULL;
  int status;

  if (argc != 1 || geteuid() != 0)
    return 1;
  (void) argv;
  umask(0077);
  service.introspection = g_dbus_node_info_new_for_xml(introspection_xml, &error);
  if (service.introspection == NULL)
    return 1;
  service.runner = network_sidebar_awg_helper_runner_new(&runner_callbacks, &service);
  if (service.runner == NULL) {
    g_dbus_node_info_unref(service.introspection);
    return 1;
  }
  service.loop = g_main_loop_new(NULL, FALSE);
  service.pending_senders = g_hash_table_new_full(g_str_hash,
                                                   g_str_equal,
                                                   g_free,
                                                   NULL);
  service.pending_requests = g_hash_table_new(g_direct_hash, g_direct_equal);
  service.sigterm_source = g_unix_signal_add(SIGTERM,
                                              shutdown_signal_cb,
                                              &service);
  service.sigint_source = g_unix_signal_add(SIGINT,
                                             shutdown_signal_cb,
                                             &service);
  begin_policy_validation(&service);
  g_main_loop_run(service.loop);

  if (service.registration_id != 0 && service.connection != NULL)
    g_dbus_connection_unregister_object(service.connection, service.registration_id);
  if (service.idle_source != 0)
    g_source_remove(service.idle_source);
  if (service.sigterm_source != 0)
    g_source_remove(service.sigterm_source);
  if (service.sigint_source != 0)
    g_source_remove(service.sigint_source);
  if (service.owner_id != 0)
    g_bus_unown_name(service.owner_id);
  status = service.failed ? 1 : 0;
  g_clear_object(&service.connection);
  if (service.authority != NULL)
    g_signal_handlers_disconnect_by_data(service.authority, &service);
  g_clear_object(&service.authority);
  g_clear_pointer(&service.pending_requests, g_hash_table_unref);
  g_clear_pointer(&service.pending_senders, g_hash_table_unref);
  g_clear_pointer(&service.runner, network_sidebar_awg_helper_runner_free);
  g_clear_pointer(&service.introspection, g_dbus_node_info_unref);
  g_clear_pointer(&service.loop, g_main_loop_unref);
  return status;
}
