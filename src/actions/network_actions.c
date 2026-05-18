#include "actions/network_actions.h"

#include "data/labels.h"
#include "sections/helpers.h"

#include <adwaita.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#define WIFI_SCAN_THROTTLE_SECONDS 15
#define WIFI_PASSWORD_MIN_LENGTH 8
#define WIFI_PASSWORD_REQUIRED_MESSAGE "Password is required"
#define WIFI_PASSWORD_TOO_SHORT_MESSAGE "Wi-Fi password must be at least 8 characters"

struct _NetworkSidebarActions {
  gint ref_count;
  NMClient *client;
  GtkWindow *parent;
  NetworkSidebarToastCallback toast;
  NetworkSidebarScheduleRefreshCallback schedule_refresh;
  gpointer user_data;
  gboolean wifi_scan_in_progress;
  guint wifi_scan_pending_requests;
  guint wifi_scan_settle_source;
  AdwToast *wifi_scan_toast;
  char *wifi_scan_error;
  gint64 last_scan_us;
  GHashTable *activation_watches;
};

typedef struct {
  NetworkSidebarActions *actions;
  char *name;
  gboolean is_vpn;
  gboolean is_wifi;
  gboolean retry_wifi_auth;
  char *temp_uuid;
  NMDeviceWifi *wifi_device;
  NMAccessPoint *wifi_ap;
  NMRemoteConnection *saved_wifi;
  NMRemoteConnection *connection;
} AsyncAction;

typedef struct {
  NetworkSidebarActions *actions;
  NMActiveConnection *active;
  char *name;
  gboolean is_vpn;
  gboolean is_wifi;
  gboolean retry_wifi_auth;
  char *temp_uuid;
  NMDeviceWifi *wifi_device;
  NMAccessPoint *wifi_ap;
  NMRemoteConnection *saved_wifi;
  guint checks_remaining;
  guint source_id;
  char *key;
} ActivationWatch;

static AdwToast *toast_with_handle(NetworkSidebarActions *actions, const char *message);
static void toast(NetworkSidebarActions *actions, const char *message);
static void schedule_refresh(NetworkSidebarActions *actions, guint delay_ms);
static void dismiss_wifi_scan_toast(NetworkSidebarActions *actions);
static void activation_watch_free(ActivationWatch *watch);
static void prompt_wifi_password(NetworkSidebarActions *actions,
                                 NMDeviceWifi *device,
                                 NMAccessPoint *ap,
                                 NMRemoteConnection *saved_wifi);

NetworkSidebarActions *
network_sidebar_actions_new(NMClient *client,
                            NetworkSidebarToastCallback toast_cb,
                            NetworkSidebarScheduleRefreshCallback schedule_refresh_cb,
                            gpointer user_data)
{
  NetworkSidebarActions *actions = g_new0(NetworkSidebarActions, 1);
  actions->ref_count = 1;
  actions->client = g_object_ref(client);
  actions->toast = toast_cb;
  actions->schedule_refresh = schedule_refresh_cb;
  actions->user_data = user_data;
  actions->activation_watches = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify) activation_watch_free);
  return actions;
}

NetworkSidebarActions *
network_sidebar_actions_ref(NetworkSidebarActions *actions)
{
  g_atomic_int_inc(&actions->ref_count);
  return actions;
}

void
network_sidebar_actions_unref(NetworkSidebarActions *actions)
{
  if (actions == NULL)
    return;
  if (!g_atomic_int_dec_and_test(&actions->ref_count))
    return;
  if (actions->wifi_scan_settle_source != 0)
    g_source_remove(actions->wifi_scan_settle_source);
  dismiss_wifi_scan_toast(actions);
  g_free(actions->wifi_scan_error);
  g_clear_pointer(&actions->activation_watches, g_hash_table_unref);
  g_clear_object(&actions->client);
  g_free(actions);
}

void
network_sidebar_actions_set_parent(NetworkSidebarActions *actions, GtkWindow *parent)
{
  if (actions == NULL)
    return;
  actions->parent = parent;
}

static AsyncAction *
async_action_new(NetworkSidebarActions *actions, const char *name)
{
  AsyncAction *async = g_new0(AsyncAction, 1);
  async->actions = network_sidebar_actions_ref(actions);
  async->name = g_strdup(name != NULL ? name : "connection");
  return async;
}

static void
async_action_free(AsyncAction *async)
{
  if (async == NULL)
    return;
  network_sidebar_actions_unref(async->actions);
  g_clear_object(&async->wifi_device);
  g_clear_object(&async->wifi_ap);
  g_clear_object(&async->saved_wifi);
  g_clear_object(&async->connection);
  g_free(async->temp_uuid);
  g_free(async->name);
  g_free(async);
}

static gboolean
ap_has_ssid(NMAccessPoint *ap)
{
  GBytes *ssid;
  gsize length = 0;

  if (ap == NULL)
    return FALSE;
  ssid = nm_access_point_get_ssid(ap);
  if (ssid == NULL)
    return FALSE;
  g_bytes_get_data(ssid, &length);
  return length > 0;
}

static gboolean
ap_is_secure(NMAccessPoint *ap)
{
  return (nm_access_point_get_flags(ap) & NM_802_11_AP_FLAGS_PRIVACY) != 0 ||
         nm_access_point_get_wpa_flags(ap) != NM_802_11_AP_SEC_NONE ||
         nm_access_point_get_rsn_flags(ap) != NM_802_11_AP_SEC_NONE;
}

static gboolean
ap_has_security(NMAccessPoint *ap, NM80211ApSecurityFlags flag)
{
  NM80211ApSecurityFlags security = nm_access_point_get_wpa_flags(ap) | nm_access_point_get_rsn_flags(ap);
  return (security & flag) != 0;
}

static gboolean
ap_supports_owe(NMAccessPoint *ap)
{
  return ap_has_security(ap, NM_802_11_AP_SEC_KEY_MGMT_OWE) ||
         ap_has_security(ap, NM_802_11_AP_SEC_KEY_MGMT_OWE_TM);
}

static gboolean
ap_needs_password(NMAccessPoint *ap)
{
  return ap_has_security(ap, NM_802_11_AP_SEC_KEY_MGMT_PSK) ||
         ap_has_security(ap, NM_802_11_AP_SEC_KEY_MGMT_SAE);
}

static char *
access_point_context(NMAccessPoint *ap)
{
  g_autoptr(GString) parts = g_string_new(NULL);
  const char *bssid = nm_access_point_get_bssid(ap);
  guint32 frequency = nm_access_point_get_frequency(ap);

  if (bssid != NULL && *bssid != '\0')
    g_string_append_printf(parts, "BSSID %s", bssid);
  if (frequency != 0) {
    if (parts->len > 0)
      g_string_append(parts, " - ");
    g_string_append_printf(parts, "%u MHz", frequency);
  }
  return parts->len > 0 ? g_string_free(g_steal_pointer(&parts), FALSE) : NULL;
}

static const char *
password_key_mgmt(NMAccessPoint *ap, NMRemoteConnection *existing)
{
  if (existing != NULL) {
    NMSettingWirelessSecurity *security = nm_connection_get_setting_wireless_security(NM_CONNECTION(existing));
    const char *key_mgmt = security != NULL ? nm_setting_wireless_security_get_key_mgmt(security) : NULL;
    if (g_strcmp0(key_mgmt, "sae") == 0 && ap_has_security(ap, NM_802_11_AP_SEC_KEY_MGMT_SAE))
      return "sae";
    if (g_strcmp0(key_mgmt, "wpa-psk") == 0 && ap_has_security(ap, NM_802_11_AP_SEC_KEY_MGMT_PSK))
      return "wpa-psk";
  }
  if (ap_has_security(ap, NM_802_11_AP_SEC_KEY_MGMT_SAE))
    return "sae";
  return "wpa-psk";
}

