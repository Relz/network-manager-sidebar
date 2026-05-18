#include "gui/app.h"

#include "core/config.h"
#include "core/ipc.h"
#include "core/ipc_commands.h"
#include "core/target_output.h"
#include "gui/command_server.h"
#include "gui/layer_shell.h"
#include "gui/styles.h"
#include "actions/network_actions.h"
#include "sections/connection_info.h"
#include "sections/helpers.h"
#include "sections/status.h"
#include "sections/vpn.h"
#include "sections/wifi.h"

#include <adwaita.h>
#include <gtk/gtk.h>
#include <NetworkManager.h>
#include <stdio.h>

typedef struct {
  GObject *object;
  gulong handler_id;
} RefreshSignalHandler;

struct _NetworkSidebarGuiApp {
  AdwApplication *application;
  NMClient *client;
  NetworkSidebarActions *actions;
  NetworkSidebarCommandServer *server;
  GtkWidget *window;
  GtkWidget *surface;
  GtkWidget *toast_overlay;
  GtkWidget *back_button;
  GtkWidget *title;
  GtkWidget *info_button;
  GtkWidget *networking_switch;
  GtkWidget *scrolled;
  GtkWidget *content;
  GtkWidget *scroll_fade_top;
  GtkWidget *scroll_fade_bottom;
  gboolean refreshing;
  gboolean showing_connection_information;
  guint refresh_source;
  gboolean refresh_visible_only;
  guint signal_refresh_source;
  guint periodic_refresh_source;
  GPtrArray *refresh_signal_handlers;
  gboolean outside_click_started;
  gboolean unsupported_layer_shell;
  gboolean runtime_failure;
  char *target_output_name;
};

static void on_startup(GApplication *application, gpointer user_data);
static void on_activate(GApplication *application, gpointer user_data);
static int on_command_line(GApplication *application, GApplicationCommandLine *command_line, gpointer user_data);
static void on_shutdown(GApplication *application, gpointer user_data);
static gboolean prepare_runtime(NetworkSidebarGuiApp *self);
static gboolean ensure_nm_client(NetworkSidebarGuiApp *self);
static gboolean ensure_window(NetworkSidebarGuiApp *self);
static gboolean ensure_command_server(NetworkSidebarGuiApp *self, gboolean repair);
static void show_sidebar(NetworkSidebarGuiApp *self);
static void hide_sidebar(NetworkSidebarGuiApp *self);
static void toggle_sidebar(NetworkSidebarGuiApp *self);
static void set_target_output_name(NetworkSidebarGuiApp *self, const char *target_output_name);
static void refresh_content(NetworkSidebarGuiApp *self);
static void schedule_refresh(NetworkSidebarGuiApp *self, guint delay_ms, gboolean visible_only);
static gboolean signal_refresh_cb(gpointer user_data);
static gboolean periodic_refresh_cb(gpointer user_data);
static void update_scroll_fades(NetworkSidebarGuiApp *self);
static void sync_refresh_signals(NetworkSidebarGuiApp *self);
static void on_nm_refresh_signal(NetworkSidebarGuiApp *self);

NetworkSidebarGuiApp *
network_sidebar_gui_app_new(const char *target_output_name)
{
  NetworkSidebarGuiApp *self = g_new0(NetworkSidebarGuiApp, 1);

  self->application = adw_application_new(
    NETWORK_SIDEBAR_APP_ID,
    G_APPLICATION_HANDLES_COMMAND_LINE);
  g_set_application_name(NETWORK_SIDEBAR_APP_NAME);
  self->target_output_name = network_sidebar_normalize_target_output(target_output_name);

  g_signal_connect(self->application, "startup", G_CALLBACK(on_startup), self);
  g_signal_connect(self->application, "activate", G_CALLBACK(on_activate), self);
  g_signal_connect(self->application, "command-line", G_CALLBACK(on_command_line), self);
  g_signal_connect(self->application, "shutdown", G_CALLBACK(on_shutdown), self);
  return self;
}

int
network_sidebar_gui_app_run(NetworkSidebarGuiApp *self, int argc, char **argv)
{
  int status;

  g_return_val_if_fail(self != NULL, 1);
  status = g_application_run(G_APPLICATION(self->application), argc, argv);
  if (self->unsupported_layer_shell || self->runtime_failure)
    status = 1;
  g_clear_pointer(&self->actions, network_sidebar_actions_unref);
  g_clear_pointer(&self->refresh_signal_handlers, g_ptr_array_unref);
  g_clear_object(&self->application);
  g_clear_object(&self->client);
  g_clear_pointer(&self->target_output_name, g_free);
  g_free(self);
  return status;
}

