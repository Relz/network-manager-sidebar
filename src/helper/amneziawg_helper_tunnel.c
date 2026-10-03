#define _GNU_SOURCE

#include "helper/amneziawg_helper_tunnel.h"

#include "helper/amneziawg_dns.h"
#include "helper/amneziawg_firewall.h"
#include "helper/amneziawg_helper_quick.h"
#include "helper/amneziawg_helper_session.h"
#include "helper/amneziawg_helper_util.h"
#include "helper/amneziawg_process.h"

#include <errno.h>
#include <limits.h>
#include <sys/random.h>
#include <unistd.h>

struct _AwgHelperTunnel {
  AwgHelperStorage *storage;
  gboolean dns_recovery_blocked;
};

struct _AwgHelperActivation {
  char name[NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH + 1];
  char config_name[NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH + 6];
  char snapshot_path[PATH_MAX];
  AwgHelperPreparedProfile *profile;
  NetworkSidebarAmneziaWGRuntimeMarker journal;
  gboolean consumed;
};

typedef enum {
  AWG_DNS_TRANSACTION_OK,
  AWG_DNS_TRANSACTION_CHANGE_FAILED,
  AWG_DNS_TRANSACTION_UNCERTAIN,
  AWG_DNS_TRANSACTION_JOURNAL_FAILED,
} AwgDnsTransactionResult;

typedef enum {
  AWG_INTERFACE_TEARDOWN_ABSENT,
  AWG_INTERFACE_TEARDOWN_REMAINS,
  AWG_INTERFACE_TEARDOWN_CONFLICT,
  AWG_INTERFACE_TEARDOWN_UNKNOWN,
  AWG_INTERFACE_TEARDOWN_JOURNAL_FAILED,
} AwgInterfaceTeardownState;

typedef struct {
  AwgInterfaceTeardownState state;
  NetworkSidebarAmneziaWGHelperExit command_result;
  AwgQuickExecution execution;
} AwgInterfaceTeardown;

AwgHelperTunnel *
awg_helper_tunnel_new(AwgHelperStorage *storage)
{
  AwgHelperTunnel *tunnel = g_new0(AwgHelperTunnel, 1);

  tunnel->storage = storage;
  return tunnel;
}

void
awg_helper_tunnel_free(AwgHelperTunnel *tunnel)
{
  g_free(tunnel);
}

static gboolean
generate_link_token(
  char token[NETWORK_SIDEBAR_AMNEZIAWG_LINK_TOKEN_LENGTH + 1])
{
  static const char hex[] = "0123456789abcdef";
  guint8 random_bytes[NETWORK_SIDEBAR_AMNEZIAWG_LINK_TOKEN_LENGTH / 2];
  gsize offset = 0;

  while (offset < sizeof(random_bytes)) {
    ssize_t count = getrandom(random_bytes + offset,
                              sizeof(random_bytes) - offset,
                              0);

    if (count < 0) {
      if (errno == EINTR)
        continue;
      awg_helper_wipe_bytes(random_bytes, sizeof(random_bytes));
      return FALSE;
    }
    if (count == 0) {
      awg_helper_wipe_bytes(random_bytes, sizeof(random_bytes));
      errno = EIO;
      return FALSE;
    }
    offset += (gsize) count;
  }
  for (gsize i = 0; i < sizeof(random_bytes); i++) {
    token[i * 2] = hex[random_bytes[i] >> 4];
    token[i * 2 + 1] = hex[random_bytes[i] & 0x0f];
  }
  token[NETWORK_SIDEBAR_AMNEZIAWG_LINK_TOKEN_LENGTH] = '\0';
  awg_helper_wipe_bytes(random_bytes, sizeof(random_bytes));
  return TRUE;
}

static AwgDnsTransactionResult
complete_dns_change(AwgHelperTunnel *tunnel,
                    const char *name,
                    NetworkSidebarAmneziaWGRuntimeMarker *journal,
                    NetworkSidebarAmneziaWGJournalDnsState previous_state,
                    NetworkSidebarAmneziaWGJournalDnsState confirmed_state,
                    DnsChangeOutcome outcome)
{
  if (outcome.result == DNS_CHANGE_OK)
    journal->dns_state = confirmed_state;
  else if (!outcome.may_have_changed)
    journal->dns_state = previous_state;
  else
    journal->dns_state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_UNKNOWN;
  if (outcome.result == DNS_CHANGE_UNCERTAIN) {
    tunnel->dns_recovery_blocked = TRUE;
    journal->state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_UNCONFIRMED;
  }
  if (!awg_helper_storage_write_runtime_marker(tunnel->storage, name, journal))
    return AWG_DNS_TRANSACTION_JOURNAL_FAILED;
  if (outcome.result == DNS_CHANGE_UNCERTAIN)
    return AWG_DNS_TRANSACTION_UNCERTAIN;
  return outcome.result == DNS_CHANGE_OK ? AWG_DNS_TRANSACTION_OK :
                                          AWG_DNS_TRANSACTION_CHANGE_FAILED;
}