static NMConnection *
create_wifi_connection(NMAccessPoint *ap, const char *password, char **out_uuid)
{
  g_autofree char *ssid = network_sidebar_ap_ssid_text(ap);
  g_autofree char *uuid = nm_utils_uuid_generate();
  NMConnection *connection = nm_simple_connection_new();
  NMSetting *s_con = nm_setting_connection_new();
  NMSetting *s_wifi = nm_setting_wireless_new();
  NMSetting *s_ip4 = nm_setting_ip4_config_new();
  NMSetting *s_ip6 = nm_setting_ip6_config_new();
  GBytes *ssid_bytes = nm_access_point_get_ssid(ap);

  g_object_set(s_con,
               NM_SETTING_CONNECTION_ID, ssid,
               NM_SETTING_CONNECTION_UUID, uuid,
               NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRELESS_SETTING_NAME,
               NM_SETTING_CONNECTION_AUTOCONNECT, TRUE,
               NULL);
  g_object_set(s_wifi, NM_SETTING_WIRELESS_SSID, ssid_bytes, NULL);
  g_object_set(s_ip4, NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO, NULL);
  g_object_set(s_ip6, NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_AUTO, NULL);

  nm_connection_add_setting(connection, s_con);
  nm_connection_add_setting(connection, s_wifi);
  nm_connection_add_setting(connection, s_ip4);
  nm_connection_add_setting(connection, s_ip6);

  if (password != NULL || ap_supports_owe(ap)) {
    NMSetting *s_security = nm_setting_wireless_security_new();
    if (password != NULL) {
      g_object_set(s_security,
                   NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, password_key_mgmt(ap, NULL),
                   NM_SETTING_WIRELESS_SECURITY_PSK, password,
                   NULL);
    } else {
      g_object_set(s_security, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "owe", NULL);
    }
    nm_connection_add_setting(connection, s_security);
  }

  if (out_uuid != NULL)
    *out_uuid = g_strdup(uuid);
  return connection;
}

typedef struct {
  AdwAlertDialog *dialog;
  char *response_id;
  char *response_label;
  char *message;
  GtkWidget *response_button;
} DisabledResponseTooltip;

static void
disabled_response_tooltip_free(DisabledResponseTooltip *tooltip)
{
  if (tooltip == NULL)
    return;
  g_free(tooltip->response_id);
  g_free(tooltip->response_label);
  g_free(tooltip->message);
  g_free(tooltip);
}

static gboolean
widget_contains_label(GtkWidget *widget, const char *label)
{
  GtkWidget *child;

  if (GTK_IS_LABEL(widget) && g_strcmp0(gtk_label_get_text(GTK_LABEL(widget)), label) == 0)
    return TRUE;

  for (child = gtk_widget_get_first_child(widget); child != NULL; child = gtk_widget_get_next_sibling(child)) {
    if (widget_contains_label(child, label))
      return TRUE;
  }
  return FALSE;
}

static GtkWidget *
find_response_button(GtkWidget *widget, const char *label)
{
  GtkWidget *child;

  if (GTK_IS_BUTTON(widget) &&
      (g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label) == 0 || widget_contains_label(widget, label)))
    return widget;

  for (child = gtk_widget_get_first_child(widget); child != NULL; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *button = find_response_button(child, label);
    if (button != NULL)
      return button;
  }
  return NULL;
}

static gboolean
widget_has_ancestor(GtkWidget *widget, GtkWidget *ancestor)
{
  for (GtkWidget *current = widget; current != NULL; current = gtk_widget_get_parent(current)) {
    if (current == ancestor)
      return TRUE;
  }
  return FALSE;
}

static gboolean
sync_disabled_response_tooltip(DisabledResponseTooltip *tooltip)
{
  if (tooltip->response_button == NULL)
    tooltip->response_button = find_response_button(GTK_WIDGET(tooltip->dialog), tooltip->response_label);
  if (tooltip->response_button != NULL) {
    const char *text = adw_alert_dialog_get_response_enabled(tooltip->dialog, tooltip->response_id) ? NULL : tooltip->message;
    gtk_widget_set_tooltip_text(tooltip->response_button, text);
  }
  return G_SOURCE_REMOVE;
}

static void
on_disabled_response_dialog_mapped(GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  sync_disabled_response_tooltip(user_data);
}

static gboolean
on_disabled_response_query_tooltip(GtkWidget *widget,
                                   int x,
                                   int y,
                                   gboolean keyboard_mode,
                                   GtkTooltip *gtk_tooltip,
                                   gpointer user_data)
{
  DisabledResponseTooltip *tooltip = user_data;
  GtkWidget *picked;

  if (keyboard_mode || adw_alert_dialog_get_response_enabled(tooltip->dialog, tooltip->response_id))
    return FALSE;

  sync_disabled_response_tooltip(tooltip);
  if (tooltip->response_button == NULL)
    return FALSE;

  picked = gtk_widget_pick(widget, x, y, GTK_PICK_INSENSITIVE);
  if (picked == NULL || !widget_has_ancestor(picked, tooltip->response_button))
    return FALSE;

  gtk_tooltip_set_text(gtk_tooltip, tooltip->message);
  return TRUE;
}

static DisabledResponseTooltip *
attach_disabled_response_tooltip(AdwAlertDialog *dialog,
                                 const char *response_id,
                                 const char *response_label,
                                 const char *message)
{
  DisabledResponseTooltip *tooltip = g_new0(DisabledResponseTooltip, 1);

  tooltip->dialog = dialog;
  tooltip->response_id = g_strdup(response_id);
  tooltip->response_label = g_strdup(response_label);
  tooltip->message = g_strdup(message);
  g_signal_connect(dialog, "map", G_CALLBACK(on_disabled_response_dialog_mapped), tooltip);
  gtk_widget_set_has_tooltip(GTK_WIDGET(dialog), TRUE);
  g_signal_connect(dialog, "query-tooltip", G_CALLBACK(on_disabled_response_query_tooltip), tooltip);
  return tooltip;
}

static void
on_dialog_backdrop_released(GtkGestureClick *gesture,
                            int n_press,
                            double x,
                            double y,
                            gpointer user_data)
{
  AdwDialog *dialog = user_data;
  GtkWidget *picked;
  (void) n_press;

  picked = gtk_widget_pick(GTK_WIDGET(dialog), x, y, GTK_PICK_DEFAULT);
  if (picked != NULL && g_strcmp0(gtk_widget_get_css_name(picked), "dimming") == 0) {
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    adw_dialog_close(dialog);
  }
}

static void
close_dialog_on_backdrop_click(AdwDialog *dialog)
{
  GtkGesture *backdrop_click = gtk_gesture_click_new();

  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(backdrop_click), GTK_PHASE_CAPTURE);
  g_signal_connect(backdrop_click, "released", G_CALLBACK(on_dialog_backdrop_released), dialog);
  gtk_widget_add_controller(GTK_WIDGET(dialog), GTK_EVENT_CONTROLLER(backdrop_click));
}

typedef struct {
  NetworkSidebarActions *actions;
  char *uuid;
  char *ssid;
  guint retries;
} TemporaryWifiCleanup;

static void temporary_wifi_cleanup_attempt(TemporaryWifiCleanup *cleanup);

static void
temporary_wifi_cleanup_free(TemporaryWifiCleanup *cleanup)
{
  if (cleanup == NULL)
    return;
  network_sidebar_actions_unref(cleanup->actions);
  g_free(cleanup->uuid);
  g_free(cleanup->ssid);
  g_free(cleanup);
}

static void
temporary_wifi_cleanup_delete_finish_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  TemporaryWifiCleanup *cleanup = user_data;
  g_autoptr(GError) error = NULL;

  if (!nm_remote_connection_delete_finish(NM_REMOTE_CONNECTION(source), result, &error)) {
    g_autofree char *message = g_strdup_printf("Could not remove failed Wi-Fi profile for %s: %s",
                                               cleanup->ssid != NULL ? cleanup->ssid : "Wi-Fi network",
                                               error != NULL ? error->message : "unknown error");
    toast(cleanup->actions, message);
  }
  schedule_refresh(cleanup->actions, 700);
  temporary_wifi_cleanup_free(cleanup);
}

static gboolean
temporary_wifi_cleanup_retry_cb(gpointer user_data)
{
  temporary_wifi_cleanup_attempt(user_data);
  return G_SOURCE_REMOVE;
}

static void
temporary_wifi_cleanup_attempt(TemporaryWifiCleanup *cleanup)
{
  NMRemoteConnection *connection = nm_client_get_connection_by_uuid(cleanup->actions->client, cleanup->uuid);

  if (connection == NULL) {
    if (cleanup->retries > 0) {
      cleanup->retries--;
      g_timeout_add(250, temporary_wifi_cleanup_retry_cb, cleanup);
    } else {
      temporary_wifi_cleanup_free(cleanup);
    }
    return;
  }

  if (g_strcmp0(nm_connection_get_connection_type(NM_CONNECTION(connection)), NM_SETTING_WIRELESS_SETTING_NAME) != 0) {
    temporary_wifi_cleanup_free(cleanup);
    return;
  }

  nm_remote_connection_delete_async(connection, NULL, temporary_wifi_cleanup_delete_finish_cb, cleanup);
}

static void
cleanup_temporary_wifi_profile(NetworkSidebarActions *actions, const char *uuid, const char *ssid)
{
  TemporaryWifiCleanup *cleanup;

  if (uuid == NULL)
    return;

  cleanup = g_new0(TemporaryWifiCleanup, 1);
  cleanup->actions = network_sidebar_actions_ref(actions);
  cleanup->uuid = g_strdup(uuid);
  cleanup->ssid = g_strdup(ssid);
  cleanup->retries = 8;
  temporary_wifi_cleanup_attempt(cleanup);
}

