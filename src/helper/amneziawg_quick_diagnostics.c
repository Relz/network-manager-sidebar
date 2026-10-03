#include "helper/amneziawg_quick_diagnostics.h"

#include "helper/amneziawg_helper_util.h"

#include <string.h>
#include <sys/wait.h>

static gboolean
line_has_prefix(const guint8 *line, gsize length, const char *prefix)
{
  gsize prefix_length = strlen(prefix);

  return length >= prefix_length && memcmp(line, prefix, prefix_length) == 0;
}

static gboolean
line_is_teardown_command(const guint8 *line, gsize length)
{
  return line_has_prefix(line, length, "[#] resolvconf -d ") ||
         line_has_prefix(line, length, "[#] ip -4 rule delete ") ||
         line_has_prefix(line, length, "[#] ip -6 rule delete ") ||
         line_has_prefix(line, length, "[#] ip -4 route delete ") ||
         line_has_prefix(line, length, "[#] ip -6 route delete ") ||
         line_has_prefix(line, length, "[#] ip link delete dev ");
}

static gboolean
line_is_link_claim(const guint8 *line, gsize length)
{
  static const char prefix[] = "[#] ip link set dev ";
  static const char alias[] = " alias " NETWORK_SIDEBAR_AMNEZIAWG_LINK_ALIAS_PREFIX;
  char name[NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH + 1];
  char token[NETWORK_SIDEBAR_AMNEZIAWG_LINK_TOKEN_LENGTH + 1];
  const guint8 *space;
  gsize name_length;

  if (!line_has_prefix(line, length, prefix))
    return FALSE;
  line += sizeof(prefix) - 1;
  length -= sizeof(prefix) - 1;
  space = memchr(line, ' ', length);
  if (space == NULL)
    return FALSE;
  name_length = (gsize) (space - line);
  if (name_length >= sizeof(name) ||
      length - name_length != sizeof(alias) - 1 + sizeof(token) - 1 ||
      memcmp(space, alias, sizeof(alias) - 1) != 0)
    return FALSE;
  memcpy(name, line, name_length);
  name[name_length] = '\0';
  memcpy(token, space + sizeof(alias) - 1, sizeof(token) - 1);
  token[sizeof(token) - 1] = '\0';
  return network_sidebar_amneziawg_name_is_valid(name) &&
         network_sidebar_amneziawg_link_token_is_valid(token);
}

static void
observe_stage(AwgQuickDiagnostics *diagnostics, AwgQuickStage stage)
{
  /* Recovery evidence is sticky and stricter than the user-facing failure
   * label: a later/unknown command must never be hidden by a cleanup message. */
  if (stage >= AWG_QUICK_STAGE_DNS || stage < diagnostics->current_stage ||
      diagnostics->teardown_seen)
    diagnostics->recovery_blocked = TRUE;
  if (stage == AWG_QUICK_STAGE_INTERFACE)
    diagnostics->interface_seen = TRUE;
}

static void
classify_line(AwgQuickDiagnostics *diagnostics)
{
  static const char fallback_marker[] =
    "[!] Missing WireGuard (Amnezia VPN) kernel module. Falling back to slow userspace implementation.";
  AwgQuickStage stage = AWG_QUICK_STAGE_NONE;
  gboolean cleanup_capable_firewall = FALSE;

  /* Only awg-quick's command echoes and fixed fallback marker affect status. */
  if (diagnostics->length == sizeof(fallback_marker) - 1 &&
      memcmp(diagnostics->line, fallback_marker, sizeof(fallback_marker) - 1) == 0) {
    observe_stage(diagnostics, AWG_QUICK_STAGE_USERSPACE);
    if (diagnostics->failure_candidate == AWG_QUICK_STAGE_INTERFACE)
      diagnostics->failure_candidate = AWG_QUICK_STAGE_NONE;
    diagnostics->current_stage = AWG_QUICK_STAGE_USERSPACE;
    diagnostics->userspace_started = TRUE;
    return;
  }
  if (line_is_teardown_command(diagnostics->line, diagnostics->length)) {
    if (line_has_prefix(diagnostics->line, diagnostics->length,
                         "[#] ip link delete dev "))
      diagnostics->teardown_seen = TRUE;
    else
      diagnostics->recovery_blocked = TRUE;
    return;
  }

  if (line_has_prefix(diagnostics->line, diagnostics->length, "[#] ip link add "))
    stage = AWG_QUICK_STAGE_INTERFACE;
  else if (line_has_prefix(diagnostics->line, diagnostics->length, "[#] amneziawg-go ")) {
    stage = AWG_QUICK_STAGE_USERSPACE;
    diagnostics->userspace_started = TRUE;
  } else if (line_has_prefix(diagnostics->line, diagnostics->length, "[#] awg setconf "))
    stage = AWG_QUICK_STAGE_SETCONF;
  else if (line_has_prefix(diagnostics->line, diagnostics->length, "[#] ip -4 address add ") ||
           line_has_prefix(diagnostics->line, diagnostics->length, "[#] ip -6 address add "))
    stage = AWG_QUICK_STAGE_ADDRESS;
  else if (line_has_prefix(diagnostics->line, diagnostics->length, "[#] ip link set mtu "))
    stage = AWG_QUICK_STAGE_MTU;
  else if (line_has_prefix(diagnostics->line, diagnostics->length, "[#] resolvconf -a "))
    stage = AWG_QUICK_STAGE_DNS;
  else if (line_has_prefix(diagnostics->line, diagnostics->length, "[#] awg set ") ||
           line_has_prefix(diagnostics->line, diagnostics->length, "[#] ip -4 rule add ") ||
           line_has_prefix(diagnostics->line, diagnostics->length, "[#] ip -6 rule add ") ||
           line_has_prefix(diagnostics->line, diagnostics->length, "[#] ip -4 route add ") ||
           line_has_prefix(diagnostics->line, diagnostics->length, "[#] ip -6 route add "))
    stage = AWG_QUICK_STAGE_ROUTE;
  else if (line_has_prefix(diagnostics->line, diagnostics->length,
                           "[#] sysctl -q net.ipv4.conf.all.src_valid_mark=1"))
    stage = AWG_QUICK_STAGE_FIREWALL;
  else if (line_has_prefix(diagnostics->line, diagnostics->length, "[#] nft -f ") ||
           line_has_prefix(diagnostics->line, diagnostics->length, "[#] iptables-restore -n") ||
           line_has_prefix(diagnostics->line, diagnostics->length, "[#] ip6tables-restore -n")) {
    stage = AWG_QUICK_STAGE_FIREWALL;
    cleanup_capable_firewall = TRUE;
  }

  if (stage != AWG_QUICK_STAGE_NONE) {
    observe_stage(diagnostics, stage);
    if (cleanup_capable_firewall && diagnostics->failure_candidate != AWG_QUICK_STAGE_NONE)
      return;
    diagnostics->current_stage = stage;
    diagnostics->failure_candidate = AWG_QUICK_STAGE_NONE;
  } else {
    if (line_has_prefix(diagnostics->line, diagnostics->length, "[#]") &&
        (!line_is_link_claim(diagnostics->line, diagnostics->length) ||
         diagnostics->teardown_seen ||
         diagnostics->current_stage > AWG_QUICK_STAGE_USERSPACE))
      diagnostics->recovery_blocked = TRUE;
    if (diagnostics->length > 0 &&
        diagnostics->current_stage != AWG_QUICK_STAGE_NONE &&
        diagnostics->failure_candidate == AWG_QUICK_STAGE_NONE)
      diagnostics->failure_candidate = diagnostics->current_stage;
  }
}

