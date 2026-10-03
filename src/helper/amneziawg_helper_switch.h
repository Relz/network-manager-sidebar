#ifndef NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SWITCH_H
#define NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SWITCH_H

#include "helper/amneziawg_helper_tunnel.h"

/* One authorized Up under the caller's runtime lock. All previous owned
 * sessions must finish cleanup before the prepared target is activated.
 * Failure never reactivates a previous session. */
NetworkSidebarAmneziaWGHelperExit awg_helper_switch_up(
  AwgHelperStorage *storage,
  AwgHelperProfiles *profiles,
  const char *name,
  const NetworkSidebarAmneziaWGDeadlines *deadlines);

#endif
