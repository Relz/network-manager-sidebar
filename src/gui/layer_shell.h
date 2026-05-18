#ifndef NETWORK_SIDEBAR_GUI_LAYER_SHELL_H
#define NETWORK_SIDEBAR_GUI_LAYER_SHELL_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NETWORK_SIDEBAR_LAYER_SHELL_ON_DEMAND_PROTOCOL_VERSION 4

char *network_sidebar_layer_shell_unavailable_message(void);
gboolean network_sidebar_initialize_layer_shell_window(GtkWindow *window, GError **error);
gboolean network_sidebar_configure_layer_shell_window(GtkWindow *window,
                                                      GtkWidget *panel,
                                                      const char *target_output_name,
                                                      GError **error);

G_END_DECLS

#endif
