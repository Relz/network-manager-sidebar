#ifndef NETWORK_SIDEBAR_SECTIONS_VPN_H
#define NETWORK_SIDEBAR_SECTIONS_VPN_H

#include "actions/network_actions.h"

#include <adwaita.h>

G_BEGIN_DECLS

void network_sidebar_add_vpn_group(GtkBox *content, NMClient *client, NetworkSidebarActions *actions);

G_END_DECLS

#endif