static gboolean
reason_looks_like_auth_failure(NMActiveConnectionStateReason reason)
{
  return reason == NM_ACTIVE_CONNECTION_STATE_REASON_LOGIN_FAILED ||
         reason == NM_ACTIVE_CONNECTION_STATE_REASON_NO_SECRETS;
}

static gboolean
error_looks_like_auth_failure(const GError *error)
{
  g_autofree char *lower = NULL;

  if (error == NULL || error->message == NULL)
    return FALSE;
  lower = g_ascii_strdown(error->message, -1);
  return strstr(lower, "secret") != NULL ||
         strstr(lower, "password") != NULL ||
         strstr(lower, "psk") != NULL ||
         strstr(lower, "802.1x") != NULL ||
         strstr(lower, "supplicant") != NULL ||
         strstr(lower, "login") != NULL;
}

static void
activation_watch_free(ActivationWatch *watch)
{
  if (watch == NULL)
    return;
  if (watch->source_id != 0)
    g_source_remove(watch->source_id);
  network_sidebar_actions_unref(watch->actions);
  g_clear_object(&watch->active);
  g_clear_object(&watch->wifi_device);
  g_clear_object(&watch->wifi_ap);
  g_clear_object(&watch->saved_wifi);
  g_free(watch->temp_uuid);
  g_free(watch->key);
  g_free(watch->name);
  g_free(watch);
}

static char *
activation_watch_key(NMActiveConnection *active, const char *fallback)
{
  const char *path = active != NULL ? nm_object_get_path(NM_OBJECT(active)) : NULL;

  if (path != NULL && *path != '\0')
    return g_strdup(path);
  return g_strdup(fallback != NULL && *fallback != '\0' ? fallback : "connection");
}

static gboolean
activation_reason_is_user_disconnected(NMActiveConnectionStateReason reason)
{
#ifdef NM_ACTIVE_CONNECTION_STATE_REASON_USER_DISCONNECTED
  return reason == NM_ACTIVE_CONNECTION_STATE_REASON_USER_DISCONNECTED;
#else
  (void) reason;
  return FALSE;
#endif
}

static char *
activation_failed_message(const char *prefix, const char *name, NMActiveConnectionStateReason reason)
{
  if (reason == NM_ACTIVE_CONNECTION_STATE_REASON_NONE || reason == NM_ACTIVE_CONNECTION_STATE_REASON_UNKNOWN)
    return g_strdup_printf("%s failed to connect: %s", prefix, name);
  {
    g_autofree char *reason_label = network_sidebar_enum_label(NM_TYPE_ACTIVE_CONNECTION_STATE_REASON, reason, "unknown reason");
    return g_strdup_printf("%s failed to connect: %s (%s)", prefix, name, reason_label);
  }
}

static gboolean
handle_wifi_auth_failure(NetworkSidebarActions *actions,
                         const char *name,
                         NMDeviceWifi *device,
                         NMAccessPoint *ap,
                         NMRemoteConnection *saved_wifi,
                         const char *temp_uuid)
{
  if (device == NULL)
    return FALSE;
  cleanup_temporary_wifi_profile(actions, temp_uuid, name);
  if (ap == NULL) {
    const char *argv[2] = { NULL, NULL };
    g_autofree char *edit_arg = NULL;
    g_autofree char *message = g_strdup_printf("Wi-Fi password needed for %s; opening editor", name);

    toast(actions, message);
    if (saved_wifi != NULL && nm_connection_get_uuid(NM_CONNECTION(saved_wifi)) != NULL) {
      edit_arg = g_strdup_printf("--edit=%s", nm_connection_get_uuid(NM_CONNECTION(saved_wifi)));
      argv[0] = edit_arg;
    }
    network_sidebar_actions_open_editor(actions, argv[0] != NULL ? argv : NULL);
    return TRUE;
  }
  if (ap_needs_password(ap)) {
    g_autofree char *message = g_strdup_printf("Wi-Fi password needed for %s", name);
    toast(actions, message);
    prompt_wifi_password(actions, device, ap, saved_wifi);
  } else {
    g_autofree char *message = g_strdup_printf("Opening advanced Wi-Fi editor for %s", name);
    const char *argv_create[] = { "--create", "--type=802-11-wireless", NULL };
    const char *argv_edit[2] = { NULL, NULL };
    g_autofree char *edit_arg = NULL;

    toast(actions, message);
    if (saved_wifi != NULL && nm_connection_get_uuid(NM_CONNECTION(saved_wifi)) != NULL) {
      edit_arg = g_strdup_printf("--edit=%s", nm_connection_get_uuid(NM_CONNECTION(saved_wifi)));
      argv_edit[0] = edit_arg;
      network_sidebar_actions_open_editor(actions, argv_edit);
    } else {
      network_sidebar_actions_open_editor(actions, argv_create);
    }
  }
  return TRUE;
}

static gboolean
activation_watch_poll(ActivationWatch *watch)
{
  NMActiveConnectionState state = nm_active_connection_get_state(watch->active);

  if (state == NM_ACTIVE_CONNECTION_STATE_ACTIVATED) {
    g_autofree char *message = g_strdup_printf(watch->is_vpn ? "VPN connected: %s" : "Wi-Fi connected: %s", watch->name);
    toast(watch->actions, message);
    schedule_refresh(watch->actions, 500);
    return FALSE;
  }

  if (state == NM_ACTIVE_CONNECTION_STATE_DEACTIVATED) {
    NMActiveConnectionStateReason reason = nm_active_connection_get_state_reason(watch->active);
    gboolean handled = FALSE;

    if (watch->is_wifi && watch->temp_uuid != NULL && !(watch->retry_wifi_auth && reason_looks_like_auth_failure(reason)))
      cleanup_temporary_wifi_profile(watch->actions, watch->temp_uuid, watch->name);

    if (watch->is_wifi && watch->retry_wifi_auth && reason_looks_like_auth_failure(reason))
      handled = handle_wifi_auth_failure(watch->actions,
                                         watch->name,
                                         watch->wifi_device,
                                         watch->wifi_ap,
                                         watch->saved_wifi,
                                         watch->temp_uuid);
    if (!handled) {
      if (!activation_reason_is_user_disconnected(reason)) {
        g_autofree char *message = activation_failed_message(watch->is_vpn ? "VPN" : "Wi-Fi", watch->name, reason);
        toast(watch->actions, message);
      }
    }
    schedule_refresh(watch->actions, 500);
    return FALSE;
  }

  if (watch->checks_remaining == 0) {
    g_autofree char *message = g_strdup_printf(watch->is_vpn ? "VPN is still connecting: %s" : "Wi-Fi is still connecting: %s", watch->name);
    toast(watch->actions, message);
    schedule_refresh(watch->actions, 500);
    return FALSE;
  }

  watch->checks_remaining--;
  return TRUE;
}

static gboolean
activation_watch_cb(gpointer user_data)
{
  ActivationWatch *watch = user_data;

  if (activation_watch_poll(watch))
    return G_SOURCE_CONTINUE;
  if (watch->actions->activation_watches != NULL && watch->key != NULL) {
    g_autofree char *key = g_strdup(watch->key);
    watch->source_id = 0;
    g_hash_table_remove(watch->actions->activation_watches, key);
  } else {
    activation_watch_free(watch);
  }
  return G_SOURCE_REMOVE;
}

static void
start_activation_watch(AsyncAction *async, NMActiveConnection *active)
{
  ActivationWatch *watch;

  if (active == NULL)
    return;
  watch = g_new0(ActivationWatch, 1);
  watch->actions = network_sidebar_actions_ref(async->actions);
  watch->active = g_object_ref(active);
  watch->name = g_strdup(async->name);
  watch->is_vpn = async->is_vpn;
  watch->is_wifi = async->is_wifi;
  watch->retry_wifi_auth = async->retry_wifi_auth;
  watch->temp_uuid = g_strdup(async->temp_uuid);
  watch->wifi_device = async->wifi_device != NULL ? g_object_ref(async->wifi_device) : NULL;
  watch->wifi_ap = async->wifi_ap != NULL ? g_object_ref(async->wifi_ap) : NULL;
  watch->saved_wifi = async->saved_wifi != NULL ? g_object_ref(async->saved_wifi) : NULL;
  watch->checks_remaining = 70;
  watch->key = activation_watch_key(active, async->name);
  g_hash_table_remove(async->actions->activation_watches, watch->key);
  if (!activation_watch_poll(watch)) {
    activation_watch_free(watch);
    return;
  }
  watch->source_id = g_timeout_add_seconds(1, activation_watch_cb, watch);
  g_hash_table_insert(async->actions->activation_watches, g_strdup(watch->key), watch);
}

