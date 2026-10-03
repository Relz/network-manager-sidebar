#include "helper/amneziawg_helper_session.h"

#include "amneziawg/link_identity.h"

AwgHelperOwnedLinkState
awg_helper_session_owned_link_state(
  const char *name,
  const NetworkSidebarAmneziaWGRuntimeMarker *marker,
  NetworkSidebarAmneziaWGLinkObservation *observation)
{
  NetworkSidebarAmneziaWGLinkObservation current = { 0 };

  if (marker == NULL ||
      !network_sidebar_amneziawg_link_observe(name, marker->link_token, &current))
    return AWG_HELPER_OWNED_LINK_UNKNOWN;
  if (observation != NULL)
    *observation = current;
  if (current.token_matches > 1)
    return AWG_HELPER_OWNED_LINK_UNKNOWN;
  if (current.token_matches == 0)
    return current.named_exists ? AWG_HELPER_OWNED_LINK_CONFLICT :
                                  AWG_HELPER_OWNED_LINK_ABSENT;
  if (!current.token_has_expected_name ||
      (current.named_exists && current.named_ifindex != current.token_ifindex) ||
      (marker->ifindex != 0 && marker->ifindex != current.token_ifindex))
    return AWG_HELPER_OWNED_LINK_CONFLICT;
  return AWG_HELPER_OWNED_LINK_PRESENT;
}

NetworkSidebarAmneziaWGHelperExit
awg_helper_session_runtime_state(
  const AwgHelperStorage *storage,
  const char *name,
  gboolean *has_marker,
  gboolean *has_interface,
  NetworkSidebarAmneziaWGRuntimeMarker *marker_value)
{
  NetworkSidebarAmneziaWGRuntimeMarker parsed_marker = { 0 };
  AwgHelperEntryState marker = awg_helper_storage_read_runtime_marker(
    storage, name, &parsed_marker);
  NetworkSidebarAmneziaWGLinkObservation observation = { 0 };
  AwgHelperOwnedLinkState owned_state;

  *has_marker = FALSE;
  *has_interface = FALSE;
  if (marker == AWG_HELPER_ENTRY_MISSING) {
    if (!network_sidebar_amneziawg_link_observe(name, NULL, &observation))
      return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
    *has_interface = observation.named_exists;
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
  }
  if (marker == AWG_HELPER_ENTRY_INVALID)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  owned_state = awg_helper_session_owned_link_state(name, &parsed_marker, NULL);
  if (owned_state == AWG_HELPER_OWNED_LINK_UNKNOWN)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  if (owned_state == AWG_HELPER_OWNED_LINK_CONFLICT)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT;
  *has_interface = owned_state == AWG_HELPER_OWNED_LINK_PRESENT;
  *has_marker = TRUE;
  if (marker_value != NULL)
    *marker_value = parsed_marker;
  return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
}

NetworkSidebarAmneziaWGHelperExit
awg_helper_session_refresh_owned_interface(
  const AwgHelperStorage *storage,
  const char *name,
  NetworkSidebarAmneziaWGRuntimeMarker *marker,
  gboolean *interface_exists)
{
  NetworkSidebarAmneziaWGLinkObservation observation = { 0 };
  AwgHelperOwnedLinkState state = awg_helper_session_owned_link_state(
    name, marker, &observation);

  if (interface_exists != NULL)
    *interface_exists = FALSE;
  if (state == AWG_HELPER_OWNED_LINK_UNKNOWN)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  if (state == AWG_HELPER_OWNED_LINK_CONFLICT)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT;
  if (state == AWG_HELPER_OWNED_LINK_ABSENT)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;

  if (interface_exists != NULL)
    *interface_exists = TRUE;
  if (marker->ifindex == 0) {
    marker->ifindex = observation.token_ifindex;
    if (!awg_helper_storage_write_runtime_marker(storage, name, marker))
      return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  }
  return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
}
