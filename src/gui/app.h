#ifndef NETWORK_SIDEBAR_GUI_APP_H
#define NETWORK_SIDEBAR_GUI_APP_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _NetworkSidebarGuiApp NetworkSidebarGuiApp;

NetworkSidebarGuiApp *network_sidebar_gui_app_new(const char *target_output_name);
int network_sidebar_gui_app_run(NetworkSidebarGuiApp *app, int argc, char **argv);
gboolean network_sidebar_gui_app_handle_command(NetworkSidebarGuiApp *app,
                                                const char *command,
                                                const char *target_output_name);

G_END_DECLS

#endif