static AdwToast *
toast_with_handle(NetworkSidebarActions *actions, const char *message)
{
  if (actions->toast != NULL)
    return actions->toast(message, actions->user_data);
  return NULL;
}

static void
toast(NetworkSidebarActions *actions, const char *message)
{
  AdwToast *handle = toast_with_handle(actions, message);

  g_clear_object(&handle);
}

static void
dismiss_wifi_scan_toast(NetworkSidebarActions *actions)
{
  if (actions->wifi_scan_toast == NULL)
    return;

  adw_toast_dismiss(actions->wifi_scan_toast);
  g_clear_object(&actions->wifi_scan_toast);
}

static void
schedule_refresh(NetworkSidebarActions *actions, guint delay_ms)
{
  if (actions->schedule_refresh != NULL)
    actions->schedule_refresh(delay_ms, actions->user_data);
}

void
network_sidebar_actions_schedule_refresh(NetworkSidebarActions *actions, guint delay_ms)
{
  schedule_refresh(actions, delay_ms);
}

gboolean
network_sidebar_actions_get_wifi_scan_in_progress(NetworkSidebarActions *actions)
{
  return actions != NULL && actions->wifi_scan_in_progress;
}

const char *
network_sidebar_actions_get_wifi_scan_error(NetworkSidebarActions *actions)
{
  return actions != NULL ? actions->wifi_scan_error : NULL;
}

void
network_sidebar_actions_set_networking_enabled(NetworkSidebarActions *actions, gboolean enabled)
{
  g_autoptr(GError) error = NULL;
  gboolean changed;

  changed = nm_client_networking_set_enabled(actions->client, enabled, &error);
  if (error != NULL) {
    g_autofree char *message = g_strdup_printf("Could not change networking state: %s", error->message);
    toast(actions, message);
    schedule_refresh(actions, 500);
    return;
  }
  if (!changed) {
    g_autofree char *message = g_strdup_printf("NetworkManager rejected the request to %s networking", enabled ? "enable" : "disable");
    toast(actions, message);
    schedule_refresh(actions, 500);
    return;
  }

  toast(actions, enabled ? "Networking enabled" : "Networking disabled");
  schedule_refresh(actions, 500);
}

typedef struct {
  NetworkSidebarActions *actions;
  gboolean enabled;
} WifiVerify;

static gboolean
verify_wifi_state_cb(gpointer user_data)
{
  WifiVerify *verify = user_data;
  gboolean hardware_enabled = nm_client_wireless_hardware_get_enabled(verify->actions->client);
  gboolean wireless_enabled = nm_client_wireless_get_enabled(verify->actions->client);

  if (verify->enabled && !hardware_enabled)
    toast(verify->actions, "Could not enable Wi-Fi: wireless hardware is disabled or blocked by rfkill");
  else if (wireless_enabled != verify->enabled) {
    g_autofree char *message = g_strdup_printf("NetworkManager rejected the request to %s Wi-Fi", verify->enabled ? "enable" : "disable");
    toast(verify->actions, message);
  }
  schedule_refresh(verify->actions, 500);
  network_sidebar_actions_unref(verify->actions);
  g_free(verify);
  return G_SOURCE_REMOVE;
}

void
network_sidebar_actions_set_wireless_enabled(NetworkSidebarActions *actions, gboolean enabled)
{
  WifiVerify *verify;

  nm_client_wireless_set_enabled(actions->client, enabled);
  toast(actions, enabled ? "Turning Wi-Fi on..." : "Turning Wi-Fi off...");
  schedule_refresh(actions, 500);

  verify = g_new0(WifiVerify, 1);
  verify->actions = network_sidebar_actions_ref(actions);
  verify->enabled = enabled;
  g_timeout_add(1200, verify_wifi_state_cb, verify);
}

static gboolean
finish_scan_settle_cb(gpointer user_data)
{
  NetworkSidebarActions *actions = user_data;
  actions->wifi_scan_settle_source = 0;
  actions->wifi_scan_in_progress = FALSE;
  g_clear_pointer(&actions->wifi_scan_error, g_free);
  dismiss_wifi_scan_toast(actions);
  schedule_refresh(actions, 1);
  network_sidebar_actions_unref(actions);
  return G_SOURCE_REMOVE;
}

static void
finish_scan_request_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  NetworkSidebarActions *actions = user_data;
  g_autoptr(GError) error = NULL;

  if (!nm_device_wifi_request_scan_finish(NM_DEVICE_WIFI(source), result, &error)) {
    const char *iface = nm_device_get_iface(NM_DEVICE(source));
    g_autofree char *message = g_strdup_printf("Could not scan %s: %s",
                                                iface != NULL ? iface : "Wi-Fi",
                                                error != NULL ? error->message : "unknown error");
    g_free(actions->wifi_scan_error);
    actions->wifi_scan_error = g_strdup(message);
    dismiss_wifi_scan_toast(actions);
    toast(actions, message);
  }

  if (actions->wifi_scan_pending_requests > 0)
    actions->wifi_scan_pending_requests--;
  if (actions->wifi_scan_pending_requests == 0 && actions->wifi_scan_settle_source == 0)
    actions->wifi_scan_settle_source = g_timeout_add(1800, finish_scan_settle_cb, network_sidebar_actions_ref(actions));
  network_sidebar_actions_unref(actions);
}

void
network_sidebar_actions_request_scan(NetworkSidebarActions *actions)
{
  const GPtrArray *devices;
  gboolean requested = FALSE;
  gint64 now = g_get_monotonic_time();

  if (actions->wifi_scan_in_progress)
    return;
  if (actions->last_scan_us != 0 && now - actions->last_scan_us < WIFI_SCAN_THROTTLE_SECONDS * G_USEC_PER_SEC)
    return;

  devices = nm_client_get_devices(actions->client);
  for (guint i = 0; devices != NULL && i < devices->len; i++) {
    NMDevice *device = g_ptr_array_index((GPtrArray *) devices, i);

    if (nm_device_get_device_type(device) != NM_DEVICE_TYPE_WIFI)
      continue;
    requested = TRUE;
  }

  if (!requested)
    return;

  actions->last_scan_us = now;
  actions->wifi_scan_in_progress = TRUE;
  actions->wifi_scan_pending_requests = 0;
  g_clear_pointer(&actions->wifi_scan_error, g_free);
  if (actions->wifi_scan_settle_source != 0) {
    g_source_remove(actions->wifi_scan_settle_source);
    actions->wifi_scan_settle_source = 0;
  }
  dismiss_wifi_scan_toast(actions);
  actions->wifi_scan_toast = toast_with_handle(actions, "Scanning for Wi-Fi networks...");
  schedule_refresh(actions, 1);

  for (guint i = 0; devices != NULL && i < devices->len; i++) {
    NMDevice *device = g_ptr_array_index((GPtrArray *) devices, i);

    if (nm_device_get_device_type(device) != NM_DEVICE_TYPE_WIFI)
      continue;
    actions->wifi_scan_pending_requests++;
    nm_device_wifi_request_scan_async(NM_DEVICE_WIFI(device), NULL, finish_scan_request_cb, network_sidebar_actions_ref(actions));
  }
}

static void
activate_finish_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  AsyncAction *async = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(NMActiveConnection) active = NULL;

  active = nm_client_activate_connection_finish(NM_CLIENT(source), result, &error);
  if (error != NULL) {
    if (async->is_wifi && async->retry_wifi_auth && error_looks_like_auth_failure(error) &&
        handle_wifi_auth_failure(async->actions,
                                 async->name,
                                 async->wifi_device,
                                 async->wifi_ap,
                                 async->saved_wifi,
                                 async->temp_uuid)) {
      schedule_refresh(async->actions, 1000);
      async_action_free(async);
      return;
    } else {
      g_autofree char *message = g_strdup_printf("Could not connect %s: %s", async->name, error->message);
      toast(async->actions, message);
      cleanup_temporary_wifi_profile(async->actions, async->temp_uuid, async->name);
    }
  } else {
    g_autofree char *message = g_strdup_printf("Started connecting %s", async->name);
    toast(async->actions, message);
    start_activation_watch(async, active);
  }
  schedule_refresh(async->actions, 1000);
  async_action_free(async);
}

void
network_sidebar_actions_activate_connection(NetworkSidebarActions *actions,
                                            NMConnection *connection,
                                            NMDevice *device,
                                            const char *specific_object)
{
  g_autofree char *name = network_sidebar_connection_name(connection, "connection");
  g_autofree char *message = g_strdup_printf("Connecting %s...", name);

  toast(actions, message);
  nm_client_activate_connection_async(actions->client,
                                      connection,
                                      device,
                                      specific_object,
                                      NULL,
                                      activate_finish_cb,
                                      async_action_new(actions, name));
}

