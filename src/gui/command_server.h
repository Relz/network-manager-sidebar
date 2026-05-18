#ifndef NETWORK_SIDEBAR_GUI_COMMAND_SERVER_H
#define NETWORK_SIDEBAR_GUI_COMMAND_SERVER_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _NetworkSidebarGuiApp NetworkSidebarGuiApp;
typedef struct _NetworkSidebarCommandServer NetworkSidebarCommandServer;

NetworkSidebarCommandServer *network_sidebar_command_server_new(NetworkSidebarGuiApp *app,
                                                                GError **error);
gboolean network_sidebar_command_server_is_healthy(NetworkSidebarCommandServer *server);
void network_sidebar_command_server_close(NetworkSidebarCommandServer *server);

G_END_DECLS

#endif
