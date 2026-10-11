#ifndef NETWORK_SIDEBAR_SECTIONS_VPN_H
#define NETWORK_SIDEBAR_SECTIONS_VPN_H

#include "actions/network_actions.h"
#include "gui/profile_editor_presenter.h"

#include <adwaita.h>

G_BEGIN_DECLS

GtkWidget *network_sidebar_vpn_section_new(
    NetworkSidebarActions *actions,
    NetworkSidebarProfileEditorPresenter *editor_presenter);
void network_sidebar_vpn_section_update(GtkWidget *section, NMClient *client);

G_END_DECLS

#endif
