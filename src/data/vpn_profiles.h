#ifndef NETWORK_SIDEBAR_DATA_VPN_PROFILES_H
#define NETWORK_SIDEBAR_DATA_VPN_PROFILES_H

#include "data/connection_state.h"

G_BEGIN_DECLS

typedef enum {
  NETWORK_SIDEBAR_VPN_PROFILE_PLUGIN,
  NETWORK_SIDEBAR_VPN_PROFILE_WIREGUARD,
} NetworkSidebarVpnProfileKind;

typedef enum {
  NETWORK_SIDEBAR_VPN_PLUGIN_AVAILABLE,
  NETWORK_SIDEBAR_VPN_PLUGIN_MISSING,
  NETWORK_SIDEBAR_VPN_PLUGIN_UNKNOWN,
} NetworkSidebarVpnPluginStatus;

typedef enum {
  NETWORK_SIDEBAR_VPN_ACTIVATION_ALLOWED,
  NETWORK_SIDEBAR_VPN_ACTIVATION_NETWORKING_DISABLED,
  NETWORK_SIDEBAR_VPN_ACTIVATION_NO_BASE_CONNECTION,
  NETWORK_SIDEBAR_VPN_ACTIVATION_PLUGIN_MISSING,
} NetworkSidebarVpnActivationReason;

/* Records belong to the snapshot, which owns all strings and object references.
 * Consumers retaining connections beyond the snapshot lifetime must ref them.
 */
typedef struct {
  NMRemoteConnection *connection;
  NMActiveConnection *active;
  NetworkSidebarVpnProfileKind kind;
  char *name;
  char *uuid;
  gboolean duplicate_name;
  char *status;
  NetworkSidebarRowState row_state;
  NetworkSidebarVpnPluginStatus plugin_status;
  NetworkSidebarVpnActivationReason activation_reason;
  gboolean can_activate;
  gboolean can_edit;
} NetworkSidebarVpnProfile;

typedef struct {
  /* NetworkSidebarVpnProfile records, sorted by case-folded ID, then UUID. */
  GPtrArray *profiles;
  NetworkSidebarVpnPluginStatus plugin_status;
  gboolean can_create_vpn;
} NetworkSidebarVpnProfiles;

NetworkSidebarVpnProfiles *network_sidebar_vpn_profiles_load(NMClient *client);
void network_sidebar_vpn_profiles_free(NetworkSidebarVpnProfiles *profiles);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarVpnProfiles,
                              network_sidebar_vpn_profiles_free)

G_END_DECLS

#endif