static AwgDnsTransactionResult
apply_dns_durably(AwgHelperTunnel *tunnel,
                  const char *name,
                  NetworkSidebarAmneziaWGRuntimeMarker *journal,
                  const NetworkSidebarAmneziaWGDnsConfig *dns,
                  gint64 deadline)
{
  const NetworkSidebarAmneziaWGLinkIdentity identity = {
    name, journal->link_token, journal->ifindex,
  };
  NetworkSidebarAmneziaWGJournalDnsState previous_state = journal->dns_state;
  DnsChangeOutcome outcome;

  if (tunnel->dns_recovery_blocked)
    return AWG_DNS_TRANSACTION_UNCERTAIN;
  if (journal->dns_backend == NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_NONE)
    return journal->dns_state ==
             NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_ABSENT ?
      AWG_DNS_TRANSACTION_OK : AWG_DNS_TRANSACTION_JOURNAL_FAILED;
  if (journal->dns_state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_APPLIED)
    return AWG_DNS_TRANSACTION_OK;

  journal->dns_state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_UNKNOWN;
  if (!awg_helper_storage_write_runtime_marker(tunnel->storage,
                                                name,
                                                journal)) {
    journal->dns_state = previous_state;
    return AWG_DNS_TRANSACTION_JOURNAL_FAILED;
  }
  outcome = dns_apply_selected(&identity, dns, journal->dns_backend, deadline);
  return complete_dns_change(tunnel,
                              name,
                              journal,
                              previous_state,
                              NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_APPLIED,
                              outcome);
}

static AwgDnsTransactionResult
revert_dns_durably(AwgHelperTunnel *tunnel,
                   const char *name,
                   NetworkSidebarAmneziaWGRuntimeMarker *journal,
                   gint64 deadline)
{
  const NetworkSidebarAmneziaWGLinkIdentity identity = {
    name, journal->link_token, journal->ifindex,
  };
  NetworkSidebarAmneziaWGJournalDnsState previous_state = journal->dns_state;
  DnsChangeOutcome outcome;

  if (tunnel->dns_recovery_blocked)
    return AWG_DNS_TRANSACTION_UNCERTAIN;
  if (journal->dns_state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_ABSENT)
    return AWG_DNS_TRANSACTION_OK;

  if (journal->dns_backend == NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_RESOLVCONF) {
    AwgHelperOwnedLinkState state = awg_helper_session_owned_link_state(name, journal, NULL);

    if (state == AWG_HELPER_OWNED_LINK_UNKNOWN ||
        state == AWG_HELPER_OWNED_LINK_CONFLICT)
      return complete_dns_change(
        tunnel, name, journal, previous_state, previous_state,
        (DnsChangeOutcome) { DNS_CHANGE_UNCERTAIN, FALSE });
  }
  journal->dns_state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_UNKNOWN;
  if (!awg_helper_storage_write_runtime_marker(tunnel->storage,
                                                name,
                                                journal)) {
    journal->dns_state = previous_state;
    return AWG_DNS_TRANSACTION_JOURNAL_FAILED;
  }
  outcome = dns_revert_selected(&identity, journal->dns_backend, deadline);
  return complete_dns_change(tunnel,
                              name,
                              journal,
                              previous_state,
                              NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_ABSENT,
                              outcome);
}

