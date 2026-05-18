#include "data/connection_state.h"

NetworkSidebarRowState
network_sidebar_connection_row_state(NMActiveConnection *active)
{
  NMActiveConnectionState state;

  if (active == NULL)
    return NETWORK_SIDEBAR_ROW_STATE_NONE;

  state = nm_active_connection_get_state(active);
  if (state == NM_ACTIVE_CONNECTION_STATE_ACTIVATED)
    return NETWORK_SIDEBAR_ROW_STATE_ACTIVE;
  if (state == NM_ACTIVE_CONNECTION_STATE_ACTIVATING)
    return NETWORK_SIDEBAR_ROW_STATE_CONNECTING;
  return NETWORK_SIDEBAR_ROW_STATE_NONE;
}

NetworkSidebarRowState
network_sidebar_device_row_state(NMDevice *device, NMActiveConnection *active)
{
  NetworkSidebarRowState active_state = network_sidebar_connection_row_state(active);
  NMDeviceState state;

  if (active_state != NETWORK_SIDEBAR_ROW_STATE_NONE)
    return active_state;
  if (device == NULL)
    return NETWORK_SIDEBAR_ROW_STATE_NONE;

  state = nm_device_get_state(device);
  switch (state) {
  case NM_DEVICE_STATE_ACTIVATED:
    return NETWORK_SIDEBAR_ROW_STATE_ACTIVE;
  case NM_DEVICE_STATE_PREPARE:
  case NM_DEVICE_STATE_CONFIG:
  case NM_DEVICE_STATE_NEED_AUTH:
  case NM_DEVICE_STATE_IP_CONFIG:
  case NM_DEVICE_STATE_IP_CHECK:
  case NM_DEVICE_STATE_SECONDARIES:
    return NETWORK_SIDEBAR_ROW_STATE_CONNECTING;
  case NM_DEVICE_STATE_FAILED:
    return NETWORK_SIDEBAR_ROW_STATE_FAILED;
  case NM_DEVICE_STATE_DISCONNECTED:
  case NM_DEVICE_STATE_UNAVAILABLE:
  case NM_DEVICE_STATE_DEACTIVATING:
    return NETWORK_SIDEBAR_ROW_STATE_DISCONNECTED;
  default:
    return NETWORK_SIDEBAR_ROW_STATE_NONE;
  }
}

NetworkSidebarRowState
network_sidebar_vpn_row_state(NMActiveConnection *active)
{
  NetworkSidebarRowState generic_state = network_sidebar_connection_row_state(active);

  if (active == NULL)
    return NETWORK_SIDEBAR_ROW_STATE_NONE;
  if (!NM_IS_VPN_CONNECTION(active))
    return generic_state;

  switch (nm_vpn_connection_get_vpn_state(NM_VPN_CONNECTION(active))) {
  case NM_VPN_CONNECTION_STATE_ACTIVATED:
    return NETWORK_SIDEBAR_ROW_STATE_ACTIVE;
  case NM_VPN_CONNECTION_STATE_PREPARE:
  case NM_VPN_CONNECTION_STATE_NEED_AUTH:
  case NM_VPN_CONNECTION_STATE_CONNECT:
  case NM_VPN_CONNECTION_STATE_IP_CONFIG_GET:
    return NETWORK_SIDEBAR_ROW_STATE_CONNECTING;
  case NM_VPN_CONNECTION_STATE_FAILED:
    return NETWORK_SIDEBAR_ROW_STATE_FAILED;
  case NM_VPN_CONNECTION_STATE_DISCONNECTED:
    return NETWORK_SIDEBAR_ROW_STATE_DISCONNECTED;
  default:
    return generic_state == NETWORK_SIDEBAR_ROW_STATE_ACTIVE ? NETWORK_SIDEBAR_ROW_STATE_NONE : generic_state;
  }
}
