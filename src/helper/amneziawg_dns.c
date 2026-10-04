#define _GNU_SOURCE

#include "helper/amneziawg_dns.h"
#include "amneziawg_build_config.h"
#include "amneziawg/deadline.h"
#include "helper/amneziawg_executable.h"
#include "helper/amneziawg_helper_util.h"
#include "helper/amneziawg_process.h"
#include "helper/amneziawg_subprocess.h"

#include <gio/gio.h>
#include <arpa/inet.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define RESOLVCONF_TIMEOUT_MSEC (10u * 1000u)
#define RESOLVCONF_TERMINATION_GRACE_MSEC 250u

typedef struct {
  GMutex mutex;
  GCond condition;
  GCancellable *cancellable;
  GThread *thread;
  gint64 deadline_usec;
  gboolean completed;
} DeadlineCancellation;

static gpointer
deadline_cancellation_thread(gpointer user_data)
{
  DeadlineCancellation *cancellation = user_data;

  g_mutex_lock(&cancellation->mutex);
  while (!cancellation->completed) {
    gint64 now = g_get_monotonic_time();

    if (now >= cancellation->deadline_usec ||
        network_sidebar_amneziawg_process_termination_requested()) {
      g_mutex_unlock(&cancellation->mutex);
      g_cancellable_cancel(cancellation->cancellable);
      return NULL;
    }
    g_cond_wait_until(&cancellation->condition,
                      &cancellation->mutex,
                      MIN(cancellation->deadline_usec,
                           now + 25 * G_TIME_SPAN_MILLISECOND));
  }
  g_mutex_unlock(&cancellation->mutex);
  return NULL;
}

static gboolean
deadline_cancellation_init(DeadlineCancellation *cancellation,
                           gint64 deadline_msec)
{
  g_autoptr(GError) error = NULL;
  gint64 now = network_sidebar_amneziawg_process_monotonic_msec();

  *cancellation = (DeadlineCancellation) { 0 };
  if (now < 0 || now >= deadline_msec ||
      deadline_msec > G_MAXINT64 / 1000 ||
      network_sidebar_amneziawg_process_termination_requested())
    return FALSE;
  g_mutex_init(&cancellation->mutex);
  g_cond_init(&cancellation->condition);
  cancellation->cancellable = g_cancellable_new();
  cancellation->deadline_usec = deadline_msec * 1000;
  cancellation->thread = g_thread_try_new("awg-dbus-deadline",
                                           deadline_cancellation_thread,
                                           cancellation,
                                           &error);
  if (cancellation->thread != NULL)
    return TRUE;
  g_clear_object(&cancellation->cancellable);
  g_cond_clear(&cancellation->condition);
  g_mutex_clear(&cancellation->mutex);
  return FALSE;
}