void
awg_quick_diagnostics_consume(const guint8 *data, gsize length, gpointer user_data)
{
  AwgQuickDiagnostics *diagnostics = user_data;

  for (gsize i = 0; i < length; i++) {
    if (data[i] == '\n') {
      if (!diagnostics->discarding)
        classify_line(diagnostics);
      awg_helper_wipe_bytes(diagnostics->line, diagnostics->length);
      diagnostics->length = 0;
      diagnostics->discarding = FALSE;
    } else if (!diagnostics->discarding) {
      if (data[i] == '\0' || data[i] == '\r')
        diagnostics->recovery_blocked = TRUE;
      if (diagnostics->length < sizeof(diagnostics->line)) {
        diagnostics->line[diagnostics->length++] = data[i];
      } else {
        diagnostics->recovery_blocked = TRUE;
        classify_line(diagnostics);
        awg_helper_wipe_bytes(diagnostics->line, diagnostics->length);
        diagnostics->length = 0;
        diagnostics->discarding = TRUE;
      }
    }
  }
}

void
awg_quick_diagnostics_finish(AwgQuickDiagnostics *diagnostics)
{
  if (!diagnostics->discarding && diagnostics->length > 0) {
    diagnostics->recovery_blocked = TRUE;
    classify_line(diagnostics);
  }
  awg_helper_wipe_bytes(diagnostics->line, sizeof(diagnostics->line));
  diagnostics->length = 0;
}

NetworkSidebarAmneziaWGHelperExit
awg_quick_diagnostics_failure_status(const AwgQuickDiagnostics *diagnostics)
{
  AwgQuickStage stage = diagnostics->failure_candidate != AWG_QUICK_STAGE_NONE ?
    diagnostics->failure_candidate : diagnostics->current_stage;

  switch (stage) {
  case AWG_QUICK_STAGE_INTERFACE:
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CREATION_FAILED;
  case AWG_QUICK_STAGE_USERSPACE:
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_USERSPACE_FAILED;
  case AWG_QUICK_STAGE_SETCONF:
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SETCONF_FAILED;
  case AWG_QUICK_STAGE_ADDRESS:
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ADDRESS_FAILED;
  case AWG_QUICK_STAGE_MTU:
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_MTU_FAILED;
  case AWG_QUICK_STAGE_DNS:
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_FAILED;
  case AWG_QUICK_STAGE_ROUTE:
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ROUTE_FAILED;
  case AWG_QUICK_STAGE_FIREWALL:
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FIREWALL_FAILED;
  case AWG_QUICK_STAGE_NONE:
  default:
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_AWG_QUICK_FAILED;
  }
}

gboolean
awg_quick_diagnostics_failed_before_routes(const AwgQuickDiagnostics *diagnostics,
                                          const AwgSubprocessResult *child)
{
  return child->status == AWG_SUBPROCESS_EXITED && child->diagnostics_complete &&
         WIFEXITED(child->wait_status) && WEXITSTATUS(child->wait_status) != 0 &&
         WEXITSTATUS(child->wait_status) != 127 &&
         diagnostics->interface_seen && diagnostics->teardown_seen &&
         !diagnostics->recovery_blocked &&
         diagnostics->current_stage >= AWG_QUICK_STAGE_INTERFACE &&
         diagnostics->current_stage <= AWG_QUICK_STAGE_MTU;
}