static void
report_startup_error(const char *message, gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;

  if (!self->runtime_failure)
    g_printerr("nm-sidebar: %s\n", message);
  self->runtime_failure = TRUE;
  if (self->server != NULL) {
    network_sidebar_command_server_close(self->server);
    self->server = NULL;
  }
  g_application_quit(G_APPLICATION(self->application));
}

static void
on_startup(GApplication *application, gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;

  g_application_hold(application);
  network_sidebar_install_application_css(report_startup_error, self);
}

static void
on_activate(GApplication *application, gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;
  (void) application;

  if (self->runtime_failure)
    return;
  if (prepare_runtime(self))
    show_sidebar(self);
}

static char *
command_from_arguments(char **argv, int argc)
{
  for (int i = 1; i < argc; i++) {
    g_autofree char *command = NULL;
    command = network_sidebar_normalize_command(argv[i]);
    if (network_sidebar_command_is_application(command))
      return g_steal_pointer(&command);
  }
  return g_strdup(NETWORK_SIDEBAR_DEFAULT_APPLICATION_COMMAND);
}

static char *
target_output_name_from_command_line(GApplicationCommandLine *command_line)
{
  static const char *env_vars[] = {
    "NM_SIDEBAR_OUTPUT",
    "WAYBAR_OUTPUT_NAME",
    NULL,
  };

  for (gsize i = 0; env_vars[i] != NULL; i++) {
    g_autofree char *normalized = network_sidebar_normalize_target_output(g_application_command_line_getenv(command_line, env_vars[i]));
    if (normalized != NULL)
      return g_steal_pointer(&normalized);
  }
  return NULL;
}

static int
handle_application_command(NetworkSidebarGuiApp *self, const char *command, gboolean is_remote)
{
  if (g_strcmp0(command, NETWORK_SIDEBAR_COMMAND_BACKGROUND) == 0) {
    if (is_remote)
      ensure_command_server(self, TRUE);
    else
      prepare_runtime(self);
    return 0;
  }
  if (g_strcmp0(command, NETWORK_SIDEBAR_COMMAND_HIDE) == 0) {
    if (is_remote)
      hide_sidebar(self);
    else {
      g_printerr("nm-sidebar: no running instance reached for --%s\n", command);
      g_application_quit(G_APPLICATION(self->application));
    }
    return is_remote ? 0 : 1;
  }
  if (g_strcmp0(command, NETWORK_SIDEBAR_COMMAND_QUIT) == 0) {
    if (is_remote)
      g_application_quit(G_APPLICATION(self->application));
    else {
      g_printerr("nm-sidebar: no running instance reached for --%s\n", command);
      g_application_quit(G_APPLICATION(self->application));
    }
    return is_remote ? 0 : 1;
  }
  if (is_remote) {
    if ((g_strcmp0(command, NETWORK_SIDEBAR_COMMAND_TOGGLE) == 0 || g_strcmp0(command, NETWORK_SIDEBAR_COMMAND_SHOW) == 0) &&
        !ensure_command_server(self, TRUE))
      return 1;
    network_sidebar_gui_app_handle_command(self, command, NULL);
    return 0;
  }
  if (prepare_runtime(self))
    network_sidebar_gui_app_handle_command(self, command, NULL);
  return 0;
}

static int
on_command_line(GApplication *application, GApplicationCommandLine *command_line, gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;
  g_auto(GStrv) argv = NULL;
  int argc = 0;
  g_autofree char *command = NULL;
  g_autofree char *target_output_name = NULL;
  int status;
  (void) application;

  if (self->runtime_failure)
    return 1;

  argv = g_application_command_line_get_arguments(command_line, &argc);
  target_output_name = target_output_name_from_command_line(command_line);
  set_target_output_name(self, target_output_name);
  command = command_from_arguments(argv, argc);
  status = handle_application_command(self, command, g_application_command_line_get_is_remote(command_line));
  if (status != 0)
    return status;
  return self->unsupported_layer_shell || self->runtime_failure ? 1 : 0;
}

static void
on_shutdown(GApplication *application, gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;
  (void) application;

  if (self->refresh_source != 0) {
    g_source_remove(self->refresh_source);
    self->refresh_source = 0;
  }
  if (self->signal_refresh_source != 0) {
    g_source_remove(self->signal_refresh_source);
    self->signal_refresh_source = 0;
  }
  if (self->periodic_refresh_source != 0) {
    g_source_remove(self->periodic_refresh_source);
    self->periodic_refresh_source = 0;
  }
  g_clear_pointer(&self->refresh_signal_handlers, g_ptr_array_unref);

  if (self->server != NULL) {
    network_sidebar_command_server_close(self->server);
    self->server = NULL;
  }
}