static void
deadline_cancellation_clear(DeadlineCancellation *cancellation)
{
  if (cancellation->thread != NULL) {
    g_mutex_lock(&cancellation->mutex);
    cancellation->completed = TRUE;
    g_cond_signal(&cancellation->condition);
    g_mutex_unlock(&cancellation->mutex);
    g_thread_join(cancellation->thread);
    cancellation->thread = NULL;
    g_clear_object(&cancellation->cancellable);
    g_cond_clear(&cancellation->condition);
    g_mutex_clear(&cancellation->mutex);
  }
  /* Scopes are cleared on the operation thread, never on the watcher. */
  network_sidebar_amneziawg_process_dispatch_termination();
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(DeadlineCancellation, deadline_cancellation_clear)

static int
open_resolvconf(void)
{
  static const char *const paths[] = {
    "/usr/bin/resolvconf",
    "/usr/sbin/resolvconf",
    "/bin/resolvconf",
    "/sbin/resolvconf",
  };

  if (AWG_RESOLVCONF_PATH[0] != '\0')
    return awg_executable_open(AWG_RESOLVCONF_PATH, TRUE);
  for (gsize i = 0; i < G_N_ELEMENTS(paths); i++) {
    int fd = awg_executable_open(paths[i], FALSE);

    if (fd >= 0)
      return fd;
  }
  return -1;
}

/* Returns 0 for success, 1 for a tool failure, and -1 for execution failure. */
static int
run_resolvconf(const char *key,
               gboolean apply,
               const guint8 *input,
               gsize input_length,
               gint64 outer_deadline)
{
  const char *const apply_arguments[] = { "resolvconf", "-a", key, "-m", "0", "-x", NULL };
  const char *const delete_arguments[] = { "resolvconf", "-d", key, "-f", NULL };
  AwgSubprocessRequest request = {
    .executable = open_resolvconf(),
    .argv = apply ? apply_arguments : delete_arguments,
    .input = input,
    .input_length = input_length,
    .timeout_msec = RESOLVCONF_TIMEOUT_MSEC,
    .termination_grace_msec = RESOLVCONF_TERMINATION_GRACE_MSEC,
    .completion = AWG_SUBPROCESS_WAIT_FOR_EOF,
  };
  AwgSubprocessResult result;

  if (request.executable < 0)
    return -1;
  result = awg_subprocess_run(&request, outer_deadline, NULL, NULL);
  close(request.executable);
  if (result.status != AWG_SUBPROCESS_EXITED || !WIFEXITED(result.wait_status) ||
      WEXITSTATUS(result.wait_status) == 127)
    return -1;
  return WEXITSTATUS(result.wait_status) == 0 ? 0 : 1;
}

static gboolean
resolved_operation_deadline(gint64 outer_deadline,
                            gint64 *operation_deadline)
{
  gint64 now = network_sidebar_amneziawg_process_monotonic_msec();

  if (now < 0 || now >= outer_deadline)
    return FALSE;
  *operation_deadline = network_sidebar_amneziawg_deadline_cap(
    now,
    outer_deadline,
    NETWORK_SIDEBAR_AMNEZIAWG_RESOLVED_CALL_TIMEOUT_MSEC);
  return *operation_deadline > now;
}

static GDBusConnection *
open_system_bus(gint64 deadline, GCancellable *cancellable)
{
  g_autoptr(GError) error = NULL;
  struct stat status;
  GDBusConnection *connection;
  gint64 now;

  if (lstat("/run/dbus/system_bus_socket", &status) != 0 ||
      !S_ISSOCK(status.st_mode) || status.st_uid != 0 || status.st_gid != 0)
    return NULL;
  now = network_sidebar_amneziawg_process_monotonic_msec();
  if (now < 0 || now >= deadline ||
      g_cancellable_is_cancelled(cancellable) ||
      network_sidebar_amneziawg_process_termination_requested())
    return NULL;
  connection = g_dbus_connection_new_for_address_sync(
    "unix:path=/run/dbus/system_bus_socket",
    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
      G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
    NULL,
    cancellable,
    &error);

  if (connection != NULL)
    g_dbus_connection_set_exit_on_close(connection, FALSE);
  return connection;
}

typedef enum {
  RESOLVED_OWNER_ERROR,
  RESOLVED_OWNER_NO,
  RESOLVED_OWNER_YES,
} ResolvedOwner;

static ResolvedOwner
resolved_owner(GDBusConnection *connection,
                char **owner,
                gint64 deadline,
                GCancellable *cancellable)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  gint timeout = network_sidebar_amneziawg_deadline_remaining_msec(
    network_sidebar_amneziawg_process_monotonic_msec(),
    deadline,
    NETWORK_SIDEBAR_AMNEZIAWG_RESOLVED_CALL_TIMEOUT_MSEC);

  if (owner != NULL)
    *owner = NULL;
  if (timeout == 0 || g_cancellable_is_cancelled(cancellable) ||
      network_sidebar_amneziawg_process_termination_requested())
    return RESOLVED_OWNER_ERROR;
  reply = g_dbus_connection_call_sync(
    connection,
    "org.freedesktop.DBus",
    "/org/freedesktop/DBus",
    "org.freedesktop.DBus",
    "GetNameOwner",
    g_variant_new("(s)", "org.freedesktop.resolve1"),
    G_VARIANT_TYPE("(s)"),
    G_DBUS_CALL_FLAGS_NO_AUTO_START,
    timeout,
    cancellable,
    &error);
  const char *unique_name;

  if (g_cancellable_is_cancelled(cancellable) ||
      network_sidebar_amneziawg_process_termination_requested())
    return RESOLVED_OWNER_ERROR;
  if (reply == NULL) {
    g_autofree char *remote = error != NULL ?
      g_dbus_error_get_remote_error(error) : NULL;

    return g_strcmp0(remote, "org.freedesktop.DBus.Error.NameHasNoOwner") == 0 ?
      RESOLVED_OWNER_NO : RESOLVED_OWNER_ERROR;
  }
  g_variant_get(reply, "(&s)", &unique_name);
  if (!g_dbus_is_unique_name(unique_name))
    return RESOLVED_OWNER_ERROR;
  if (owner != NULL)
    *owner = g_strdup(unique_name);
  return RESOLVED_OWNER_YES;
}