static void
add_and_activate_wifi_finish_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  AsyncAction *async = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(NMActiveConnection) active = NULL;

  active = nm_client_add_and_activate_connection_finish(NM_CLIENT(source), result, &error);
  if (error != NULL) {
    if (async->retry_wifi_auth && error_looks_like_auth_failure(error) &&
        handle_wifi_auth_failure(async->actions,
                                 async->name,
                                 async->wifi_device,
                                 async->wifi_ap,
                                 NULL,
                                 async->temp_uuid)) {
      schedule_refresh(async->actions, 1000);
      async_action_free(async);
      return;
    } else {
      g_autofree char *message = g_strdup_printf("Could not connect to %s: %s", async->name, error->message);
      toast(async->actions, message);
      cleanup_temporary_wifi_profile(async->actions, async->temp_uuid, async->name);
    }
  } else {
    g_autofree char *message = g_strdup_printf("Started connecting %s", async->name);
    toast(async->actions, message);
    start_activation_watch(async, active);
  }
  schedule_refresh(async->actions, 1200);
  async_action_free(async);
}

static void
add_and_activate_wifi(NetworkSidebarActions *actions, NMDeviceWifi *device, NMAccessPoint *ap, const char *password)
{
  g_autofree char *ssid = network_sidebar_ap_ssid_text(ap);
  g_autofree char *uuid = NULL;
  g_autoptr(NMConnection) connection = NULL;
  AsyncAction *async;

  connection = create_wifi_connection(ap, password, &uuid);
  async = async_action_new(actions, ssid);
  async->is_wifi = TRUE;
  async->retry_wifi_auth = password != NULL;
  async->temp_uuid = g_strdup(uuid);
  async->wifi_device = g_object_ref(device);
  async->wifi_ap = g_object_ref(ap);

  {
    g_autofree char *message = g_strdup_printf("Connecting to %s...", ssid);
    toast(actions, message);
  }
  nm_client_add_and_activate_connection_async(actions->client,
                                             connection,
                                             NM_DEVICE(device),
                                             nm_object_get_path(NM_OBJECT(ap)),
                                             NULL,
                                             add_and_activate_wifi_finish_cb,
                                             async);
}

typedef struct {
  NetworkSidebarActions *actions;
  NMDeviceWifi *device;
  NMAccessPoint *ap;
  NMRemoteConnection *saved_wifi;
  GtkWidget *password_row;
} PasswordDialogData;

typedef struct {
  AdwAlertDialog *dialog;
  GtkWidget *password_row;
  DisabledResponseTooltip *connect_tooltip;
} PasswordValidationData;

static void
password_dialog_data_free(PasswordDialogData *data)
{
  network_sidebar_actions_unref(data->actions);
  g_clear_object(&data->device);
  g_clear_object(&data->ap);
  g_clear_object(&data->saved_wifi);
  g_free(data);
}

static void
password_validation_data_free(PasswordValidationData *data)
{
  if (data == NULL)
    return;
  disabled_response_tooltip_free(data->connect_tooltip);
  g_free(data);
}

static const char *
wifi_password_error(const char *password)
{
  if (password == NULL || *password == '\0')
    return WIFI_PASSWORD_REQUIRED_MESSAGE;
  if (g_utf8_strlen(password, -1) < WIFI_PASSWORD_MIN_LENGTH)
    return WIFI_PASSWORD_TOO_SHORT_MESSAGE;
  return NULL;
}

static gboolean
wifi_password_valid(const char *password)
{
  return wifi_password_error(password) == NULL;
}

static void
sync_password_connect_response(PasswordValidationData *validation)
{
  const char *password = gtk_editable_get_text(GTK_EDITABLE(validation->password_row));

  adw_alert_dialog_set_response_enabled(validation->dialog, "connect", wifi_password_valid(password));
  sync_disabled_response_tooltip(validation->connect_tooltip);
}

static void
on_password_text_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
  (void) object;
  (void) pspec;
  sync_password_connect_response(user_data);
}

static void
activate_saved_wifi_with_context(NetworkSidebarActions *actions,
                                  NMRemoteConnection *connection,
                                  NMDeviceWifi *device,
                                  NMAccessPoint *ap)
{
  g_autofree char *name = network_sidebar_connection_name(NM_CONNECTION(connection), "Wi-Fi");
  AsyncAction *async = async_action_new(actions, name);

  async->is_wifi = TRUE;
  async->retry_wifi_auth = TRUE;
  async->wifi_device = g_object_ref(device);
  async->wifi_ap = g_object_ref(ap);
  async->saved_wifi = g_object_ref(connection);
  {
    g_autofree char *message = g_strdup_printf("Connecting %s...", name);
    toast(actions, message);
  }
  nm_client_activate_connection_async(actions->client,
                                      NM_CONNECTION(connection),
                                      NM_DEVICE(device),
                                      nm_object_get_path(NM_OBJECT(ap)),
                                      NULL,
                                      activate_finish_cb,
                                      async);
}

static NMDeviceWifi *
compatible_wifi_device_for_connection(NetworkSidebarActions *actions, NMRemoteConnection *connection)
{
  const GPtrArray *devices = nm_client_get_devices(actions->client);
  NMConnection *nm_connection = NM_CONNECTION(connection);

  for (guint i = 0; devices != NULL && i < devices->len; i++) {
    NMDevice *device = g_ptr_array_index((GPtrArray *) devices, i);

    if (nm_device_get_device_type(device) != NM_DEVICE_TYPE_WIFI)
      continue;
    if (nm_device_connection_valid(device, nm_connection))
      return NM_DEVICE_WIFI(device);
  }
  return NULL;
}

void
network_sidebar_actions_activate_saved_wifi_profile(NetworkSidebarActions *actions, NMRemoteConnection *connection)
{
  g_autofree char *name = network_sidebar_connection_name(NM_CONNECTION(connection), "Wi-Fi");
  NMDeviceWifi *device = compatible_wifi_device_for_connection(actions, connection);
  AsyncAction *async;

  if (device == NULL) {
    g_autofree char *message = g_strdup_printf("Could not connect to %s: no compatible Wi-Fi device is available", name);
    toast(actions, message);
    schedule_refresh(actions, 1000);
    return;
  }

  async = async_action_new(actions, name);
  async->is_wifi = TRUE;
  async->retry_wifi_auth = TRUE;
  async->wifi_device = g_object_ref(device);
  async->saved_wifi = g_object_ref(connection);
  {
    g_autofree char *message = g_strdup_printf("Connecting %s...", name);
    toast(actions, message);
  }
  nm_client_activate_connection_async(actions->client,
                                      NM_CONNECTION(connection),
                                      NM_DEVICE(device),
                                      NULL,
                                      NULL,
                                      activate_finish_cb,
                                      async);
}

static void
commit_password_finish_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  PasswordDialogData *data = user_data;
  g_autoptr(GError) error = NULL;
  g_autofree char *name = network_sidebar_connection_name(NM_CONNECTION(data->saved_wifi), "Wi-Fi");

  if (!nm_remote_connection_commit_changes_finish(NM_REMOTE_CONNECTION(source), result, &error)) {
    g_autofree char *message = g_strdup_printf("Could not update Wi-Fi password for %s: %s", name, error != NULL ? error->message : "unknown error");
    toast(data->actions, message);
    schedule_refresh(data->actions, 1000);
    password_dialog_data_free(data);
    return;
  }

  activate_saved_wifi_with_context(data->actions, data->saved_wifi, data->device, data->ap);
  schedule_refresh(data->actions, 500);
  password_dialog_data_free(data);
}

static void
update_saved_wifi_password(PasswordDialogData *data, const char *password)
{
  g_autoptr(NMConnection) clone = NULL;
  NMSettingWirelessSecurity *security;
  g_autofree char *name = network_sidebar_connection_name(NM_CONNECTION(data->saved_wifi), "Wi-Fi");

  clone = nm_simple_connection_new_clone(NM_CONNECTION(data->saved_wifi));
  security = nm_connection_get_setting_wireless_security(clone);
  if (security == NULL) {
    NMSetting *new_security = nm_setting_wireless_security_new();
    nm_connection_add_setting(clone, new_security);
    security = NM_SETTING_WIRELESS_SECURITY(new_security);
  }

  g_object_set(security,
               NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, password_key_mgmt(data->ap, data->saved_wifi),
               NM_SETTING_WIRELESS_SECURITY_PSK, password,
               NULL);
  nm_connection_replace_settings_from_connection(NM_CONNECTION(data->saved_wifi), clone);
  {
    g_autofree char *message = g_strdup_printf("Updating Wi-Fi password for %s...", name);
    toast(data->actions, message);
  }
  nm_remote_connection_commit_changes_async(data->saved_wifi,
                                            TRUE,
                                            NULL,
                                            commit_password_finish_cb,
                                            data);
}