static void
fail_layer_shell(NetworkSidebarGuiApp *self, const char *message)
{
  if (!self->unsupported_layer_shell)
    g_printerr("nm-sidebar: %s\n", message);
  self->unsupported_layer_shell = TRUE;
  if (self->server != NULL) {
    network_sidebar_command_server_close(self->server);
    self->server = NULL;
  }
  g_application_quit(G_APPLICATION(self->application));
}

static void
fail_runtime(NetworkSidebarGuiApp *self, const char *message)
{
  if (!self->runtime_failure)
    g_printerr("nm-sidebar: %s\n", message);
  self->runtime_failure = TRUE;
  if (self->server != NULL) {
    network_sidebar_command_server_close(self->server);
    self->server = NULL;
  }
  g_application_quit(G_APPLICATION(self->application));
}

static gboolean
ensure_nm_client(NetworkSidebarGuiApp *self)
{
  g_autoptr(GError) error = NULL;

  if (self->client != NULL)
    return TRUE;

  self->client = nm_client_new(NULL, &error);
  if (self->client == NULL) {
    g_autofree char *message = g_strdup_printf("error: failed to initialize NetworkManager client: %s",
                                               error != NULL ? error->message : "unknown error");
    fail_runtime(self, message);
    return FALSE;
  }
  return TRUE;
}

static AdwToast *
toast_callback(const char *message, gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;
  AdwToast *toast;

  if (self->toast_overlay == NULL || self->window == NULL || !gtk_widget_get_visible(self->window))
    return NULL;

  toast = adw_toast_new(message);
  adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toast_overlay), g_object_ref(toast));
  return toast;
}

static void
schedule_refresh_callback(guint delay_ms, gpointer user_data)
{
  schedule_refresh(user_data, delay_ms, TRUE);
}

static void
ensure_actions(NetworkSidebarGuiApp *self)
{
  if (self->actions != NULL)
    return;
  self->actions = network_sidebar_actions_new(self->client, toast_callback, schedule_refresh_callback, self);
  if (self->window != NULL)
    network_sidebar_actions_set_parent(self->actions, GTK_WINDOW(self->window));
}

static void
refresh_signal_handler_free(RefreshSignalHandler *handler)
{
  if (handler == NULL)
    return;
  if (handler->object != NULL && handler->handler_id != 0 && g_signal_handler_is_connected(handler->object, handler->handler_id))
    g_signal_handler_disconnect(handler->object, handler->handler_id);
  g_clear_object(&handler->object);
  g_free(handler);
}

static gboolean
refresh_signal_supported(GObject *object, const char *signal_name)
{
  g_autofree char *base_signal = NULL;
  const char *separator;

  if (object == NULL || signal_name == NULL)
    return FALSE;
  separator = strstr(signal_name, "::");
  if (separator != NULL) {
    g_autofree char *property_name = g_strdup(separator + 2);
    if (g_strcmp0(signal_name, "notify") != 0 && g_str_has_prefix(signal_name, "notify::") &&
        g_object_class_find_property(G_OBJECT_GET_CLASS(object), property_name) == NULL)
      return FALSE;
  }
  base_signal = separator != NULL ? g_strndup(signal_name, separator - signal_name) : g_strdup(signal_name);
  return g_signal_lookup(base_signal, G_OBJECT_TYPE(object)) != 0;
}

static void
connect_refresh_signal(NetworkSidebarGuiApp *self, GObject *object, const char *signal_name)
{
  RefreshSignalHandler *handler;

  if (!refresh_signal_supported(object, signal_name))
    return;
  if (self->refresh_signal_handlers == NULL)
    self->refresh_signal_handlers = g_ptr_array_new_with_free_func((GDestroyNotify) refresh_signal_handler_free);

  handler = g_new0(RefreshSignalHandler, 1);
  handler->object = g_object_ref(object);
  handler->handler_id = g_signal_connect_swapped(object, signal_name, G_CALLBACK(on_nm_refresh_signal), self);
  g_ptr_array_add(self->refresh_signal_handlers, handler);
}

static void
connect_refresh_signals_for_object(NetworkSidebarGuiApp *self, GObject *object, const char *const *signals)
{
  for (guint i = 0; signals[i] != NULL; i++)
    connect_refresh_signal(self, object, signals[i]);
}