typedef struct {
  GDBusConnection *connection;
  const char *owner;
  NetworkSidebarAmneziaWGLinkGuard *guard;
  GCancellable *cancellable;
  gint64 deadline;
  gboolean may_have_changed;
} ResolvedOperation;

static gboolean
resolved_target_is_valid(ResolvedOperation *operation)
{
  g_autofree char *owner = NULL;
  ResolvedOwner owner_state = resolved_owner(operation->connection,
                                              &owner,
                                              operation->deadline,
                                              operation->cancellable);
  /* Drain the guard even when the bus check fails. The synchronous call above
   * may have allowed link changes to accumulate. */
  gboolean link_valid = network_sidebar_amneziawg_link_guard_check(
    operation->guard, operation->deadline, operation->cancellable);

  return owner_state == RESOLVED_OWNER_YES &&
         g_strcmp0(owner, operation->owner) == 0 && link_valid;
}

static DnsChangeResult
resolved_call(ResolvedOperation *operation,
              const char *method,
              GVariant *parameters)
{
  g_autoptr(GVariant) arguments = g_variant_ref_sink(parameters);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  g_autofree char *remote = NULL;
  gint timeout;

  if (!resolved_target_is_valid(operation))
    return DNS_CHANGE_UNCERTAIN;
  if (g_cancellable_is_cancelled(operation->cancellable) ||
      network_sidebar_amneziawg_process_termination_requested())
    return operation->may_have_changed ? DNS_CHANGE_UNCERTAIN :
                                         DNS_CHANGE_FAILED;
  timeout = network_sidebar_amneziawg_deadline_remaining_msec(
    network_sidebar_amneziawg_process_monotonic_msec(),
    operation->deadline,
    NETWORK_SIDEBAR_AMNEZIAWG_RESOLVED_CALL_TIMEOUT_MSEC);
  if (timeout == 0)
    return DNS_CHANGE_FAILED;

  operation->may_have_changed = TRUE;
  reply = g_dbus_connection_call_sync(
    operation->connection,
    operation->owner,
    "/org/freedesktop/resolve1",
    "org.freedesktop.resolve1.Manager",
    method,
    arguments,
    G_VARIANT_TYPE_UNIT,
    G_DBUS_CALL_FLAGS_NO_AUTO_START,
    timeout,
    operation->cancellable,
    &error);
  if (!resolved_target_is_valid(operation) ||
      g_cancellable_is_cancelled(operation->cancellable) ||
      network_sidebar_amneziawg_process_termination_requested())
    return DNS_CHANGE_UNCERTAIN;
  if (reply != NULL)
    return DNS_CHANGE_OK;

  remote = error != NULL ? g_dbus_error_get_remote_error(error) : NULL;
  /* Only definitive method rejections allow guarded rollback. A lost reply,
   * including cancellation or a timeout, cannot undo a mutation already sent. */
  if (remote != NULL &&
      ((g_str_has_prefix(remote, "org.freedesktop.resolve1.") &&
        strcmp(remote, "org.freedesktop.resolve1.NoSuchLink") != 0) ||
       strcmp(remote, "org.freedesktop.DBus.Error.InvalidArgs") == 0 ||
       strcmp(remote, "org.freedesktop.DBus.Error.AccessDenied") == 0 ||
       strcmp(remote, "org.freedesktop.DBus.Error.UnknownMethod") == 0 ||
       strcmp(remote, "org.freedesktop.DBus.Error.Failed") == 0 ||
       strcmp(remote, "org.freedesktop.DBus.Error.NoMemory") == 0 ||
       strcmp(remote, "org.freedesktop.DBus.Error.LimitsExceeded") == 0 ||
       strcmp(remote, "org.freedesktop.DBus.Error.NotSupported") == 0))
    return DNS_CHANGE_FAILED;
  return DNS_CHANGE_UNCERTAIN;
}

