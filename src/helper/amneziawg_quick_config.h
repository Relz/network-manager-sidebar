#ifndef NETWORK_SIDEBAR_AMNEZIAWG_QUICK_CONFIG_H
#define NETWORK_SIDEBAR_AMNEZIAWG_QUICK_CONFIG_H

#include "amneziawg/config.h"

/* Builds the DNS-free snapshot with space reserved for internal quick hooks. */
gboolean awg_quick_config_build_snapshot(
  const guint8 *protocol_config,
  gsize protocol_config_length,
  const NetworkSidebarAmneziaWGConfigEnvelope *envelope,
  guint8 **snapshot,
  gsize *snapshot_length,
  GError **error);

/* Adds the only internally generated command hooks. Stored user hooks remain
 * prohibited by the shared envelope parser. */
gboolean awg_quick_config_add_link_claim(
  const guint8 *snapshot,
  gsize snapshot_length,
  const char *link_token,
  guint8 **claimed_snapshot,
  gsize *claimed_snapshot_length,
  GError **error);

#endif