static void
on_nm_refresh_signal(NetworkSidebarGuiApp *self)
{
  sync_refresh_signals(self);
  if (self->signal_refresh_source != 0)
    return;
  self->signal_refresh_source = g_timeout_add(300, signal_refresh_cb, self);
}

static void
sync_refresh_signals(NetworkSidebarGuiApp *self)
{
  static const char *client_signals[] = {
    "device-added",
    "device-removed",
    "connection-added",
    "connection-removed",
    "active-connection-added",
    "active-connection-removed",
    "notify::connections",
    "notify::active-connections",
    "notify::networking-enabled",
    "notify::wireless-enabled",
    "notify::wireless-hardware-enabled",
    NULL,
  };
  static const char *device_signals[] = {
    "state-changed",
    "notify",
    NULL,
  };
  static const char *wifi_device_signals[] = {
    "access-point-added",
    "access-point-removed",
    "notify::access-points",
    "notify::active-access-point",
    "notify::last-scan",
    "notify::state",
    "notify::active-connection",
    NULL,
  };
  static const char *access_point_signals[] = {
    "notify::strength",
    NULL,
  };
  static const char *active_connection_signals[] = {
    "state-changed",
    "notify::state",
    "notify",
    NULL,
  };
  static const char *vpn_connection_signals[] = {
    "state-changed",
    "notify::state",
    "notify",
    "vpn-state-changed",
    "notify::vpn-state",
    NULL,
  };
  static const char *remote_connection_signals[] = {
    "changed",
    "notify::version-id",
    "notify::visible",
    "notify",
    NULL,
  };
  const GPtrArray *devices;
  const GPtrArray *active_connections;
  const GPtrArray *connections;

  if (self->client == NULL)
    return;
  if (self->refresh_signal_handlers != NULL)
    g_ptr_array_set_size(self->refresh_signal_handlers, 0);
  else
    self->refresh_signal_handlers = g_ptr_array_new_with_free_func((GDestroyNotify) refresh_signal_handler_free);

  connect_refresh_signals_for_object(self, G_OBJECT(self->client), client_signals);

  devices = nm_client_get_devices(self->client);
  for (guint i = 0; devices != NULL && i < devices->len; i++) {
    NMDevice *device = g_ptr_array_index((GPtrArray *) devices, i);
    connect_refresh_signals_for_object(self,
                                       G_OBJECT(device),
                                       nm_device_get_device_type(device) == NM_DEVICE_TYPE_WIFI ? wifi_device_signals : device_signals);
    if (nm_device_get_device_type(device) == NM_DEVICE_TYPE_WIFI) {
      const GPtrArray *access_points = nm_device_wifi_get_access_points(NM_DEVICE_WIFI(device));
      for (guint j = 0; access_points != NULL && j < access_points->len; j++) {
        NMAccessPoint *ap = g_ptr_array_index((GPtrArray *) access_points, j);
        connect_refresh_signals_for_object(self, G_OBJECT(ap), access_point_signals);
      }
    }
  }

  active_connections = nm_client_get_active_connections(self->client);
  for (guint i = 0; active_connections != NULL && i < active_connections->len; i++) {
    NMActiveConnection *active = g_ptr_array_index((GPtrArray *) active_connections, i);
    connect_refresh_signals_for_object(self,
                                       G_OBJECT(active),
                                       NM_IS_VPN_CONNECTION(active) ? vpn_connection_signals : active_connection_signals);
  }

  connections = nm_client_get_connections(self->client);
  for (guint i = 0; connections != NULL && i < connections->len; i++) {
    NMRemoteConnection *connection = g_ptr_array_index((GPtrArray *) connections, i);
    connect_refresh_signals_for_object(self, G_OBJECT(connection), remote_connection_signals);
  }
}

static gboolean
on_overlay_get_child_position(GtkOverlay *overlay,
                              GtkWidget *widget,
                              GdkRectangle *allocation,
                              gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;
  int surface_width;
  int panel_width;

  if (widget != self->toast_overlay)
    return FALSE;

  surface_width = MAX(0, gtk_widget_get_width(GTK_WIDGET(overlay)));
  panel_width = MIN(NETWORK_SIDEBAR_WIDTH, surface_width);
  allocation->x = MAX(0, surface_width - panel_width);
  allocation->y = 0;
  allocation->width = panel_width;
  allocation->height = MAX(0, gtk_widget_get_height(GTK_WIDGET(overlay)));
  return TRUE;
}