static DnsChangeResult
resolved_apply_connection(ResolvedOperation *operation,
                          guint ifindex,
                          const NetworkSidebarAmneziaWGDnsConfig *dns)
{
  GVariantBuilder servers;
  GVariantBuilder domains;
  DnsChangeResult result;

  g_variant_builder_init(&servers, G_VARIANT_TYPE("a(iay)"));
  for (guint i = 0; i < dns->servers->len; i++) {
    const NetworkSidebarAmneziaWGDnsServer *server = &g_array_index(
      dns->servers, NetworkSidebarAmneziaWGDnsServer, i);
    GVariant *address = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                                   server->address,
                                                   server->length,
                                                   sizeof(guint8));

    g_variant_builder_add(&servers, "(i@ay)", server->family, address);
  }
  result = resolved_call(operation,
                          "SetLinkDNS",
                          g_variant_new("(i@a(iay))",
                                        (gint) ifindex,
                                        g_variant_builder_end(&servers)));
  if (result != DNS_CHANGE_OK)
    return result;

  g_variant_builder_init(&domains, G_VARIANT_TYPE("a(sb)"));
  for (guint i = 0; i < dns->domains->len; i++) {
    g_variant_builder_add(&domains,
                          "(sb)",
                          (const char *) g_ptr_array_index(dns->domains, i),
                          FALSE);
  }
  g_variant_builder_add(&domains, "(sb)", ".", TRUE);
  result = resolved_call(operation,
                          "SetLinkDomains",
                          g_variant_new("(i@a(sb))",
                                        (gint) ifindex,
                                        g_variant_builder_end(&domains)));
  if (result != DNS_CHANGE_OK)
    return result;
  return resolved_call(operation,
                        "SetLinkDefaultRoute",
                        g_variant_new("(ib)", (gint) ifindex, TRUE));
}

static DnsChangeOutcome
resolved_change(const NetworkSidebarAmneziaWGLinkIdentity *identity,
                 const NetworkSidebarAmneziaWGDnsConfig *dns,
                 gint64 deadline)
{
  g_auto(DeadlineCancellation) cancellation = { 0 };
  g_autoptr(GDBusConnection) connection = NULL;
  g_autofree char *owner = NULL;
  g_autoptr(NetworkSidebarAmneziaWGLinkGuard) guard = NULL;
  ResolvedOperation operation = { 0 };
  DnsChangeResult result = DNS_CHANGE_UNAVAILABLE;

  if (!resolved_operation_deadline(deadline, &operation.deadline) ||
      !deadline_cancellation_init(&cancellation, operation.deadline))
    return (DnsChangeOutcome) { DNS_CHANGE_FAILED, FALSE };
  operation.cancellable = cancellation.cancellable;
  connection = open_system_bus(operation.deadline, operation.cancellable);
  if (connection == NULL ||
      resolved_owner(connection, &owner, operation.deadline,
                       operation.cancellable) !=
        RESOLVED_OWNER_YES)
    goto out;
  result = DNS_CHANGE_UNCERTAIN;
  guard = network_sidebar_amneziawg_link_guard_new(identity,
                                                    operation.deadline,
                                                    operation.cancellable);
  if (guard == NULL)
    goto out;
  operation.connection = connection;
  operation.owner = owner;
  operation.guard = guard;
  result = dns != NULL ?
    resolved_apply_connection(&operation, identity->ifindex, dns) :
    resolved_call(&operation,
                    "RevertLink",
                    g_variant_new("(i)", (gint) identity->ifindex));

out:
  if (!operation.may_have_changed &&
      (g_cancellable_is_cancelled(operation.cancellable) ||
       network_sidebar_amneziawg_process_termination_requested()))
    result = DNS_CHANGE_FAILED;
  return (DnsChangeOutcome) { result, operation.may_have_changed };
}

static gboolean
make_resolvconf_key(const char *name, char *key, gsize size)
{
  int written = snprintf(key, size, "nm-sidebar.%s", name);

  return written > 0 && (gsize) written < size;
}

static DnsChangeResult
resolvconf_apply(const char *name,
                 const NetworkSidebarAmneziaWGDnsConfig *dns,
                 gint64 deadline)
{
  char key[sizeof("nm-sidebar.") +
           NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH];
  GString *contents = g_string_sized_new(128);
  int status;

  if (!make_resolvconf_key(name, key, sizeof(key))) {
    g_string_free(contents, TRUE);
    return DNS_CHANGE_FAILED;
  }
  for (guint i = 0; i < dns->servers->len; i++) {
    const NetworkSidebarAmneziaWGDnsServer *server = &g_array_index(
      dns->servers, NetworkSidebarAmneziaWGDnsServer, i);
    char address[INET6_ADDRSTRLEN];

    if (inet_ntop(server->family,
                  server->address,
                  address,
                  sizeof(address)) == NULL) {
      awg_helper_wipe_bytes(contents->str, contents->allocated_len);
      g_string_free(contents, TRUE);
      return DNS_CHANGE_FAILED;
    }
    g_string_append_printf(contents, "nameserver %s\n", address);
    awg_helper_wipe_bytes(address, sizeof(address));
  }
  if (dns->domains->len > 0) {
    g_string_append(contents, "search");
    for (guint i = 0; i < dns->domains->len; i++) {
      g_string_append_c(contents, ' ');
      g_string_append(contents, g_ptr_array_index(dns->domains, i));
    }
    g_string_append_c(contents, '\n');
  }

  status = run_resolvconf(key,
                          TRUE,
                          (const guint8 *) contents->str,
                          contents->len,
                          deadline);
  awg_helper_wipe_bytes(contents->str, contents->allocated_len);
  g_string_free(contents, TRUE);
  return status == 0 ? DNS_CHANGE_OK : DNS_CHANGE_FAILED;
}

