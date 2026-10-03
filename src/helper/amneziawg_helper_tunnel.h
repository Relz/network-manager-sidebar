#ifndef NETWORK_SIDEBAR_AMNEZIAWG_HELPER_TUNNEL_H
#define NETWORK_SIDEBAR_AMNEZIAWG_HELPER_TUNNEL_H

#include "amneziawg/deadline.h"
#include "helper/amneziawg_helper_profiles.h"

typedef struct _AwgHelperTunnel AwgHelperTunnel;
typedef struct _AwgHelperActivation AwgHelperActivation;

AwgHelperTunnel *awg_helper_tunnel_new(AwgHelperStorage *storage);
void awg_helper_tunnel_free(AwgHelperTunnel *tunnel);

/* Prepares immutable profile data and checks prerequisites without changing
 * network state or creating a runtime journal. The caller holds the runtime
 * lock through preparation, any intervening disconnects, and activation. */
NetworkSidebarAmneziaWGHelperExit awg_helper_tunnel_prepare_up(
  AwgHelperTunnel *tunnel,
  AwgHelperProfiles *profiles,
  const char *name,
  gint64 deadline,
  AwgHelperActivation **activation);
void awg_helper_activation_free(AwgHelperActivation *activation);
/* Consumes the prepared attempt exactly once; only successful activation
 * commits/releases the helper's process supervision. */
NetworkSidebarAmneziaWGHelperExit awg_helper_tunnel_activate(
  AwgHelperTunnel *tunnel,
  AwgHelperActivation *activation,
  const NetworkSidebarAmneziaWGDeadlines *deadlines);
/* Read-only admission check for automatic cleanup. Down revalidates all state. */
NetworkSidebarAmneziaWGHelperExit awg_helper_tunnel_check_down(
  AwgHelperTunnel *tunnel,
  const char *name,
  gint64 deadline);
NetworkSidebarAmneziaWGHelperExit awg_helper_tunnel_down(
  AwgHelperTunnel *tunnel,
  const char *name,
  const char *config_name,
  const char *snapshot_path,
  const NetworkSidebarAmneziaWGDeadlines *deadlines);

#endif