static gboolean
point_in_panel(NetworkSidebarGuiApp *self, double x, double y)
{
  GtkAllocation allocation;

  if (!on_overlay_get_child_position(GTK_OVERLAY(self->surface), self->toast_overlay, &allocation, self))
    return FALSE;
  return x >= allocation.x &&
         x < allocation.x + allocation.width &&
         y >= allocation.y &&
         y < allocation.y + allocation.height;
}

static void
on_surface_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;
  (void) gesture;
  (void) n_press;

  self->outside_click_started = !point_in_panel(self, x, y);
}

static void
on_surface_click_released(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;
  (void) gesture;
  (void) n_press;

  if (self->outside_click_started && !point_in_panel(self, x, y))
    hide_sidebar(self);
  self->outside_click_started = FALSE;
}

static gboolean
on_key_pressed(GtkEventControllerKey *controller,
               guint keyval,
               guint keycode,
               GdkModifierType state,
               gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;
  (void) controller;
  (void) keycode;
  (void) state;

  if (keyval == GDK_KEY_Escape) {
    hide_sidebar(self);
    return TRUE;
  }
  return FALSE;
}

static gboolean
on_close_request(GtkWindow *window, gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;
  (void) window;

  hide_sidebar(self);
  return TRUE;
}

static void
on_back_clicked(GtkButton *button, gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;
  (void) button;

  self->showing_connection_information = FALSE;
  refresh_content(self);
}

static void
on_info_clicked(GtkButton *button, gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;
  (void) button;

  self->showing_connection_information = TRUE;
  refresh_content(self);
}

static void
on_networking_switch(GtkSwitch *switch_widget, GParamSpec *pspec, gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;
  (void) pspec;

  if (self->refreshing || self->actions == NULL)
    return;
  network_sidebar_actions_set_networking_enabled(self->actions, gtk_switch_get_active(switch_widget));
}

static void
scan_wifi_on_show(NetworkSidebarGuiApp *self)
{
  const GPtrArray *devices;

  if (self->showing_connection_information || self->actions == NULL)
    return;
  if (network_sidebar_actions_get_wifi_scan_in_progress(self->actions))
    return;
  if (!nm_client_networking_get_enabled(self->client) ||
      !nm_client_wireless_hardware_get_enabled(self->client) ||
      !nm_client_wireless_get_enabled(self->client))
    return;

  devices = nm_client_get_devices(self->client);
  for (guint i = 0; devices != NULL && i < devices->len; i++) {
    NMDevice *device = g_ptr_array_index((GPtrArray *) devices, i);
    if (nm_device_get_device_type(device) == NM_DEVICE_TYPE_WIFI) {
      network_sidebar_actions_request_scan(self->actions);
      return;
    }
  }
}

static void
on_window_map(GtkWidget *widget, gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;
  g_autoptr(GError) error = NULL;
  (void) widget;

  if (!network_sidebar_configure_layer_shell_window(GTK_WINDOW(self->window), self->toast_overlay, self->target_output_name, &error)) {
    fail_layer_shell(self, error != NULL ? error->message : NETWORK_SIDEBAR_LAYER_SHELL_REQUIRED_MESSAGE);
    return;
  }
  update_scroll_fades(self);
  scan_wifi_on_show(self);
}

static GtkWidget *
create_scroll_fade(const char *css_class, GtkAlign valign)
{
  GtkWidget *fade = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(fade, "network-scroll-fade");
  gtk_widget_add_css_class(fade, css_class);
  gtk_widget_set_can_target(fade, FALSE);
  gtk_widget_set_halign(fade, GTK_ALIGN_FILL);
  gtk_widget_set_valign(fade, valign);
  gtk_widget_set_hexpand(fade, TRUE);
  gtk_widget_set_size_request(fade, -1, 44);
  gtk_widget_set_opacity(fade, 0.0);
  return fade;
}

static void
update_scroll_fades(NetworkSidebarGuiApp *self)
{
  GtkAdjustment *adjustment;
  double lower;
  double upper;
  double page_size;
  double value;
  double max_value;
  gboolean can_scroll;

  if (self->scrolled == NULL || self->scroll_fade_top == NULL || self->scroll_fade_bottom == NULL)
    return;
  adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scrolled));
  lower = gtk_adjustment_get_lower(adjustment);
  upper = gtk_adjustment_get_upper(adjustment);
  page_size = gtk_adjustment_get_page_size(adjustment);
  value = gtk_adjustment_get_value(adjustment);
  max_value = MAX(lower, upper - page_size);
  can_scroll = upper - lower > page_size + 0.5;
  gtk_widget_set_opacity(self->scroll_fade_top, can_scroll && value > lower + 0.5 ? 1.0 : 0.0);
  gtk_widget_set_opacity(self->scroll_fade_bottom, can_scroll && value < max_value - 0.5 ? 1.0 : 0.0);
}

