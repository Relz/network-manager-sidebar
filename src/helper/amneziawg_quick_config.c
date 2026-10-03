#include "helper/amneziawg_quick_config.h"

#include <string.h>

#define AWG_QUICK_LINK_HOOK_RESERVE 256u

gboolean
awg_quick_config_build_snapshot(
  const guint8 *protocol_config,
  gsize protocol_config_length,
  const NetworkSidebarAmneziaWGConfigEnvelope *envelope,
  guint8 **snapshot,
  gsize *snapshot_length,
  GError **error)
{
  return network_sidebar_amneziawg_runtime_snapshot_build(
    protocol_config, protocol_config_length, envelope, AWG_QUICK_LINK_HOOK_RESERVE,
    snapshot, snapshot_length, error);
}

gboolean
awg_quick_config_add_link_claim(
  const guint8 *snapshot,
  gsize snapshot_length,
  const char *link_token,
  guint8 **claimed_snapshot,
  gsize *claimed_snapshot_length,
  GError **error)
{
  NetworkSidebarAmneziaWGConfigEnvelope envelope = { 0 };
  char claim_lines[AWG_QUICK_LINK_HOOK_RESERVE];
  gsize insertion_offset;
  gsize output_offset;
  gsize separator_length = 0;
  gsize claim_length;
  gsize result_length;
  guint8 *result;
  int written;

  g_return_val_if_fail(claimed_snapshot != NULL, FALSE);
  g_return_val_if_fail(claimed_snapshot_length != NULL, FALSE);
  *claimed_snapshot = NULL;
  *claimed_snapshot_length = 0;
  if (!network_sidebar_amneziawg_link_token_is_valid(link_token)) {
    g_set_error_literal(error,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR_INVALID_DATA,
                        "runtime link identity is invalid");
    return FALSE;
  }
  if (!network_sidebar_amneziawg_config_parse_envelope(snapshot,
                                                        snapshot_length,
                                                        &envelope,
                                                        error))
    return FALSE;
  insertion_offset = envelope.interface_line_end;
  network_sidebar_amneziawg_config_envelope_clear(&envelope);

  written = g_snprintf(
    claim_lines,
    sizeof(claim_lines),
    "PreUp = ip link set dev %%i alias "
      NETWORK_SIDEBAR_AMNEZIAWG_LINK_ALIAS_PREFIX "%s\n"
    "PreDown = grep -Fxq "
      NETWORK_SIDEBAR_AMNEZIAWG_LINK_ALIAS_PREFIX "%s "
      "/sys/class/net/%%i/ifalias\n",
    link_token,
    link_token);
  if (written <= 0 || (gsize) written >= sizeof(claim_lines)) {
    g_set_error_literal(error,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR_TOO_LARGE,
                        "runtime link claim is too large");
    return FALSE;
  }
  claim_length = (gsize) written;
  if (insertion_offset > 0 && snapshot[insertion_offset - 1] != '\n')
    separator_length = 1;
  if (snapshot_length > NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE -
                          separator_length ||
      claim_length > NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE -
                       separator_length - snapshot_length) {
    g_set_error_literal(error,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR_TOO_LARGE,
                        "runtime configuration exceeds the size limit");
    return FALSE;
  }

  result_length = snapshot_length + separator_length + claim_length;
  result = g_malloc(result_length);
  memcpy(result, snapshot, insertion_offset);
  output_offset = insertion_offset;
  if (separator_length != 0)
    result[output_offset++] = '\n';
  memcpy(result + output_offset, claim_lines, claim_length);
  output_offset += claim_length;
  memcpy(result + output_offset,
         snapshot + insertion_offset,
         snapshot_length - insertion_offset);
  *claimed_snapshot = result;
  *claimed_snapshot_length = result_length;
  return TRUE;
}