static void
password_dialog_chosen_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  PasswordDialogData *data = user_data;
  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
  const char *password = gtk_editable_get_text(GTK_EDITABLE(data->password_row));
  const char *validation_error;

  if (g_strcmp0(response, "connect") != 0) {
    password_dialog_data_free(data);
    return;
  }
  validation_error = wifi_password_error(password);
  if (validation_error != NULL) {
    toast(data->actions, validation_error);
    password_dialog_data_free(data);
    return;
  }

  if (data->saved_wifi != NULL)
    update_saved_wifi_password(data, password);
  else {
    add_and_activate_wifi(data->actions, data->device, data->ap, password);
    password_dialog_data_free(data);
  }
}

static void
prompt_wifi_password(NetworkSidebarActions *actions,
                     NMDeviceWifi *device,
                     NMAccessPoint *ap,
                     NMRemoteConnection *saved_wifi)
{
  g_autofree char *ssid = network_sidebar_ap_ssid_text(ap);
  g_autofree char *heading = g_strdup_printf("Connect to %s", ssid);
  AdwDialog *dialog;
  GtkWidget *list;
  GtkWidget *password_row;
  PasswordDialogData *data;
  PasswordValidationData *validation;

  if (actions->parent == NULL) {
    toast(actions, "Wi-Fi password is required");
    return;
  }

  dialog = adw_alert_dialog_new(heading, "Enter the Wi-Fi password. It must be at least 8 characters.");
  gtk_widget_add_css_class(GTK_WIDGET(dialog), "nm-sidebar-dialog");
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "connect", "Connect");
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "connect");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "connect", ADW_RESPONSE_SUGGESTED);

  list = gtk_list_box_new();
  gtk_widget_add_css_class(list, "boxed-list");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  password_row = adw_password_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(password_row), "Password");
  adw_entry_row_set_activates_default(ADW_ENTRY_ROW(password_row), TRUE);
  gtk_list_box_append(GTK_LIST_BOX(list), password_row);
  adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), list);

  validation = g_new0(PasswordValidationData, 1);
  validation->dialog = ADW_ALERT_DIALOG(dialog);
  validation->password_row = password_row;
  validation->connect_tooltip = attach_disabled_response_tooltip(ADW_ALERT_DIALOG(dialog),
                                                                 "connect",
                                                                 "Connect",
                                                                 WIFI_PASSWORD_TOO_SHORT_MESSAGE);
  g_object_set_data_full(G_OBJECT(dialog),
                         "nm-sidebar-password-validation",
                         validation,
                         (GDestroyNotify) password_validation_data_free);
  g_signal_connect(password_row, "notify::text", G_CALLBACK(on_password_text_changed), validation);
  sync_password_connect_response(validation);
  close_dialog_on_backdrop_click(dialog);

  data = g_new0(PasswordDialogData, 1);
  data->actions = network_sidebar_actions_ref(actions);
  data->device = g_object_ref(device);
  data->ap = g_object_ref(ap);
  data->saved_wifi = saved_wifi != NULL ? g_object_ref(saved_wifi) : NULL;
  data->password_row = password_row;
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(actions->parent), NULL, password_dialog_chosen_cb, data);
}

static gint
saved_wifi_connection_compare(gconstpointer left, gconstpointer right)
{
  NMRemoteConnection *left_connection = *(NMRemoteConnection * const *) left;
  NMRemoteConnection *right_connection = *(NMRemoteConnection * const *) right;
  const char *left_id = nm_connection_get_id(NM_CONNECTION(left_connection));
  const char *right_id = nm_connection_get_id(NM_CONNECTION(right_connection));
  g_autofree char *left_folded = g_utf8_casefold(left_id != NULL ? left_id : "", -1);
  g_autofree char *right_folded = g_utf8_casefold(right_id != NULL ? right_id : "", -1);

  return g_strcmp0(left_folded, right_folded);
}

static GPtrArray *
saved_wifi_connections_for_ap(NetworkSidebarActions *actions, NMDeviceWifi *device, NMAccessPoint *ap)
{
  const GPtrArray *connections = nm_client_get_connections(actions->client);
  GPtrArray *matches = g_ptr_array_new_with_free_func(g_object_unref);

  for (guint i = 0; connections != NULL && i < connections->len; i++) {
    NMRemoteConnection *connection = g_ptr_array_index((GPtrArray *) connections, i);
    NMConnection *nm_connection = NM_CONNECTION(connection);
    if (g_strcmp0(nm_connection_get_connection_type(nm_connection), NM_SETTING_WIRELESS_SETTING_NAME) != 0)
      continue;
    if (nm_device_connection_valid(NM_DEVICE(device), nm_connection) && nm_access_point_connection_valid(ap, nm_connection))
      g_ptr_array_add(matches, g_object_ref(connection));
  }
  g_ptr_array_sort(matches, saved_wifi_connection_compare);
  return matches;
}

typedef struct {
  NetworkSidebarActions *actions;
  NMDeviceWifi *device;
  NMAccessPoint *ap;
  GPtrArray *profiles;
} SavedProfilesDialogData;

typedef struct {
  NetworkSidebarActions *actions;
  NMDeviceWifi *device;
  NMAccessPoint *ap;
  NMRemoteConnection *profile;
  NMActiveConnection *active;
  AdwDialog *dialog;
} SavedProfileActionData;

static void
saved_profiles_dialog_data_free(SavedProfilesDialogData *data)
{
  network_sidebar_actions_unref(data->actions);
  g_clear_object(&data->device);
  g_clear_object(&data->ap);
  if (data->profiles != NULL)
    g_ptr_array_unref(data->profiles);
  g_free(data);
}

static SavedProfileActionData *
saved_profile_action_data_new(NetworkSidebarActions *actions,
                              NMDeviceWifi *device,
                              NMAccessPoint *ap,
                              NMRemoteConnection *profile,
                              NMActiveConnection *active,
                              AdwDialog *dialog)
{
  SavedProfileActionData *data = g_new0(SavedProfileActionData, 1);

  data->actions = network_sidebar_actions_ref(actions);
  data->device = g_object_ref(device);
  data->ap = g_object_ref(ap);
  data->profile = g_object_ref(profile);
  data->active = active != NULL ? g_object_ref(active) : NULL;
  data->dialog = dialog;
  return data;
}

static void
saved_profile_action_data_free(SavedProfileActionData *data)
{
  if (data == NULL)
    return;
  network_sidebar_actions_unref(data->actions);
  g_clear_object(&data->device);
  g_clear_object(&data->ap);
  g_clear_object(&data->profile);
  g_clear_object(&data->active);
  g_free(data);
}

static void
saved_profile_action_closure_notify(gpointer data, GClosure *closure)
{
  (void) closure;
  saved_profile_action_data_free(data);
}

static NMActiveConnection *
active_connection_by_uuid(NetworkSidebarActions *actions, const char *uuid)
{
  const GPtrArray *active_connections = nm_client_get_active_connections(actions->client);

  if (uuid == NULL)
    return NULL;
  for (guint i = 0; active_connections != NULL && i < active_connections->len; i++) {
    NMActiveConnection *active = g_ptr_array_index((GPtrArray *) active_connections, i);
    if (g_strcmp0(uuid, nm_active_connection_get_uuid(active)) == 0)
      return active;
  }
  return NULL;
}

static void
activate_saved_profile_from_dialog(SavedProfileActionData *data)
{
  adw_dialog_close(data->dialog);
  activate_saved_wifi_with_context(data->actions, data->profile, data->device, data->ap);
}

static void
on_saved_profile_connect_clicked(GtkButton *button, gpointer user_data)
{
  (void) button;
  activate_saved_profile_from_dialog(user_data);
}

static void
on_saved_profile_edit_clicked(GtkButton *button, gpointer user_data)
{
  SavedProfileActionData *data = user_data;
  (void) button;

  adw_dialog_close(data->dialog);
  network_sidebar_actions_edit_connection(data->actions, data->profile);
}

static void
on_saved_profile_remove_clicked(GtkButton *button, gpointer user_data)
{
  SavedProfileActionData *data = user_data;
  (void) button;

  adw_dialog_close(data->dialog);
  network_sidebar_actions_confirm_delete_connection(data->actions, data->profile, "Wi-Fi");
}

static void
on_saved_profile_row_activated(GtkListBoxRow *row, gpointer user_data)
{
  SavedProfileActionData *data = user_data;
  (void) row;

  if (data->active != NULL) {
    adw_dialog_close(data->dialog);
    network_sidebar_actions_deactivate(data->actions, data->active);
  } else {
    activate_saved_profile_from_dialog(data);
  }
}