static void
on_scroll_adjustment_changed(GtkAdjustment *adjustment, gpointer user_data)
{
  (void) adjustment;
  update_scroll_fades(user_data);
}

static gboolean
ensure_window(NetworkSidebarGuiApp *self)
{
  g_autofree char *unavailable_message = NULL;
  g_autoptr(GError) error = NULL;
  GtkWidget *panel_content;
  GtkWidget *top_bar;
  GtkWidget *scroll_overlay;
  GtkWidget *click_area;
  GtkGesture *click_controller;
  GtkEventController *key_controller;

  if (self->window != NULL)
    return TRUE;

  unavailable_message = network_sidebar_layer_shell_unavailable_message();
  if (unavailable_message != NULL) {
    fail_layer_shell(self, unavailable_message);
    return FALSE;
  }

  self->window = adw_application_window_new(GTK_APPLICATION(self->application));
  if (self->actions != NULL)
    network_sidebar_actions_set_parent(self->actions, GTK_WINDOW(self->window));
  gtk_window_set_title(GTK_WINDOW(self->window), NETWORK_SIDEBAR_APP_NAME);
  gtk_window_set_default_size(GTK_WINDOW(self->window), 1, 1);
  gtk_widget_set_size_request(self->window, 1, 1);
  gtk_window_set_decorated(GTK_WINDOW(self->window), FALSE);
  gtk_widget_add_css_class(self->window, "nm-sidebar-window");

  if (!network_sidebar_initialize_layer_shell_window(GTK_WINDOW(self->window), &error)) {
    fail_layer_shell(self, error != NULL ? error->message : NETWORK_SIDEBAR_LAYER_SHELL_REQUIRED_MESSAGE);
    return FALSE;
  }

  self->surface = gtk_overlay_new();
  gtk_widget_add_css_class(self->surface, "nm-sidebar-surface");
  gtk_widget_set_hexpand(self->surface, TRUE);
  gtk_widget_set_vexpand(self->surface, TRUE);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(self->window), self->surface);

  click_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(click_area, TRUE);
  gtk_widget_set_vexpand(click_area, TRUE);
  gtk_overlay_set_child(GTK_OVERLAY(self->surface), click_area);

  self->toast_overlay = adw_toast_overlay_new();
  gtk_widget_add_css_class(self->toast_overlay, "nm-sidebar-panel");
  gtk_widget_set_hexpand(self->toast_overlay, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->surface), self->toast_overlay);
  g_signal_connect(self->surface, "get-child-position", G_CALLBACK(on_overlay_get_child_position), self);

  if (!network_sidebar_configure_layer_shell_window(GTK_WINDOW(self->window), self->toast_overlay, self->target_output_name, &error)) {
    fail_layer_shell(self, error != NULL ? error->message : NETWORK_SIDEBAR_LAYER_SHELL_REQUIRED_MESSAGE);
    return FALSE;
  }

  panel_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_vexpand(panel_content, TRUE);
  gtk_widget_set_valign(panel_content, GTK_ALIGN_FILL);
  gtk_widget_set_margin_top(panel_content, 12);
  gtk_widget_set_margin_bottom(panel_content, 12);
  gtk_widget_set_margin_start(panel_content, 12);
  gtk_widget_set_margin_end(panel_content, 12);
  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(self->toast_overlay), panel_content);

  top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(panel_content), top_bar);

  self->back_button = gtk_button_new_from_icon_name("go-previous-symbolic");
  gtk_widget_add_css_class(self->back_button, "flat");
  gtk_widget_set_tooltip_text(self->back_button, "Back to networks");
  gtk_widget_set_valign(self->back_button, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(self->back_button, FALSE);
  g_signal_connect(self->back_button, "clicked", G_CALLBACK(on_back_clicked), self);
  gtk_box_append(GTK_BOX(top_bar), self->back_button);

  self->title = gtk_label_new("Network");
  gtk_widget_add_css_class(self->title, "title-2");
  gtk_label_set_xalign(GTK_LABEL(self->title), 0.0f);
  gtk_widget_set_hexpand(self->title, TRUE);
  gtk_box_append(GTK_BOX(top_bar), self->title);

  self->info_button = gtk_button_new_from_icon_name("dialog-information-symbolic");
  gtk_widget_add_css_class(self->info_button, "flat");
  gtk_widget_set_tooltip_text(self->info_button, "Connection information");
  gtk_widget_set_valign(self->info_button, GTK_ALIGN_CENTER);
  g_signal_connect(self->info_button, "clicked", G_CALLBACK(on_info_clicked), self);
  gtk_box_append(GTK_BOX(top_bar), self->info_button);

  self->networking_switch = gtk_switch_new();
  gtk_widget_set_tooltip_text(self->networking_switch, "Networking");
  gtk_widget_set_valign(self->networking_switch, GTK_ALIGN_CENTER);
  g_signal_connect(self->networking_switch, "notify::active", G_CALLBACK(on_networking_switch), self);
  gtk_box_append(GTK_BOX(top_bar), self->networking_switch);

  scroll_overlay = gtk_overlay_new();
  gtk_widget_set_vexpand(scroll_overlay, TRUE);
  gtk_box_append(GTK_BOX(panel_content), scroll_overlay);

  self->scrolled = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(self->scrolled, TRUE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scrolled), GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
  gtk_overlay_set_child(GTK_OVERLAY(scroll_overlay), self->scrolled);

  self->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scrolled), self->content);

  self->scroll_fade_top = create_scroll_fade("network-scroll-fade-top", GTK_ALIGN_START);
  self->scroll_fade_bottom = create_scroll_fade("network-scroll-fade-bottom", GTK_ALIGN_END);
  gtk_overlay_add_overlay(GTK_OVERLAY(scroll_overlay), self->scroll_fade_top);
  gtk_overlay_add_overlay(GTK_OVERLAY(scroll_overlay), self->scroll_fade_bottom);
  {
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scrolled));
    g_signal_connect(adjustment, "value-changed", G_CALLBACK(on_scroll_adjustment_changed), self);
    g_signal_connect(adjustment, "notify::lower", G_CALLBACK(on_scroll_adjustment_changed), self);
    g_signal_connect(adjustment, "notify::upper", G_CALLBACK(on_scroll_adjustment_changed), self);
    g_signal_connect(adjustment, "notify::page-size", G_CALLBACK(on_scroll_adjustment_changed), self);
  }

  click_controller = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_controller), 0);
  g_signal_connect(click_controller, "pressed", G_CALLBACK(on_surface_click_pressed), self);
  g_signal_connect(click_controller, "released", G_CALLBACK(on_surface_click_released), self);
  gtk_widget_add_controller(self->surface, GTK_EVENT_CONTROLLER(click_controller));

  key_controller = gtk_event_controller_key_new();
  g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), self);
  gtk_widget_add_controller(self->window, key_controller);

  g_signal_connect(self->window, "close-request", G_CALLBACK(on_close_request), self);
  g_signal_connect(self->window, "map", G_CALLBACK(on_window_map), self);

  ensure_actions(self);
  sync_refresh_signals(self);
  if (self->periodic_refresh_source == 0)
    self->periodic_refresh_source = g_timeout_add_seconds(60, periodic_refresh_cb, self);
  return TRUE;
}

