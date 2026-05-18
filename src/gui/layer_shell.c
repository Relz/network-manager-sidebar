#include "gui/layer_shell.h"

#include "core/config.h"

#include <gio/gio.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>

static GdkMonitor *target_monitor(const char *target_output_name);

char *
network_sidebar_layer_shell_unavailable_message(void)
{
  if (gtk_layer_is_supported())
    return NULL;
  return g_strdup_printf("%s (the current compositor/session does not support gtk4-layer-shell)",
                         NETWORK_SIDEBAR_LAYER_SHELL_REQUIRED_MESSAGE);
}

gboolean
network_sidebar_initialize_layer_shell_window(GtkWindow *window, GError **error)
{
  g_autofree char *message = network_sidebar_layer_shell_unavailable_message();

  if (message != NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, message);
    return FALSE;
  }

  gtk_layer_init_for_window(window);
  gtk_layer_set_namespace(window, "nm-sidebar");
  return TRUE;
}

static GtkLayerShellKeyboardMode
keyboard_mode(void)
{
  if (gtk_layer_get_protocol_version() >= NETWORK_SIDEBAR_LAYER_SHELL_ON_DEMAND_PROTOCOL_VERSION)
    return (GtkLayerShellKeyboardMode) 2;
  return GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE;
}

static void
set_respect_close_if_supported(GtkWindow *window)
{
  if (gtk_layer_get_major_version() > 1 ||
      (gtk_layer_get_major_version() == 1 && gtk_layer_get_minor_version() >= 3))
    gtk_layer_set_respect_close(window, TRUE);
}

gboolean
network_sidebar_configure_layer_shell_window(GtkWindow *window,
                                             GtkWidget *panel,
                                             const char *target_output_name,
                                             GError **error)
{
  g_autofree char *message = network_sidebar_layer_shell_unavailable_message();
  g_autoptr(GdkMonitor) monitor = NULL;

  if (message != NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, message);
    return FALSE;
  }

  monitor = target_monitor(target_output_name);
  if (monitor != NULL)
    gtk_layer_set_monitor(window, monitor);

  gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_TOP);
  gtk_layer_set_exclusive_zone(window, 0);
  gtk_layer_set_keyboard_mode(window, keyboard_mode());
  set_respect_close_if_supported(window);

  gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
  gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
  gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
  gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);

  gtk_widget_set_halign(panel, GTK_ALIGN_END);
  gtk_widget_set_valign(panel, GTK_ALIGN_FILL);
  gtk_widget_set_vexpand(panel, TRUE);
  gtk_widget_set_size_request(panel, NETWORK_SIDEBAR_WIDTH, -1);
  return TRUE;
}

static GdkMonitor *
monitor_for_output_name(GListModel *monitors, const char *output_name)
{
  guint count;

  if (output_name == NULL)
    return NULL;

  count = g_list_model_get_n_items(monitors);
  for (guint i = 0; i < count; i++) {
    g_autoptr(GdkMonitor) monitor = g_list_model_get_item(monitors, i);
    const char *connector = gdk_monitor_get_connector(monitor);
    if (g_strcmp0(connector, output_name) == 0)
      return g_object_ref(monitor);
  }
  return NULL;
}

static GdkMonitor *
target_monitor(const char *target_output_name)
{
  GdkDisplay *display = gdk_display_get_default();
  GListModel *monitors;

  if (display == NULL)
    return NULL;

  monitors = gdk_display_get_monitors(display);
  if (target_output_name != NULL) {
    GdkMonitor *monitor = monitor_for_output_name(monitors, target_output_name);
    if (monitor != NULL)
      return monitor;
  }

  if (g_list_model_get_n_items(monitors) < 1)
    return NULL;
  return g_list_model_get_item(monitors, 0);
}
