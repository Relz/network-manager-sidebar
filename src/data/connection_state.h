#ifndef NETWORK_SIDEBAR_DATA_CONNECTION_STATE_H
#define NETWORK_SIDEBAR_DATA_CONNECTION_STATE_H

#include <NetworkManager.h>

G_BEGIN_DECLS

typedef enum {
  NETWORK_SIDEBAR_ROW_STATE_NONE,
  NETWORK_SIDEBAR_ROW_STATE_ACTIVE,
  NETWORK_SIDEBAR_ROW_STATE_CONNECTING,
  NETWORK_SIDEBAR_ROW_STATE_FAILED,
  NETWORK_SIDEBAR_ROW_STATE_DISCONNECTED,
} NetworkSidebarRowState;

NetworkSidebarRowState network_sidebar_connection_row_state(NMActiveConnection *active);
NetworkSidebarRowState network_sidebar_device_row_state(NMDevice *device, NMActiveConnection *active);
NetworkSidebarRowState network_sidebar_vpn_row_state(NMActiveConnection *active);

G_END_DECLS

#endif