static gboolean
ensure_command_server(NetworkSidebarGuiApp *self, gboolean repair)
{
  g_autoptr(GError) error = NULL;

  if (self->server != NULL) {
    if (!repair || network_sidebar_command_server_is_healthy(self->server))
      return TRUE;
    network_sidebar_command_server_close(self->server);
    self->server = NULL;
  }

  self->server = network_sidebar_command_server_new(self, &error);
  if (self->server == NULL) {
    fail_runtime(self, error != NULL ? error->message : "could not start command server");
    return FALSE;
  }
  return TRUE;
}

static gboolean
prepare_runtime(NetworkSidebarGuiApp *self)
{
  if (!ensure_nm_client(self))
    return FALSE;
  ensure_actions(self);
  if (!ensure_window(self))
    return FALSE;
  return ensure_command_server(self, FALSE);
}

static void
set_target_output_name(NetworkSidebarGuiApp *self, const char *target_output_name)
{
  g_autofree char *normalized = network_sidebar_normalize_target_output(target_output_name);
  g_autoptr(GError) error = NULL;

  if (normalized == NULL)
    return;
  g_free(self->target_output_name);
  self->target_output_name = g_steal_pointer(&normalized);
  if (self->window != NULL &&
      !network_sidebar_configure_layer_shell_window(GTK_WINDOW(self->window), self->toast_overlay, self->target_output_name, &error))
    fail_layer_shell(self, error != NULL ? error->message : NETWORK_SIDEBAR_LAYER_SHELL_REQUIRED_MESSAGE);
}