static gboolean
resolvconf_revert(const char *name, gint64 deadline)
{
  char key[sizeof("nm-sidebar.") +
           NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH];

  return make_resolvconf_key(name, key, sizeof(key)) &&
         run_resolvconf(key, FALSE, NULL, 0, deadline) == 0;
}

gboolean
dns_config_present(const NetworkSidebarAmneziaWGDnsConfig *dns)
{
  return dns->servers->len > 0 || dns->domains->len > 0;
}

DnsChangeResult
dns_select_backend(const NetworkSidebarAmneziaWGDnsConfig *dns,
                   NetworkSidebarAmneziaWGDnsBackend *backend,
                   gint64 deadline)
{
  g_auto(DeadlineCancellation) cancellation = { 0 };
  g_autoptr(GDBusConnection) connection = NULL;
  ResolvedOwner owner;
  gint64 operation_deadline;
  int executable;

  if (!dns_config_present(dns)) {
    *backend = NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_NONE;
    return DNS_CHANGE_OK;
  }
  if (!resolved_operation_deadline(deadline, &operation_deadline) ||
      !deadline_cancellation_init(&cancellation, operation_deadline))
    return DNS_CHANGE_FAILED;
  connection = open_system_bus(operation_deadline, cancellation.cancellable);
  if (connection == NULL)
    return DNS_CHANGE_FAILED;
  owner = resolved_owner(connection, NULL, operation_deadline,
                           cancellation.cancellable);
  if (owner == RESOLVED_OWNER_ERROR)
    return DNS_CHANGE_FAILED;
  if (owner == RESOLVED_OWNER_YES) {
    *backend = NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_RESOLVED;
    return DNS_CHANGE_OK;
  }

  executable = open_resolvconf();
  if (executable < 0)
    return DNS_CHANGE_UNAVAILABLE;
  close(executable);
  *backend = NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_RESOLVCONF;
  return DNS_CHANGE_OK;
}

DnsChangeOutcome
dns_apply_selected(const NetworkSidebarAmneziaWGLinkIdentity *identity,
                   const NetworkSidebarAmneziaWGDnsConfig *dns,
                   NetworkSidebarAmneziaWGDnsBackend backend,
                   gint64 deadline)
{
  if (backend == NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_NONE)
    return (DnsChangeOutcome) { DNS_CHANGE_OK, FALSE };
  if (dns == NULL || dns->servers == NULL || dns->domains == NULL)
    return (DnsChangeOutcome) { DNS_CHANGE_FAILED, FALSE };
  if (identity == NULL ||
      !network_sidebar_amneziawg_name_is_valid(identity->name))
    return (DnsChangeOutcome) { DNS_CHANGE_UNCERTAIN, FALSE };
  if (backend == NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_RESOLVCONF)
    return (DnsChangeOutcome) {
      resolvconf_apply(identity->name, dns, deadline), TRUE,
    };
  if (backend != NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_RESOLVED)
    return (DnsChangeOutcome) { DNS_CHANGE_FAILED, FALSE };
  return resolved_change(identity, dns, deadline);
}

DnsChangeOutcome
dns_revert_selected(const NetworkSidebarAmneziaWGLinkIdentity *identity,
                    NetworkSidebarAmneziaWGDnsBackend backend,
                    gint64 deadline)
{
  if (backend == NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_NONE)
    return (DnsChangeOutcome) { DNS_CHANGE_OK, FALSE };
  if (identity == NULL ||
      !network_sidebar_amneziawg_name_is_valid(identity->name))
    return (DnsChangeOutcome) { DNS_CHANGE_UNCERTAIN, FALSE };
  if (backend == NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_RESOLVCONF)
    return (DnsChangeOutcome) {
      resolvconf_revert(identity->name, deadline) ? DNS_CHANGE_OK :
                                                   DNS_CHANGE_FAILED,
      TRUE,
    };
  if (backend != NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_RESOLVED)
    return (DnsChangeOutcome) { DNS_CHANGE_FAILED, FALSE };
  return resolved_change(identity, NULL, deadline);
}