static NetworkSidebarAmneziaWGHelperExit
abort_before_interface_teardown(
  AwgHelperTunnel *tunnel,
  const char *name,
  NetworkSidebarAmneziaWGRuntimeMarker *journal,
  const NetworkSidebarAmneziaWGDnsConfig *dns,
  gboolean began_active,
  NetworkSidebarAmneziaWGHelperExit failure,
  gint64 deadline)
{
  AwgHelperOwnedLinkState interface_state;
  AwgDnsTransactionResult dns_result;
  NetworkSidebarAmneziaWGHelperExit cleanup_failure = began_active ?
    NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_ROLLBACK_FAILED :
    NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ROLLBACK_FAILED;

  if (tunnel->dns_recovery_blocked)
    return failure;
  interface_state = awg_helper_session_owned_link_state(name, journal, NULL);
  if (interface_state == AWG_HELPER_OWNED_LINK_CONFLICT) {
    journal->state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_UNCONFIRMED;
    awg_helper_storage_write_runtime_marker(tunnel->storage, name, journal);
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT;
  }
  if (interface_state == AWG_HELPER_OWNED_LINK_UNKNOWN)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  if (interface_state == AWG_HELPER_OWNED_LINK_ABSENT)
    return failure;
  if (awg_helper_phase_stop_status(deadline) != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return cleanup_failure;

  journal->state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_REQUIRED;
  if (!awg_helper_storage_write_runtime_marker(tunnel->storage,
                                                name,
                                                journal))
    return cleanup_failure;
  /* Unstarted teardown is retryable, but cleanup/activation rollback must never
   * restore DNS or promote a session to active based only on was_active. */
  if (!began_active)
    return failure;
  dns_result = apply_dns_durably(tunnel, name, journal, dns, deadline);
  if (dns_result != AWG_DNS_TRANSACTION_OK)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_ROLLBACK_FAILED;
  journal->state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_ACTIVE;
  if (!awg_helper_storage_write_runtime_marker(tunnel->storage,
                                                name,
                                                journal))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_ROLLBACK_FAILED;
  return failure;
}

static NetworkSidebarAmneziaWGHelperExit
finalize_runtime_entries(AwgHelperTunnel *tunnel,
                         const char *name,
                         const char *config_name,
                         NetworkSidebarAmneziaWGRuntimeMarker *journal)
{
  char dns_name[NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH + 5];

  if (tunnel->dns_recovery_blocked ||
      journal->state != NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_COMPLETE ||
      journal->dns_state != NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_ABSENT ||
      journal->was_active)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  if (!awg_helper_storage_write_runtime_marker(tunnel->storage,
                                                name,
                                                journal) ||
      !awg_helper_storage_make_dns_filename(name,
                                             dns_name,
                                             sizeof(dns_name)))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  if (!awg_helper_storage_remove_and_sync(tunnel->storage,
                                          AWG_HELPER_STORAGE_RUNTIME_CONFIGS,
                                          config_name) ||
      !awg_helper_storage_remove_and_sync(tunnel->storage,
                                          AWG_HELPER_STORAGE_RUNTIME_CONFIGS,
                                          dns_name))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  return awg_helper_storage_remove_and_sync(tunnel->storage,
                                            AWG_HELPER_STORAGE_RUNTIME,
                                            name) ?
    NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS :
    NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
}

static gboolean
load_runtime_dns(AwgHelperTunnel *tunnel,
                 const char *name,
                 NetworkSidebarAmneziaWGDnsConfig *dns)
{
  char dns_name[NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH + 5];
  guint8 *contents = NULL;
  gsize length = 0;
  gboolean valid;

  if (!awg_helper_storage_make_dns_filename(name,
                                             dns_name,
                                             sizeof(dns_name)) ||
      !awg_helper_storage_read_secure_file(
        tunnel->storage,
        AWG_HELPER_STORAGE_RUNTIME_CONFIGS,
        dns_name,
        0600,
        NETWORK_SIDEBAR_AMNEZIAWG_MAX_DNS_PAYLOAD,
        &contents,
        &length))
    return FALSE;
  valid = network_sidebar_amneziawg_dns_config_parse(contents, length, dns);
  network_sidebar_amneziawg_secret_free(contents, MAX(length, 1u));
  return valid;
}

static NetworkSidebarAmneziaWGHelperExit
discard_unstarted_runtime(AwgHelperTunnel *tunnel,
                          const char *name,
                          const char *config_name,
                          NetworkSidebarAmneziaWGRuntimeMarker *journal,
                          gint64 cleanup_deadline)
{
  NetworkSidebarAmneziaWGHelperExit result;

  /* Only positive evidence that up could not run permits artifact-only cleanup.
   * A pre-start cancellation must not prevent these rollback writes. */
  network_sidebar_amneziawg_process_set_cleanup_mode(TRUE);
  result = awg_helper_phase_stop_status(cleanup_deadline);
  if (result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    journal->state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_COMPLETE;
    journal->dns_state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_ABSENT;
    journal->was_active = FALSE;
    journal->firewall_scope = NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_NONE;
    result = finalize_runtime_entries(tunnel, name, config_name, journal);
  }
  network_sidebar_amneziawg_process_set_cleanup_mode(FALSE);
  return result;
}

static void
include_current_firewall_scope(NetworkSidebarAmneziaWGRuntimeMarker *journal,
                                gint64 deadline)
{
  guint scope;

  /* Failed discovery returns ALL. Never forget a namespace because a tool was
   * removed, replaced, or could no longer be inspected during an operation. */
  awg_firewall_capture_scope(deadline, &scope);
  journal->firewall_scope |= scope;
}

static NetworkSidebarAmneziaWGHelperExit
verify_runtime_cleanup(AwgHelperTunnel *tunnel,
                        const char *name,
                        const char *config_name,
                        NetworkSidebarAmneziaWGRuntimeMarker *journal,
                        gint64 deadline)
{
  AwgHelperOwnedLinkState link;

  if (tunnel->dns_recovery_blocked ||
      journal->dns_state != NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_ABSENT ||
      (journal->state != NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_REQUIRED &&
       journal->state != NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_FIREWALL_PENDING))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  link = awg_helper_session_owned_link_state(name, journal, NULL);
  if (link != AWG_HELPER_OWNED_LINK_ABSENT)
    return link == AWG_HELPER_OWNED_LINK_UNKNOWN ?
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED :
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT;
  /* The caller establishes either successful teardown or an observed early-up
   * failure before routing/DNS changes. Persist that link/DNS cleanup is settled
   * before the restartable firewall-only verification phase. */
  journal->state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_FIREWALL_PENDING;
  journal->was_active = FALSE;
  if (!awg_helper_storage_write_runtime_marker(tunnel->storage, name, journal))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  if (awg_firewall_probe(name, journal->firewall_scope, deadline) != AWG_FIREWALL_ABSENT)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FIREWALL_CLEANUP_FAILED;
  link = awg_helper_session_owned_link_state(name, journal, NULL);
  if (link != AWG_HELPER_OWNED_LINK_ABSENT)
    return link == AWG_HELPER_OWNED_LINK_UNKNOWN ?
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED :
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT;
  if (awg_helper_phase_stop_status(deadline) != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FIREWALL_CLEANUP_FAILED;
  journal->state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_COMPLETE;
  return finalize_runtime_entries(tunnel, name, config_name, journal);
}

static gboolean
try_finalize_early_up_failure(AwgHelperTunnel *tunnel,
                              const char *name,
                              const char *config_name,
                              NetworkSidebarAmneziaWGRuntimeMarker *journal,
                              const AwgQuickResult *quick,
                              gint64 cleanup_deadline,
                              NetworkSidebarAmneziaWGHelperExit *result)
{
  NetworkSidebarAmneziaWGHelperExit cleanup;

  /* Only the just-finished Up can supply this evidence. A missing link in an
   * active/restarted session, or an incomplete/later tool failure, is not proof
   * that routing was never installed. No resolver mutation has happened here. */
  if (!quick->failed_before_routes ||
      quick->status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS ||
      tunnel->dns_recovery_blocked ||
      journal->state != NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_ACTIVATING ||
      journal->was_active || journal->ifindex != 0 ||
      journal->dns_state != NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_ABSENT ||
      awg_helper_session_owned_link_state(name, journal, NULL) !=
        AWG_HELPER_OWNED_LINK_ABSENT)
    return FALSE;

  network_sidebar_amneziawg_process_set_cleanup_mode(TRUE);
  journal->state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_REQUIRED;
  cleanup = verify_runtime_cleanup(tunnel, name, config_name, journal,
                                    cleanup_deadline);
  network_sidebar_amneziawg_process_set_cleanup_mode(FALSE);
  /* Preserve the original activation error after successful artifact cleanup. */
  if (cleanup != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    *result = cleanup;
  return TRUE;
}

static AwgInterfaceTeardown
teardown_owned_interface(AwgHelperTunnel *tunnel,
                         const char *name,
                         const char *snapshot_path,
                         NetworkSidebarAmneziaWGRuntimeMarker *journal,
                         NetworkSidebarAmneziaWGJournalState absent_state,
                         gint64 deadline)
{
  AwgInterfaceTeardown teardown = {
    .state = AWG_INTERFACE_TEARDOWN_UNKNOWN,
    .command_result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS,
    .execution = AWG_QUICK_NOT_STARTED,
  };
  NetworkSidebarAmneziaWGLinkObservation observation = { 0 };
  NetworkSidebarAmneziaWGHelperExit identity_result;
  AwgHelperOwnedLinkState interface_state;
  AwgQuickResult quick;
  gboolean interface_exists;

  identity_result = awg_helper_session_refresh_owned_interface(tunnel->storage,
                                                                name,
                                                                journal,
                                                                &interface_exists);
  if (identity_result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT) {
    teardown.state = AWG_INTERFACE_TEARDOWN_CONFLICT;
    return teardown;
  }
  if (identity_result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return teardown;
  if (!interface_exists) {
    journal->state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_UNCONFIRMED;
    if (!awg_helper_storage_write_runtime_marker(tunnel->storage,
                                                  name,
                                                  journal))
      teardown.state = AWG_INTERFACE_TEARDOWN_JOURNAL_FAILED;
    return teardown;
  }

  journal->state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_UNCONFIRMED;
  if (!awg_helper_storage_write_runtime_marker(tunnel->storage,
                                                name,
                                                journal)) {
    teardown.state = AWG_INTERFACE_TEARDOWN_JOURNAL_FAILED;
    return teardown;
  }
  quick = awg_helper_quick_run("down", snapshot_path, NULL, NULL, deadline);
  teardown.command_result = quick.status;
  teardown.execution = quick.execution;

  interface_state = awg_helper_session_owned_link_state(name, journal, &observation);
  if (interface_state == AWG_HELPER_OWNED_LINK_ABSENT &&
      teardown.command_result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    journal->state = absent_state;
    if (!awg_helper_storage_write_runtime_marker(tunnel->storage,
                                                  name,
                                                  journal)) {
      teardown.state = AWG_INTERFACE_TEARDOWN_JOURNAL_FAILED;
      return teardown;
    }
    teardown.state = AWG_INTERFACE_TEARDOWN_ABSENT;
  } else if (interface_state == AWG_HELPER_OWNED_LINK_PRESENT) {
    teardown.state = AWG_INTERFACE_TEARDOWN_REMAINS;
  } else if (interface_state == AWG_HELPER_OWNED_LINK_CONFLICT) {
    teardown.state = AWG_INTERFACE_TEARDOWN_CONFLICT;
  }
  return teardown;
}

static NetworkSidebarAmneziaWGHelperExit
rollback_activation_impl(AwgHelperTunnel *tunnel,
                         const char *name,
                         const char *config_name,
                         const char *snapshot_path,
                         NetworkSidebarAmneziaWGRuntimeMarker *journal,
                         NetworkSidebarAmneziaWGHelperExit failure,
                         gint64 cleanup_deadline)
{
  AwgDnsTransactionResult dns_result;
  NetworkSidebarAmneziaWGHelperExit cleanup_failure;
  NetworkSidebarAmneziaWGHelperExit identity_result;
  AwgInterfaceTeardown teardown;

  if (tunnel->dns_recovery_blocked)
    return failure;
  journal->state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_REQUIRED;
  if (!awg_helper_storage_write_runtime_marker(tunnel->storage,
                                                name,
                                                journal))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ROLLBACK_FAILED;
  identity_result = awg_helper_session_refresh_owned_interface(tunnel->storage,
                                                                name,
                                                                journal,
                                                                NULL);
  if (identity_result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    journal->state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_UNCONFIRMED;
    awg_helper_storage_write_runtime_marker(tunnel->storage, name, journal);
    return identity_result;
  }
  dns_result = revert_dns_durably(tunnel, name, journal, cleanup_deadline);
  if (tunnel->dns_recovery_blocked)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  teardown = teardown_owned_interface(
    tunnel,
    name,
    snapshot_path,
    journal,
    NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_REQUIRED,
    cleanup_deadline);
  if (teardown.state == AWG_INTERFACE_TEARDOWN_ABSENT) {
    NetworkSidebarAmneziaWGHelperExit removed;

    if (dns_result != AWG_DNS_TRANSACTION_OK)
      return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_ROLLBACK_FAILED;
    removed = verify_runtime_cleanup(tunnel, name, config_name, journal,
                                       cleanup_deadline);
    return removed == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS ? failure :
                                                                 removed;
  }
  if (teardown.state == AWG_INTERFACE_TEARDOWN_CONFLICT)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT;
  if (teardown.state == AWG_INTERFACE_TEARDOWN_JOURNAL_FAILED) {
    cleanup_failure = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ROLLBACK_FAILED;
  } else if (teardown.state == AWG_INTERFACE_TEARDOWN_UNKNOWN) {
    cleanup_failure = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  } else if (teardown.command_result !=
             NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    cleanup_failure = teardown.command_result;
  } else if (dns_result != AWG_DNS_TRANSACTION_OK) {
    cleanup_failure = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_ROLLBACK_FAILED;
  } else {
    cleanup_failure = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ROLLBACK_FAILED;
  }
  if (teardown.state == AWG_INTERFACE_TEARDOWN_REMAINS &&
      teardown.execution == AWG_QUICK_NOT_STARTED)
    return abort_before_interface_teardown(tunnel, name, journal, NULL, FALSE,
                                            cleanup_failure, cleanup_deadline);
  return cleanup_failure;
}

static NetworkSidebarAmneziaWGHelperExit
rollback_activation(AwgHelperTunnel *tunnel,
                    const char *name,
                    const char *config_name,
                    const char *snapshot_path,
                    NetworkSidebarAmneziaWGRuntimeMarker *journal,
                    NetworkSidebarAmneziaWGHelperExit failure,
                    gint64 cleanup_deadline)
{
  NetworkSidebarAmneziaWGHelperExit result;

  network_sidebar_amneziawg_process_set_cleanup_mode(TRUE);
  result = rollback_activation_impl(tunnel,
                                    name,
                                    config_name,
                                    snapshot_path,
                                    journal,
                                    failure,
                                    cleanup_deadline);
  network_sidebar_amneziawg_process_set_cleanup_mode(FALSE);
  return result;
}

void
awg_helper_activation_free(AwgHelperActivation *activation)
{
  if (activation == NULL)
    return;
  awg_helper_prepared_profile_free(activation->profile);
  g_free(activation);
}

NetworkSidebarAmneziaWGHelperExit
awg_helper_tunnel_prepare_up(AwgHelperTunnel *tunnel,
                             AwgHelperProfiles *profiles,
                             const char *name,
                             gint64 deadline,
                             AwgHelperActivation **activation_out)
{
  AwgHelperActivation *activation;
  gboolean marker;
  gboolean interface;
  NetworkSidebarAmneziaWGHelperExit result;
  DnsChangeResult backend_result;
  AwgHelperEntryState config_state;

  *activation_out = NULL;
  if (!network_sidebar_amneziawg_name_is_valid(name))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_NAME;
  result = awg_helper_phase_stop_status(deadline);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  activation = g_new0(AwgHelperActivation, 1);
  g_strlcpy(activation->name, name, sizeof(activation->name));
  activation->journal.state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_ACTIVATING;
  if (!awg_helper_storage_make_config_filename(name,
                                                activation->config_name,
                                                sizeof(activation->config_name)) ||
      !awg_helper_storage_make_runtime_config_path(name,
                                                    activation->snapshot_path,
                                                    sizeof(activation->snapshot_path))) {
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_NAME;
    goto out;
  }
  config_state = awg_helper_storage_secure_file_state(
    tunnel->storage,
    AWG_HELPER_STORAGE_CONFIG,
    activation->config_name,
    0600);
  if (config_state != AWG_HELPER_ENTRY_VALID) {
    result = config_state == AWG_HELPER_ENTRY_MISSING ?
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_NOT_FOUND :
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
    goto out;
  }
  result = awg_helper_session_runtime_state(tunnel->storage,
                                            name,
                                            &marker,
                                            &interface,
                                            NULL);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    goto out;
  if (marker || interface) {
    result = marker ? NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ACTIVE :
                      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT;
    goto out;
  }
  if (!generate_link_token(activation->journal.link_token)) {
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
    goto out;
  }

  result = awg_helper_profiles_prepare_runtime(profiles,
                                               activation->config_name,
                                               deadline,
                                               &activation->profile);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    goto out;
  if (!awg_helper_prepared_profile_add_link_claim(activation->profile,
                                                   activation->journal.link_token) ||
      !awg_helper_prepared_profile_serialize_dns(activation->profile)) {
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
    goto out;
  }
  backend_result = dns_select_backend(
    awg_helper_prepared_profile_dns(activation->profile),
    &activation->journal.dns_backend,
    deadline);
  if (backend_result != DNS_CHANGE_OK) {
    NetworkSidebarAmneziaWGHelperExit dns_failure =
      backend_result == DNS_CHANGE_UNAVAILABLE ?
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_BACKEND_UNAVAILABLE :
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_APPLY_FAILED;

    result = awg_helper_phase_failure_or(deadline, dns_failure);
    goto out;
  }
  if (!awg_firewall_capture_scope(deadline, &activation->journal.firewall_scope)) {
    result = awg_helper_phase_failure_or(
      deadline, NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FIREWALL_FAILED);
    goto out;
  }
  result = awg_helper_phase_stop_status(deadline);
  if (result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    *activation_out = g_steal_pointer(&activation);

out:
  awg_helper_activation_free(activation);
  return result;
}

NetworkSidebarAmneziaWGHelperExit
awg_helper_tunnel_activate(AwgHelperTunnel *tunnel,
                           AwgHelperActivation *activation,
                           const NetworkSidebarAmneziaWGDeadlines *deadlines)
{
  char dns_name[NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH + 5];
  gboolean marker;
  gboolean interface;
  gboolean tunnel_started = FALSE;
  gsize runtime_snapshot_length = 0;
  gsize dns_payload_length = 0;
  const guint8 *runtime_snapshot;
  const guint8 *dns_payload;
  const NetworkSidebarAmneziaWGDnsConfig *dns;
  NetworkSidebarAmneziaWGRuntimeMarker journal;
  NetworkSidebarAmneziaWGHelperExit result;
  AwgQuickResult quick;
  AwgDnsTransactionResult dns_result;
  const char *name;
  const char *config_name;
  const char *snapshot_path;

  if (activation == NULL || activation->consumed)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_USAGE;
  activation->consumed = TRUE;
  name = activation->name;
  config_name = activation->config_name;
  snapshot_path = activation->snapshot_path;
  journal = activation->journal;
  dns = awg_helper_prepared_profile_dns(activation->profile);
  runtime_snapshot = awg_helper_prepared_profile_snapshot(
    activation->profile, &runtime_snapshot_length);
  dns_payload = awg_helper_prepared_profile_dns_payload(
    activation->profile, &dns_payload_length);
  result = awg_helper_phase_stop_status(deadlines->forward_at);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  /* Preparation may precede several disconnects. Never overwrite a runtime
   * session or same-name interface that appeared in the meantime. */
  result = awg_helper_session_runtime_state(tunnel->storage, name,
                                            &marker, &interface, NULL);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  if (marker || interface)
    return marker ? NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ACTIVE :
                    NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT;
  if (!awg_helper_storage_make_dns_filename(name,
                                              dns_name,
                                              sizeof(dns_name)))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  if (!awg_helper_storage_atomic_write(tunnel->storage,
                                       AWG_HELPER_STORAGE_RUNTIME_CONFIGS,
                                       config_name,
                                       runtime_snapshot,
                                       runtime_snapshot_length,
                                       0600) ||
      !awg_helper_storage_atomic_write(tunnel->storage,
                                       AWG_HELPER_STORAGE_RUNTIME_CONFIGS,
                                       dns_name,
                                       dns_payload,
                                       dns_payload_length,
                                       0600) ||
      !awg_helper_storage_write_runtime_marker(tunnel->storage,
                                                name,
                                                &journal)) {
    NetworkSidebarAmneziaWGHelperExit cleanup = discard_unstarted_runtime(
      tunnel, name, config_name, &journal, deadlines->cleanup_at);

    result = cleanup == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS ?
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED :
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ROLLBACK_FAILED;
    goto out;
  }

  quick = awg_helper_quick_run("up",
                               snapshot_path,
                               NULL,
                               NULL,
                               deadlines->forward_at);
  result = quick.status;
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS &&
      quick.execution == AWG_QUICK_NOT_STARTED) {
    NetworkSidebarAmneziaWGHelperExit cleanup = discard_unstarted_runtime(
      tunnel, name, config_name, &journal, deadlines->cleanup_at);

    if (cleanup != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
      result = cleanup;
    goto out;
  }
  include_current_firewall_scope(&journal, deadlines->forward_at);
  if (!awg_helper_storage_write_runtime_marker(tunnel->storage, name, &journal))
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    if (try_finalize_early_up_failure(tunnel, name, config_name, &journal,
                                      &quick, deadlines->cleanup_at, &result))
      goto out;
    result = rollback_activation(tunnel,
                                 name,
                                 config_name,
                                 snapshot_path,
                                 &journal,
                                 result,
                                 deadlines->cleanup_at);
    goto out;
  }
  tunnel_started = TRUE;
  result = awg_helper_phase_stop_status(deadlines->forward_at);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    result = rollback_activation(tunnel,
                                 name,
                                 config_name,
                                 snapshot_path,
                                 &journal,
                                 result,
                                 deadlines->cleanup_at);
    goto out;
  }
  result = awg_helper_session_refresh_owned_interface(tunnel->storage,
                                                       name,
                                                       &journal,
                                                       &interface);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS || !interface) {
    result = rollback_activation(
      tunnel,
      name,
      config_name,
      snapshot_path,
      &journal,
      result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS ?
        NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED : result,
      deadlines->cleanup_at);
    goto out;
  }
  result = awg_helper_phase_stop_status(deadlines->forward_at);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    result = rollback_activation(tunnel,
                                 name,
                                 config_name,
                                 snapshot_path,
                                 &journal,
                                 result,
                                 deadlines->cleanup_at);
    goto out;
  }

  result = awg_helper_phase_stop_status(deadlines->forward_at);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    result = rollback_activation(tunnel,
                                 name,
                                 config_name,
                                 snapshot_path,
                                 &journal,
                                 result,
                                 deadlines->cleanup_at);
    goto out;
  }
  dns_result = apply_dns_durably(tunnel,
                                 name,
                                 &journal,
                                 dns,
                                 deadlines->forward_at);
  if (dns_result != AWG_DNS_TRANSACTION_OK) {
    NetworkSidebarAmneziaWGHelperExit dns_failure =
      dns_result == AWG_DNS_TRANSACTION_CHANGE_FAILED ?
        awg_helper_phase_failure_or(
          deadlines->forward_at,
          NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_APPLY_FAILED) :
        NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;

    result = rollback_activation(tunnel,
                                 name,
                                 config_name,
                                 snapshot_path,
                                 &journal,
                                 dns_failure,
                                 deadlines->cleanup_at);
    goto out;
  }
  result = awg_helper_phase_stop_status(deadlines->forward_at);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    result = rollback_activation(tunnel,
                                 name,
                                 config_name,
                                 snapshot_path,
                                 &journal,
                                 result,
                                 deadlines->cleanup_at);
    goto out;
  }
  journal.state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_ACTIVE;
  journal.was_active = TRUE;
  if (!awg_helper_storage_write_runtime_marker(tunnel->storage,
                                                name,
                                                &journal)) {
    journal.state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_ACTIVATING;
    journal.was_active = FALSE;
    result = rollback_activation(tunnel,
                                 name,
                                 config_name,
                                 snapshot_path,
                                 &journal,
                                 NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED,
                                 deadlines->cleanup_at);
    goto out;
  }
  result = awg_helper_phase_stop_status(deadlines->forward_at);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    result = rollback_activation(tunnel,
                                 name,
                                 config_name,
                                 snapshot_path,
                                 &journal,
                                 result,
                                 deadlines->cleanup_at);
    goto out;
  }
  result = awg_helper_session_runtime_state(tunnel->storage,
                                            name,
                                            &marker,
                                            &interface,
                                            NULL);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS || !marker ||
      !interface)
    result = rollback_activation(
      tunnel,
      name,
      config_name,
      snapshot_path,
      &journal,
      result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS ?
        NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED : result,
      deadlines->cleanup_at);