static void
show_sidebar(NetworkSidebarGuiApp *self)
{
  g_autoptr(GError) error = NULL;

  if (!ensure_window(self))
    return;
  if (!network_sidebar_configure_layer_shell_window(GTK_WINDOW(self->window), self->toast_overlay, self->target_output_name, &error)) {
    fail_layer_shell(self, error != NULL ? error->message : NETWORK_SIDEBAR_LAYER_SHELL_REQUIRED_MESSAGE);
    return;
  }
  gtk_window_present(GTK_WINDOW(self->window));
  schedule_refresh(self, 1, TRUE);
}

static void
hide_sidebar(NetworkSidebarGuiApp *self)
{
  if (self->window != NULL)
    gtk_widget_set_visible(self->window, FALSE);
}

static void
toggle_sidebar(NetworkSidebarGuiApp *self)
{
  if (!ensure_window(self))
    return;
  if (gtk_widget_get_visible(self->window))
    hide_sidebar(self);
  else
    show_sidebar(self);
}

gboolean
network_sidebar_gui_app_handle_command(NetworkSidebarGuiApp *self,
                                       const char *command,
                                       const char *target_output_name)
{
  g_autofree char *normalized_command = NULL;

  g_return_val_if_fail(self != NULL, FALSE);

  normalized_command = network_sidebar_normalize_command(command);
  set_target_output_name(self, target_output_name);

  if (g_strcmp0(normalized_command, NETWORK_SIDEBAR_COMMAND_SHOW) == 0)
    show_sidebar(self);
  else if (g_strcmp0(normalized_command, NETWORK_SIDEBAR_COMMAND_HIDE) == 0)
    hide_sidebar(self);
  else if (g_strcmp0(normalized_command, NETWORK_SIDEBAR_COMMAND_QUIT) == 0)
    g_application_quit(G_APPLICATION(self->application));
  else if (g_strcmp0(normalized_command, NETWORK_SIDEBAR_COMMAND_TOGGLE) == 0)
    toggle_sidebar(self);

  return FALSE;
}

static gboolean
refresh_timeout_cb(gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;
  self->refresh_source = 0;
  if (self->refresh_visible_only && (self->window == NULL || !gtk_widget_get_visible(self->window)))
    return G_SOURCE_REMOVE;
  refresh_content(self);
  return G_SOURCE_REMOVE;
}

static gboolean
periodic_refresh_cb(gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;
  if (self->window != NULL && gtk_widget_get_visible(self->window))
    refresh_content(self);
  return G_SOURCE_CONTINUE;
}

static gboolean
signal_refresh_cb(gpointer user_data)
{
  NetworkSidebarGuiApp *self = user_data;
  self->signal_refresh_source = 0;
  if (self->window != NULL && gtk_widget_get_visible(self->window))
    refresh_content(self);
  return G_SOURCE_REMOVE;
}

static void
schedule_refresh(NetworkSidebarGuiApp *self, guint delay_ms, gboolean visible_only)
{
  if (self->refresh_source != 0)
    return;
  self->refresh_visible_only = visible_only;
  self->refresh_source = g_timeout_add(delay_ms, refresh_timeout_cb, self);
}

static void
refresh_content(NetworkSidebarGuiApp *self)
{
  if (self->content == NULL || self->client == NULL || self->actions == NULL)
    return;

  sync_refresh_signals(self);
  self->refreshing = TRUE;
  gtk_switch_set_active(GTK_SWITCH(self->networking_switch), nm_client_networking_get_enabled(self->client));
  network_sidebar_clear_box(GTK_BOX(self->content));

  if (self->showing_connection_information) {
    gtk_label_set_label(GTK_LABEL(self->title), "Connection Information");
    gtk_widget_set_visible(self->back_button, TRUE);
    gtk_widget_set_visible(self->info_button, FALSE);
    gtk_widget_set_visible(self->networking_switch, FALSE);
    network_sidebar_add_connection_info_content(GTK_BOX(self->content), self->client);
  } else {
    gtk_label_set_label(GTK_LABEL(self->title), "Network");
    gtk_widget_set_visible(self->back_button, FALSE);
    gtk_widget_set_visible(self->info_button, TRUE);
    gtk_widget_set_visible(self->networking_switch, TRUE);
    network_sidebar_add_status_group(GTK_BOX(self->content), self->client, self->actions);
    network_sidebar_add_vpn_group(GTK_BOX(self->content), self->client, self->actions);
    network_sidebar_add_wifi_group(GTK_BOX(self->content), self->client, self->actions);
  }
  self->refreshing = FALSE;
  update_scroll_fades(self);
}
