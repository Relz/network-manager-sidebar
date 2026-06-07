#ifndef NETWORK_SIDEBAR_GUI_STYLES_H
#define NETWORK_SIDEBAR_GUI_STYLES_H

#include <glib.h>

G_BEGIN_DECLS

typedef void (*NetworkSidebarErrorCallback)(const char *message, gpointer user_data);

void network_sidebar_install_application_css(NetworkSidebarErrorCallback report_error,
                                              gpointer user_data);
void network_sidebar_reload_user_css(NetworkSidebarErrorCallback report_error,
                                     gpointer user_data);

G_END_DECLS

#endif
