#ifndef NETWORK_SIDEBAR_AMNEZIAWG_HELPER_REMOVE_H
#define NETWORK_SIDEBAR_AMNEZIAWG_HELPER_REMOVE_H

#include "amneziawg/deadline.h"
#include "helper/amneziawg_helper_profiles.h"

/* The caller retains the runtime lock and supervision across cleanup and
 * deletion. A failed cleanup always retains the saved profile. */
NetworkSidebarAmneziaWGHelperExit awg_helper_remove(
  AwgHelperStorage *storage,
  AwgHelperProfiles *profiles,
  const char *name,
  const NetworkSidebarAmneziaWGDeadlines *deadlines);

#endif