out:
  if (tunnel_started && result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    NetworkSidebarAmneziaWGHelperExit stop = awg_helper_phase_stop_status(
      deadlines->forward_at);

    if (stop != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
      result = rollback_activation(tunnel,
                                   name,
                                   config_name,
                                   snapshot_path,
                                   &journal,
                                   stop,
                                   deadlines->cleanup_at);
  }
  if (result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    if (!network_sidebar_amneziawg_process_release_retained_group(
          deadlines->forward_at, deadlines->cleanup_at)) {
      NetworkSidebarAmneziaWGHelperExit release_failure =
        awg_helper_phase_stop_status(deadlines->forward_at);

      if (release_failure == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
        release_failure = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SIGNALED;
      result = rollback_activation(tunnel,
                                   name,
                                   config_name,
                                   snapshot_path,
                                   &journal,
                                   release_failure,
                                   deadlines->cleanup_at);
      if (!network_sidebar_amneziawg_process_terminate_retained_group(
            deadlines->cleanup_at))
        result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ROLLBACK_FAILED;
    }
  } else if (!network_sidebar_amneziawg_process_terminate_retained_group(
               deadlines->cleanup_at)) {
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ROLLBACK_FAILED;
  }
  return result;
}

NetworkSidebarAmneziaWGHelperExit
awg_helper_tunnel_check_down(AwgHelperTunnel *tunnel,
                             const char *name,
                             gint64 deadline)
{
  char config_name[NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH + 6];
  gboolean marker;
  gboolean interface;
  NetworkSidebarAmneziaWGRuntimeMarker journal = { 0 };
  NetworkSidebarAmneziaWGDnsConfig dns = { 0 };
  NetworkSidebarAmneziaWGHelperExit result = awg_helper_phase_stop_status(deadline);

  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  if (!network_sidebar_amneziawg_name_is_valid(name) ||
      !awg_helper_storage_make_config_filename(name, config_name, sizeof(config_name)))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_NAME;
  result = awg_helper_session_runtime_state(tunnel->storage, name,
                                            &marker, &interface, &journal);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  if (!marker || journal.state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_COMPLETE ||
      journal.state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_FIREWALL_PENDING)
    return interface ? NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT :
                       NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
  if (!interface ||
      (journal.state != NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_ACTIVE &&
       journal.state != NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_ACTIVATING &&
       journal.state != NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_REQUIRED) ||
      (journal.dns_backend == NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_RESOLVED &&
       journal.dns_state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_UNKNOWN))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  if (!load_runtime_dns(tunnel, name, &dns) ||
      awg_helper_storage_secure_file_state(tunnel->storage,
                                            AWG_HELPER_STORAGE_RUNTIME_CONFIGS,
                                            config_name, 0600) != AWG_HELPER_ENTRY_VALID ||
      ((journal.dns_backend == NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_NONE) !=
       !dns_config_present(&dns)))
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  network_sidebar_amneziawg_dns_config_clear(&dns);
  return awg_helper_phase_failure_or(deadline, result);
}

NetworkSidebarAmneziaWGHelperExit
awg_helper_tunnel_down(AwgHelperTunnel *tunnel,
                       const char *name,
                       const char *config_name,
                       const char *snapshot_path,
                       const NetworkSidebarAmneziaWGDeadlines *deadlines)
{
  gboolean marker;
  gboolean interface;
  gboolean began_active;
  NetworkSidebarAmneziaWGDnsConfig dns = { 0 };
  NetworkSidebarAmneziaWGRuntimeMarker runtime_marker = { 0 };
  NetworkSidebarAmneziaWGHelperExit result;
  AwgDnsTransactionResult dns_result;
  AwgInterfaceTeardown teardown;

  result = awg_helper_phase_stop_status(deadlines->forward_at);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  result = awg_helper_session_runtime_state(tunnel->storage,
                                            name,
                                            &marker,
                                            &interface,
                                            &runtime_marker);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  if (!marker)
    return interface ? NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT :
                       NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
  if (runtime_marker.state ==
      NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_COMPLETE) {
    if (interface)
      return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT;
    return finalize_runtime_entries(tunnel,
                                    name,
                                    config_name,
                                    &runtime_marker);
  }
  if (runtime_marker.state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_FIREWALL_PENDING) {
    if (interface)
      return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT;
    return verify_runtime_cleanup(tunnel, name, config_name, &runtime_marker,
                                    deadlines->forward_at);
  }
  if (runtime_marker.state ==
      NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_UNCONFIRMED)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  /* A new worker has no continuity evidence for a previously interrupted
   * resolved mutation. Even a matching link or NoSuchLink cannot settle it. */
  if (runtime_marker.dns_backend == NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_RESOLVED &&
      runtime_marker.dns_state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_UNKNOWN) {
    tunnel->dns_recovery_blocked = TRUE;
    runtime_marker.state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_UNCONFIRMED;
    awg_helper_storage_write_runtime_marker(tunnel->storage, name, &runtime_marker);
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  }
  if (interface && runtime_marker.ifindex == 0) {
    result = awg_helper_session_refresh_owned_interface(tunnel->storage,
                                                         name,
                                                         &runtime_marker,
                                                         &interface);
    if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
      return result;
    if (!interface)
      return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  }

  if (!load_runtime_dns(tunnel, name, &dns)) {
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
    goto out;
  }
  if (awg_helper_storage_secure_file_state(
        tunnel->storage,
        AWG_HELPER_STORAGE_RUNTIME_CONFIGS,
        config_name,
        0600) != AWG_HELPER_ENTRY_VALID) {
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
    goto out;
  }
  if ((runtime_marker.dns_backend ==
       NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_NONE) !=
      !dns_config_present(&dns)) {
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
    goto out;
  }
  /* was_active is history, not proof that a cleanup retry is still active. */
  began_active = runtime_marker.state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_ACTIVE;
  include_current_firewall_scope(&runtime_marker, deadlines->forward_at);
  runtime_marker.state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_REQUIRED;
  if (!awg_helper_storage_write_runtime_marker(tunnel->storage,
                                                name,
                                                &runtime_marker)) {
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
    goto out;
  }
  dns_result = revert_dns_durably(tunnel,
                                  name,
                                  &runtime_marker,
                                  deadlines->forward_at);
  if (tunnel->dns_recovery_blocked) {
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
    goto out;
  }
  if (dns_result != AWG_DNS_TRANSACTION_OK) {
    result = dns_result == AWG_DNS_TRANSACTION_CHANGE_FAILED ?
      awg_helper_phase_failure_or(
        deadlines->forward_at,
        NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_REVERT_FAILED) :
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
    goto abort_before_teardown;
  }
  teardown = teardown_owned_interface(
    tunnel,
    name,
    snapshot_path,
    &runtime_marker,
    NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_REQUIRED,
    deadlines->forward_at);
  if (teardown.state == AWG_INTERFACE_TEARDOWN_ABSENT) {
    result = verify_runtime_cleanup(tunnel,
                                      name,
                                      config_name,
                                      &runtime_marker,
                                      deadlines->forward_at);
    goto out;
  }
  if (teardown.state == AWG_INTERFACE_TEARDOWN_CONFLICT) {
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT;
    goto out;
  }
  if (teardown.state == AWG_INTERFACE_TEARDOWN_JOURNAL_FAILED)
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ROLLBACK_FAILED;
  else if (teardown.state == AWG_INTERFACE_TEARDOWN_UNKNOWN)
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  else if (teardown.command_result !=
           NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    result = teardown.command_result;
  else
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  if (teardown.state == AWG_INTERFACE_TEARDOWN_REMAINS &&
      teardown.execution == AWG_QUICK_NOT_STARTED)
    goto abort_before_teardown;
  /* A surviving link cannot prove that down left routing and firewall intact. */
  goto out;

abort_before_teardown:
  network_sidebar_amneziawg_process_set_cleanup_mode(TRUE);
  result = abort_before_interface_teardown(tunnel,
                                           name,
                                           &runtime_marker,
                                           &dns,
                                           began_active,
                                           result,
                                           deadlines->cleanup_at);
  network_sidebar_amneziawg_process_set_cleanup_mode(FALSE);

out:
  network_sidebar_amneziawg_dns_config_clear(&dns);
  return result;
}
