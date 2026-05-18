#ifndef NETWORK_SIDEBAR_SECTIONS_HELPERS_H
#define NETWORK_SIDEBAR_SECTIONS_HELPERS_H

#include "data/connection_state.h"

#include <adwaita.h>

G_BEGIN_DECLS

GtkWidget *network_sidebar_section_group(const char *title);
GtkWidget *network_sidebar_action_row(const char *title, const char *subtitle, const char *icon_name);
GtkWidget *network_sidebar_flat_button(const char *icon_name, const char *tooltip);
void network_sidebar_apply_row_state(GtkWidget *row, NetworkSidebarRowState state);
void network_sidebar_clear_box(GtkBox *box);
void network_sidebar_add_notice(AdwPreferencesGroup *group, const char *title, const char *subtitle, const char *icon_name);

G_END_DECLS

#endif