static void
saved_profile_chosen_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  SavedProfilesDialogData *data = user_data;
  (void) adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
  saved_profiles_dialog_data_free(data);
}

static void
add_saved_profile_dialog_row(GtkListBox *profile_list,
                             NetworkSidebarActions *actions,
                             NMDeviceWifi *device,
                             NMAccessPoint *ap,
                             NMRemoteConnection *connection,
                             AdwDialog *dialog)
{
  const char *uuid = nm_connection_get_uuid(NM_CONNECTION(connection));
  NMActiveConnection *active = active_connection_by_uuid(actions, uuid);
  g_autofree char *profile_name = network_sidebar_connection_name(NM_CONNECTION(connection), "Wi-Fi");
  g_autofree char *subtitle = network_sidebar_wifi_profile_subtitle(connection);
  GtkWidget *row = adw_action_row_new();
  GtkWidget *connect_button;
  GtkWidget *edit_button;
  GtkWidget *remove_button;

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), profile_name);
  if (subtitle != NULL && *subtitle != '\0')
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  network_sidebar_apply_row_state(row, network_sidebar_connection_row_state(active));

  connect_button = network_sidebar_flat_button("network-wireless-symbolic", "Connect");
  gtk_widget_set_sensitive(connect_button, active == NULL);
  g_signal_connect_data(connect_button,
                        "clicked",
                        G_CALLBACK(on_saved_profile_connect_clicked),
                        saved_profile_action_data_new(actions, device, ap, connection, active, dialog),
                        saved_profile_action_closure_notify,
                        0);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), connect_button);

  edit_button = network_sidebar_flat_button("document-edit-symbolic", "Edit");
  g_signal_connect_data(edit_button,
                        "clicked",
                        G_CALLBACK(on_saved_profile_edit_clicked),
                        saved_profile_action_data_new(actions, device, ap, connection, active, dialog),
                        saved_profile_action_closure_notify,
                        0);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), edit_button);

  remove_button = network_sidebar_flat_button("user-trash-symbolic", "Remove");
  g_signal_connect_data(remove_button,
                        "clicked",
                        G_CALLBACK(on_saved_profile_remove_clicked),
                        saved_profile_action_data_new(actions, device, ap, connection, active, dialog),
                        saved_profile_action_closure_notify,
                        0);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove_button);

  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
  g_signal_connect_data(row,
                        "activated",
                        G_CALLBACK(on_saved_profile_row_activated),
                        saved_profile_action_data_new(actions, device, ap, connection, active, dialog),
                        saved_profile_action_closure_notify,
                        0);
  gtk_list_box_append(profile_list, row);
}

static void
show_saved_profiles_dialog(NetworkSidebarActions *actions, NMDeviceWifi *device, NMAccessPoint *ap, GPtrArray *profiles)
{
  g_autofree char *ssid = network_sidebar_ap_ssid_text(ap);
  g_autofree char *heading = g_strdup_printf("Saved profiles for %s", ssid);
  AdwDialog *dialog;
  GtkWidget *profile_list;
  SavedProfilesDialogData *data;

  if (actions->parent == NULL) {
    NMRemoteConnection *first = g_ptr_array_index(profiles, 0);
    activate_saved_wifi_with_context(actions, first, device, ap);
    g_ptr_array_unref(profiles);
    return;
  }

  dialog = adw_alert_dialog_new(heading, "Choose the Wi-Fi profile to connect, edit, or remove.");
  gtk_widget_add_css_class(GTK_WIDGET(dialog), "nm-sidebar-dialog");
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", "Close");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "close");
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "close");

  profile_list = gtk_list_box_new();
  gtk_widget_add_css_class(profile_list, "boxed-list");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(profile_list), GTK_SELECTION_NONE);
  for (guint i = 0; i < profiles->len; i++) {
    NMRemoteConnection *connection = g_ptr_array_index(profiles, i);
    add_saved_profile_dialog_row(GTK_LIST_BOX(profile_list), actions, device, ap, connection, dialog);
  }
  adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), profile_list);
  close_dialog_on_backdrop_click(dialog);

  data = g_new0(SavedProfilesDialogData, 1);
  data->actions = network_sidebar_actions_ref(actions);
  data->device = g_object_ref(device);
  data->ap = g_object_ref(ap);
  data->profiles = profiles;
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(actions->parent), NULL, saved_profile_chosen_cb, data);
}

void
network_sidebar_actions_connect_wifi(NetworkSidebarActions *actions, NMDeviceWifi *device, NMAccessPoint *ap)
{
  g_autoptr(GPtrArray) saved = saved_wifi_connections_for_ap(actions, device, ap);
  const char *editor_args[] = { "--create", "--type=802-11-wireless", NULL };

  if (saved->len == 1) {
    NMRemoteConnection *connection = g_ptr_array_index(saved, 0);
    activate_saved_wifi_with_context(actions, connection, device, ap);
    return;
  }
  if (saved->len > 1) {
    show_saved_profiles_dialog(actions, device, ap, g_steal_pointer(&saved));
    return;
  }

  if (!ap_has_ssid(ap)) {
    g_autofree char *context = access_point_context(ap);
    g_autofree char *message = context != NULL ?
      g_strdup_printf("Opening advanced Wi-Fi editor for hidden network (%s); enter the SSID", context) :
      g_strdup("Opening advanced Wi-Fi editor for hidden network; enter the SSID");
    toast(actions, message);
    network_sidebar_actions_open_editor(actions, editor_args);
    return;
  }

  if (!ap_is_secure(ap) || ap_supports_owe(ap)) {
    add_and_activate_wifi(actions, device, ap, NULL);
    return;
  }

  if (!ap_needs_password(ap)) {
    g_autofree char *ssid = network_sidebar_ap_ssid_text(ap);
    g_autofree char *message = g_strdup_printf("Opening advanced Wi-Fi editor for %s", ssid);
    toast(actions, message);
    network_sidebar_actions_open_editor(actions, editor_args);
    return;
  }

  prompt_wifi_password(actions, device, ap, NULL);
}

static const char *
vpn_base_active_connection_path(NetworkSidebarActions *actions)
{
  const GPtrArray *active_connections = nm_client_get_active_connections(actions->client);
  NMActiveConnection *fallback = NULL;
  NMActiveConnection *default6 = NULL;

  for (guint i = 0; active_connections != NULL && i < active_connections->len; i++) {
    NMActiveConnection *active = g_ptr_array_index((GPtrArray *) active_connections, i);
    const char *path = nm_object_get_path(NM_OBJECT(active));
    if (nm_active_connection_get_vpn(active) ||
        g_strcmp0(nm_active_connection_get_connection_type(active), NM_SETTING_VPN_SETTING_NAME) == 0 ||
        nm_active_connection_get_state(active) != NM_ACTIVE_CONNECTION_STATE_ACTIVATED ||
        path == NULL || *path == '\0')
      continue;
    if (fallback == NULL)
      fallback = active;
    if (nm_active_connection_get_default(active))
      return path;
    if (default6 == NULL && nm_active_connection_get_default6(active))
      default6 = active;
  }
  if (default6 != NULL)
    return nm_object_get_path(NM_OBJECT(default6));
  return fallback != NULL ? nm_object_get_path(NM_OBJECT(fallback)) : NULL;
}

void
network_sidebar_actions_activate_vpn_connection(NetworkSidebarActions *actions, NMRemoteConnection *connection)
{
  const char *base_path = vpn_base_active_connection_path(actions);
  g_autofree char *name = network_sidebar_connection_name(NM_CONNECTION(connection), "VPN");
  AsyncAction *async;

  if (base_path == NULL) {
    toast(actions, "VPN needs an active network connection");
    schedule_refresh(actions, 500);
    return;
  }

  async = async_action_new(actions, name);
  async->is_vpn = TRUE;
  {
    g_autofree char *message = g_strdup_printf("Connecting %s...", name);
    toast(actions, message);
  }
  nm_client_activate_connection_async(actions->client,
                                      NM_CONNECTION(connection),
                                      NULL,
                                      base_path,
                                      NULL,
                                      activate_finish_cb,
                                      async);
}

static void
deactivate_finish_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  AsyncAction *async = user_data;
  g_autoptr(GError) error = NULL;

  if (!nm_client_deactivate_connection_finish(NM_CLIENT(source), result, &error)) {
    g_autofree char *message = g_strdup_printf("Could not disconnect %s: %s", async->name, error != NULL ? error->message : "unknown error");
    toast(async->actions, message);
  } else {
    g_autofree char *message = g_strdup_printf("Disconnected %s", async->name);
    toast(async->actions, message);
  }
  schedule_refresh(async->actions, 700);
  async_action_free(async);
}

