#ifndef NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SESSION_H
#define NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SESSION_H

#include "helper/amneziawg_helper_storage.h"

typedef enum {
  AWG_HELPER_OWNED_LINK_ABSENT,
  AWG_HELPER_OWNED_LINK_PRESENT,
  AWG_HELPER_OWNED_LINK_CONFLICT,
  AWG_HELPER_OWNED_LINK_UNKNOWN,
} AwgHelperOwnedLinkState;

/* Reconciles kernel identity with the protected journal. Storage itself never
 * observes network links or decides whether a session may adopt an ifindex. */
AwgHelperOwnedLinkState awg_helper_session_owned_link_state(
  const char *name,
  const NetworkSidebarAmneziaWGRuntimeMarker *marker,
  NetworkSidebarAmneziaWGLinkObservation *observation);
NetworkSidebarAmneziaWGHelperExit awg_helper_session_runtime_state(
  const AwgHelperStorage *storage,
  const char *name,
  gboolean *has_marker,
  gboolean *has_interface,
  NetworkSidebarAmneziaWGRuntimeMarker *marker_value);
/* Adopts only a uniquely token-matched link, persisting the ifindex before
 * returning success. The caller holds the storage operation lock. */
NetworkSidebarAmneziaWGHelperExit awg_helper_session_refresh_owned_interface(
  const AwgHelperStorage *storage,
  const char *name,
  NetworkSidebarAmneziaWGRuntimeMarker *marker,
  gboolean *interface_exists);

#endif