void
network_sidebar_actions_deactivate(NetworkSidebarActions *actions, NMActiveConnection *active)
{
  g_autofree char *name = network_sidebar_active_connection_name(active, "connection");
  g_autofree char *message = g_strdup_printf("Disconnecting %s...", name);
  g_autofree char *watch_key = activation_watch_key(active, name);

  g_hash_table_remove(actions->activation_watches, watch_key);
  toast(actions, message);
  nm_client_deactivate_connection_async(actions->client,
                                        active,
                                        NULL,
                                        deactivate_finish_cb,
                                        async_action_new(actions, name));
}

static NMActiveConnection *
active_connection_for_remote_connection(NetworkSidebarActions *actions, NMRemoteConnection *connection)
{
  const char *uuid = nm_connection_get_uuid(NM_CONNECTION(connection));
  const GPtrArray *active_connections = nm_client_get_active_connections(actions->client);

  if (uuid == NULL)
    return NULL;
  for (guint i = 0; active_connections != NULL && i < active_connections->len; i++) {
    NMActiveConnection *active = g_ptr_array_index((GPtrArray *) active_connections, i);
    if (g_strcmp0(uuid, nm_active_connection_get_uuid(active)) == 0)
      return active;
  }
  return NULL;
}

static void
delete_finish_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  AsyncAction *async = user_data;
  g_autoptr(GError) error = NULL;

  if (!nm_remote_connection_delete_finish(NM_REMOTE_CONNECTION(source), result, &error)) {
    g_autofree char *message = g_strdup_printf("Could not remove %s: %s", async->name, error != NULL ? error->message : "unknown error");
    toast(async->actions, message);
  } else {
    g_autofree char *message = g_strdup_printf("Removed %s", async->name);
    toast(async->actions, message);
  }
  schedule_refresh(async->actions, 700);
  async_action_free(async);
}

static void
delete_after_deactivate_finish_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  AsyncAction *async = user_data;
  g_autoptr(GError) error = NULL;

  if (!nm_client_deactivate_connection_finish(NM_CLIENT(source), result, &error)) {
    g_autofree char *message = g_strdup_printf("Could not disconnect %s before removing it: %s", async->name, error != NULL ? error->message : "unknown error");
    toast(async->actions, message);
    schedule_refresh(async->actions, 700);
    async_action_free(async);
    return;
  }

  {
    g_autofree char *message = g_strdup_printf("Removing %s...", async->name);
    toast(async->actions, message);
  }
  nm_remote_connection_delete_async(async->connection, NULL, delete_finish_cb, async);
}

void
network_sidebar_actions_delete_connection(NetworkSidebarActions *actions, NMRemoteConnection *connection)
{
  g_autofree char *name = network_sidebar_connection_name(NM_CONNECTION(connection), "connection");
  NMActiveConnection *active = active_connection_for_remote_connection(actions, connection);

  if (active != NULL) {
    AsyncAction *async = async_action_new(actions, name);
    g_autofree char *watch_key = activation_watch_key(active, name);
    g_autofree char *message = g_strdup_printf("Disconnecting and removing %s...", name);

    async->connection = g_object_ref(connection);
    g_hash_table_remove(actions->activation_watches, watch_key);
    toast(actions, message);
    nm_client_deactivate_connection_async(actions->client,
                                          active,
                                          NULL,
                                          delete_after_deactivate_finish_cb,
                                          async);
    return;
  }

  {
    g_autofree char *message = g_strdup_printf("Removing %s...", name);
    toast(actions, message);
  }
  nm_remote_connection_delete_async(connection, NULL, delete_finish_cb, async_action_new(actions, name));
}

typedef struct {
  NetworkSidebarActions *actions;
  NMRemoteConnection *connection;
} DeleteConfirmData;

static void
delete_confirm_data_free(DeleteConfirmData *data)
{
  network_sidebar_actions_unref(data->actions);
  g_object_unref(data->connection);
  g_free(data);
}

static void
delete_confirm_chosen_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  DeleteConfirmData *data = user_data;
  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);

  if (g_strcmp0(response, "confirm") == 0)
    network_sidebar_actions_delete_connection(data->actions, data->connection);
  delete_confirm_data_free(data);
}

void
network_sidebar_actions_confirm_delete_connection(NetworkSidebarActions *actions,
                                                  NMRemoteConnection *connection,
                                                  const char *profile_kind)
{
  const char *kind = profile_kind != NULL ? profile_kind : "connection";
  gboolean active = active_connection_for_remote_connection(actions, connection) != NULL;
  g_autofree char *name = network_sidebar_connection_name(NM_CONNECTION(connection), profile_kind != NULL ? profile_kind : "profile");
  g_autofree char *heading = g_strdup_printf("Remove %s?", name);
  g_autofree char *body = active ?
    g_strdup_printf("This disconnects the active %s connection and removes the saved profile from NetworkManager.", kind) :
    g_strdup_printf("This removes the saved %s profile from NetworkManager.", kind);
  AdwDialog *dialog;
  DeleteConfirmData *data;

  if (actions->parent == NULL) {
    toast(actions, "Could not show remove confirmation");
    return;
  }

  dialog = adw_alert_dialog_new(heading, body);
  gtk_widget_add_css_class(GTK_WIDGET(dialog), "nm-sidebar-dialog");
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "confirm", "Remove");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "confirm", ADW_RESPONSE_DESTRUCTIVE);
  close_dialog_on_backdrop_click(dialog);

  data = g_new0(DeleteConfirmData, 1);
  data->actions = network_sidebar_actions_ref(actions);
  data->connection = g_object_ref(connection);
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(actions->parent), NULL, delete_confirm_chosen_cb, data);
}

static char **
child_process_env(void)
{
  char **env = g_get_environ();
  const char *preload = g_environ_getenv(env, "LD_PRELOAD");

  env = g_environ_unsetenv(env, "NM_SIDEBAR_LAYER_SHELL_PRELOAD");
  if (preload != NULL && strstr(preload, "libgtk4-layer-shell.so") != NULL)
    env = g_environ_unsetenv(env, "LD_PRELOAD");
  return env;
}

static char *
find_program_in_path_env(const char *program, char **env)
{
  const char *path = g_environ_getenv(env, "PATH");
  g_auto(GStrv) directories = NULL;

  if (path == NULL)
    return g_find_program_in_path(program);

  directories = g_strsplit(path, G_SEARCHPATH_SEPARATOR_S, -1);
  for (guint i = 0; directories[i] != NULL; i++) {
    const char *directory = *directories[i] != '\0' ? directories[i] : ".";
    g_autofree char *candidate = g_build_filename(directory, program, NULL);

    if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE))
      return g_steal_pointer(&candidate);
  }

  return NULL;
}

static void
editor_child_setup(gpointer user_data)
{
  (void) user_data;
  setsid();
}

gboolean
network_sidebar_actions_connection_editor_available(void)
{
  g_auto(GStrv) env = child_process_env();
  g_autofree char *path = find_program_in_path_env("nm-connection-editor", env);
  return path != NULL;
}

void
network_sidebar_actions_open_editor(NetworkSidebarActions *actions, const char *const *args)
{
  g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
  g_auto(GStrv) env = child_process_env();
  g_autofree char *editor_path = find_program_in_path_env("nm-connection-editor", env);
  g_autoptr(GError) error = NULL;

  g_ptr_array_add(argv, g_strdup(editor_path != NULL ? editor_path : "nm-connection-editor"));
  for (guint i = 0; args != NULL && args[i] != NULL; i++)
    g_ptr_array_add(argv, g_strdup(args[i]));
  g_ptr_array_add(argv, NULL);

  if (!g_spawn_async(NULL,
                     (char **) argv->pdata,
                     env,
                     G_SPAWN_SEARCH_PATH_FROM_ENVP | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                     editor_child_setup,
                     NULL,
                     NULL,
                     &error)) {
    g_autofree char *message = g_strdup_printf("Could not open nm-connection-editor: %s", error != NULL ? error->message : "unknown error");
    toast(actions, message);
  }
}

void
network_sidebar_actions_edit_connection(NetworkSidebarActions *actions, NMRemoteConnection *connection)
{
  const char *uuid = nm_connection_get_uuid(NM_CONNECTION(connection));
  g_autofree char *edit_arg = NULL;
  const char *argv[2] = { NULL, NULL };

  if (uuid != NULL && *uuid != '\0') {
    edit_arg = g_strdup_printf("--edit=%s", uuid);
    argv[0] = edit_arg;
  }
  network_sidebar_actions_open_editor(actions, argv[0] != NULL ? argv : NULL);
}
