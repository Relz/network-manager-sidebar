#include "sections/wifi.h"

#include "data/connection_state.h"
#include "data/labels.h"
#include "sections/helpers.h"

#include <string.h>

typedef struct {
  NetworkSidebarActions *actions;
  NMDeviceWifi *device;
  NMAccessPoint *ap;
  NMActiveConnection *active;
  NMRemoteConnection *saved;
} WifiRowAction;

typedef struct {
  NMDeviceWifi *device;
  NMAccessPoint *ap;
  NMActiveConnection *active;
  NetworkSidebarRowState row_state;
  GPtrArray *saved_connections;
  gboolean show_ap_details;
  gboolean show_device_iface;
} WifiEntry;

static void
wifi_row_action_free(WifiRowAction *data)
{
  network_sidebar_actions_unref(data->actions);
  g_clear_object(&data->device);
  g_clear_object(&data->ap);
  g_clear_object(&data->active);
  g_clear_object(&data->saved);
  g_free(data);
}

static void
wifi_row_action_closure_notify(gpointer data, GClosure *closure)
{
  (void) closure;
  wifi_row_action_free(data);
}

static void
wifi_entry_free(WifiEntry *entry)
{
  if (entry == NULL)
    return;
  g_clear_object(&entry->device);
  g_clear_object(&entry->ap);
  g_clear_object(&entry->active);
  g_clear_pointer(&entry->saved_connections, g_ptr_array_unref);
  g_free(entry);
}

static gboolean
ssid_bytes_equal(GBytes *left, GBytes *right)
{
  const guint8 *left_bytes;
  const guint8 *right_bytes;
  gsize left_len = 0;
  gsize right_len = 0;

  if (left != NULL)
    left_bytes = g_bytes_get_data(left, &left_len);
  else
    left_bytes = NULL;
  if (right != NULL)
    right_bytes = g_bytes_get_data(right, &right_len);
  else
    right_bytes = NULL;
  if (left_len != right_len)
    return FALSE;
  if (left_len == 0)
    return TRUE;
  return memcmp(left_bytes, right_bytes, left_len) == 0;
}

static gboolean
ap_has_ssid(NMAccessPoint *ap)
{
  GBytes *ssid = nm_access_point_get_ssid(ap);
  gsize length = 0;

  if (ssid == NULL)
    return FALSE;
  g_bytes_get_data(ssid, &length);
  return length > 0;
}

static char *
ap_bssid(NMAccessPoint *ap)
{
  const char *bssid = nm_access_point_get_bssid(ap);

  return g_ascii_strdown(bssid != NULL ? bssid : "", -1);
}

static char *
ap_bssid_or_path(NMAccessPoint *ap)
{
  const char *bssid = nm_access_point_get_bssid(ap);
  const char *path;

  if (bssid != NULL && *bssid != '\0')
    return g_ascii_strdown(bssid, -1);
  path = nm_object_get_path(NM_OBJECT(ap));
  return g_strdup(path != NULL ? path : "");
}

static char *
device_identity(NMDeviceWifi *device)
{
  const char *path = nm_object_get_path(NM_OBJECT(device));
  const char *iface;

  if (path != NULL && *path != '\0')
    return g_strdup(path);
  iface = nm_device_get_iface(NM_DEVICE(device));
  return g_strdup(iface != NULL ? iface : "unknown");
}

static gboolean
same_ap_identity(WifiEntry *entry, NMDeviceWifi *device, NMAccessPoint *ap)
{
  g_autofree char *entry_bssid = ap_bssid_or_path(entry->ap);
  g_autofree char *candidate_bssid = ap_bssid_or_path(ap);

  return g_strcmp0(nm_device_get_iface(NM_DEVICE(entry->device)), nm_device_get_iface(NM_DEVICE(device))) == 0 &&
         ssid_bytes_equal(nm_access_point_get_ssid(entry->ap), nm_access_point_get_ssid(ap)) &&
         g_strcmp0(entry_bssid, candidate_bssid) == 0 &&
         nm_access_point_get_frequency(entry->ap) == nm_access_point_get_frequency(ap) &&
         nm_access_point_get_flags(entry->ap) == nm_access_point_get_flags(ap) &&
         nm_access_point_get_wpa_flags(entry->ap) == nm_access_point_get_wpa_flags(ap) &&
         nm_access_point_get_rsn_flags(entry->ap) == nm_access_point_get_rsn_flags(ap);
}

static gboolean
same_ap_display_identity(NMAccessPoint *left, NMAccessPoint *right)
{
  g_autofree char *left_bssid = ap_bssid(left);
  g_autofree char *right_bssid = ap_bssid(right);

  return ssid_bytes_equal(nm_access_point_get_ssid(left), nm_access_point_get_ssid(right)) &&
         g_strcmp0(left_bssid, right_bssid) == 0 &&
         nm_access_point_get_frequency(left) == nm_access_point_get_frequency(right) &&
         nm_access_point_get_flags(left) == nm_access_point_get_flags(right) &&
         nm_access_point_get_wpa_flags(left) == nm_access_point_get_wpa_flags(right) &&
         nm_access_point_get_rsn_flags(left) == nm_access_point_get_rsn_flags(right);
}

static gboolean
row_state_preferred(NetworkSidebarRowState state)
{
  return state == NETWORK_SIDEBAR_ROW_STATE_ACTIVE || state == NETWORK_SIDEBAR_ROW_STATE_CONNECTING;
}

static int
row_state_rank(NetworkSidebarRowState state)
{
  if (state == NETWORK_SIDEBAR_ROW_STATE_ACTIVE)
    return 0;
  if (state == NETWORK_SIDEBAR_ROW_STATE_CONNECTING)
    return 1;
  return 2;
}

static gboolean
ap_matches_active(NMDeviceWifi *device, NMAccessPoint *ap, NMActiveConnection *active)
{
  NMAccessPoint *active_ap = nm_device_wifi_get_active_access_point(device);
  const char *ap_path = nm_object_get_path(NM_OBJECT(ap));
  const char *specific_path = active != NULL ? nm_active_connection_get_specific_object_path(active) : NULL;

  if (active_ap != NULL && g_strcmp0(nm_object_get_path(NM_OBJECT(active_ap)), ap_path) == 0)
    return TRUE;
  return specific_path != NULL && g_strcmp0(specific_path, "/") != 0 && g_strcmp0(specific_path, ap_path) == 0;
}

static void
wifi_entry_set(WifiEntry *entry,
               NMDeviceWifi *device,
               NMAccessPoint *ap,
               NMActiveConnection *active,
               NetworkSidebarRowState row_state)
{
  g_set_object(&entry->device, device);
  g_set_object(&entry->ap, ap);
  g_set_object(&entry->active, active);
  entry->row_state = row_state;
}

static GPtrArray *
saved_wifi_connections_for_ap(NMClient *client, NMDeviceWifi *device, NMAccessPoint *ap)
{
  const GPtrArray *connections = nm_client_get_connections(client);
  GPtrArray *matches = g_ptr_array_new_with_free_func(g_object_unref);

  for (guint i = 0; connections != NULL && i < connections->len; i++) {
    NMRemoteConnection *connection = g_ptr_array_index((GPtrArray *) connections, i);
    NMConnection *nm_connection = NM_CONNECTION(connection);

    if (g_strcmp0(nm_connection_get_connection_type(nm_connection), NM_SETTING_WIRELESS_SETTING_NAME) != 0)
      continue;
    if (nm_device_connection_valid(NM_DEVICE(device), nm_connection) && nm_access_point_connection_valid(ap, nm_connection))
      g_ptr_array_add(matches, g_object_ref(connection));
  }
  return matches;
}

static gint
saved_connection_compare(gconstpointer left, gconstpointer right)
{
  NMRemoteConnection *left_connection = *(NMRemoteConnection * const *) left;
  NMRemoteConnection *right_connection = *(NMRemoteConnection * const *) right;
  const char *left_id = nm_connection_get_id(NM_CONNECTION(left_connection));
  const char *right_id = nm_connection_get_id(NM_CONNECTION(right_connection));
  g_autofree char *left_folded = g_utf8_casefold(left_id != NULL ? left_id : "", -1);
  g_autofree char *right_folded = g_utf8_casefold(right_id != NULL ? right_id : "", -1);
  int id_compare = g_strcmp0(left_folded, right_folded);

  if (id_compare != 0)
    return id_compare;
  return g_strcmp0(nm_connection_get_uuid(NM_CONNECTION(left_connection)), nm_connection_get_uuid(NM_CONNECTION(right_connection)));
}

static GPtrArray *
saved_wifi_profiles(NMClient *client)
{
  const GPtrArray *connections = nm_client_get_connections(client);
  GPtrArray *profiles = g_ptr_array_new_with_free_func(g_object_unref);

  for (guint i = 0; connections != NULL && i < connections->len; i++) {
    NMRemoteConnection *connection = g_ptr_array_index((GPtrArray *) connections, i);

    if (g_strcmp0(nm_connection_get_connection_type(NM_CONNECTION(connection)), NM_SETTING_WIRELESS_SETTING_NAME) == 0)
      g_ptr_array_add(profiles, g_object_ref(connection));
  }
  g_ptr_array_sort(profiles, saved_connection_compare);
  return profiles;
}

static char *
saved_profile_key(NMRemoteConnection *connection)
{
  const char *uuid = nm_connection_get_uuid(NM_CONNECTION(connection));
  const char *path;

  if (uuid != NULL && *uuid != '\0')
    return g_strdup_printf("uuid:%s", uuid);
  path = nm_object_get_path(NM_OBJECT(connection));
  if (path != NULL && *path != '\0')
    return g_strdup_printf("path:%s", path);
  return g_strdup_printf("object:%p", connection);
}

static NMActiveConnection *
active_connection_by_uuid(NMClient *client, const char *uuid)
{
  const GPtrArray *active_connections = nm_client_get_active_connections(client);

  if (uuid == NULL)
    return NULL;
  for (guint i = 0; active_connections != NULL && i < active_connections->len; i++) {
    NMActiveConnection *active = g_ptr_array_index((GPtrArray *) active_connections, i);
    if (g_strcmp0(uuid, nm_active_connection_get_uuid(active)) == 0)
      return active;
  }
  return NULL;
}

static NMDeviceWifi *
compatible_wifi_device_for_profile(NMClient *client, NMRemoteConnection *connection)
{
  const GPtrArray *devices = nm_client_get_devices(client);
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

static gboolean
saved_profile_is_connected(NMClient *client, NMRemoteConnection *profile)
{
  const char *uuid = nm_connection_get_uuid(NM_CONNECTION(profile));
  NMActiveConnection *active = active_connection_by_uuid(client, uuid);

  return row_state_preferred(network_sidebar_connection_row_state(active));
}

static GPtrArray *
build_wifi_entries(NMClient *client)
{
  const GPtrArray *devices = nm_client_get_devices(client);
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify) wifi_entry_free);

  for (guint i = 0; devices != NULL && i < devices->len; i++) {
    NMDevice *device = g_ptr_array_index((GPtrArray *) devices, i);
    NMDeviceWifi *wifi;
    NMActiveConnection *active_connection;
    const GPtrArray *access_points;

    if (nm_device_get_device_type(device) != NM_DEVICE_TYPE_WIFI)
      continue;
    wifi = NM_DEVICE_WIFI(device);
    active_connection = nm_device_get_active_connection(device);
    access_points = nm_device_wifi_get_access_points(wifi);

    for (guint j = 0; access_points != NULL && j < access_points->len; j++) {
      NMAccessPoint *ap = g_ptr_array_index((GPtrArray *) access_points, j);
      NMActiveConnection *current_active = ap_matches_active(wifi, ap, active_connection) ? active_connection : NULL;
      NetworkSidebarRowState row_state = network_sidebar_connection_row_state(current_active);
      WifiEntry *current = NULL;

      for (guint k = 0; k < entries->len; k++) {
        WifiEntry *entry = g_ptr_array_index(entries, k);
        if (same_ap_identity(entry, wifi, ap)) {
          current = entry;
          break;
        }
      }

      if (current != NULL) {
        if (row_state_preferred(current->row_state))
          continue;
        if (!row_state_preferred(row_state) && nm_access_point_get_strength(ap) <= nm_access_point_get_strength(current->ap))
          continue;
        wifi_entry_set(current, wifi, ap, current_active, row_state);
        continue;
      }

      current = g_new0(WifiEntry, 1);
      wifi_entry_set(current, wifi, ap, current_active, row_state);
      g_ptr_array_add(entries, current);
    }
  }
  return entries;
}

static gint
wifi_entry_compare(gconstpointer left, gconstpointer right)
{
  WifiEntry *left_entry = *(WifiEntry * const *) left;
  WifiEntry *right_entry = *(WifiEntry * const *) right;
  int left_rank = row_state_rank(left_entry->row_state);
  int right_rank = row_state_rank(right_entry->row_state);

  if (left_rank != right_rank)
    return left_rank - right_rank;
  return (int) nm_access_point_get_strength(right_entry->ap) - (int) nm_access_point_get_strength(left_entry->ap);
}

static void
mark_duplicate_access_points(GPtrArray *entries)
{
  for (guint i = 0; i < entries->len; i++) {
    WifiEntry *entry = g_ptr_array_index(entries, i);
    guint ssid_count = 0;
    gboolean multiple_devices = FALSE;
    g_autofree char *entry_device = device_identity(entry->device);

    for (guint j = 0; j < entries->len; j++) {
      WifiEntry *other = g_ptr_array_index(entries, j);
      if (ssid_bytes_equal(nm_access_point_get_ssid(entry->ap), nm_access_point_get_ssid(other->ap)))
        ssid_count++;
      if (same_ap_display_identity(entry->ap, other->ap)) {
        g_autofree char *other_device = device_identity(other->device);
        if (g_strcmp0(entry_device, other_device) != 0)
          multiple_devices = TRUE;
      }
    }

    entry->show_ap_details = !ap_has_ssid(entry->ap) || ssid_count > 1;
    entry->show_device_iface = multiple_devices;
  }
}

static char *
channel_label(guint32 mhz)
{
  if (mhz == 2484)
    return g_strdup("Channel 14");
  if (mhz >= 2412 && mhz <= 2472 && (mhz - 2407) % 5 == 0)
    return g_strdup_printf("Channel %u", (mhz - 2407) / 5);
  if (mhz >= 5000 && mhz <= 5900 && (mhz - 5000) % 5 == 0)
    return g_strdup_printf("Channel %u", (mhz - 5000) / 5);
  if (mhz >= 5955 && mhz <= 7115 && (mhz - 5950) % 5 == 0)
    return g_strdup_printf("Channel %u", (mhz - 5950) / 5);
  return NULL;
}

static void
append_subtitle_part(GString *subtitle, const char *part)
{
  if (part == NULL || *part == '\0')
    return;
  if (subtitle->len > 0)
    g_string_append(subtitle, " - ");
  g_string_append(subtitle, part);
}

static char *
wifi_entry_subtitle(WifiEntry *entry)
{
  g_autoptr(GString) subtitle = g_string_new(NULL);
  g_autofree char *signal = g_strdup_printf("Signal %u%%", nm_access_point_get_strength(entry->ap));
  g_autofree char *security = network_sidebar_ap_security_label(entry->ap);
  g_autofree char *frequency = network_sidebar_frequency_label(nm_access_point_get_frequency(entry->ap));

  append_subtitle_part(subtitle, signal);
  append_subtitle_part(subtitle, security);
  append_subtitle_part(subtitle, frequency);
  if (entry->show_ap_details) {
    const char *bssid = nm_access_point_get_bssid(entry->ap);
    g_autofree char *bssid_part = bssid != NULL && *bssid != '\0' ? g_strdup_printf("BSSID %s", bssid) : NULL;
    g_autofree char *channel = channel_label(nm_access_point_get_frequency(entry->ap));
    append_subtitle_part(subtitle, bssid_part);
    append_subtitle_part(subtitle, channel);
  }
  if (entry->show_device_iface) {
    g_autofree char *iface_part = g_strdup_printf("Interface %s", nm_device_get_iface(NM_DEVICE(entry->device)) != NULL ? nm_device_get_iface(NM_DEVICE(entry->device)) : "unknown");
    append_subtitle_part(subtitle, iface_part);
  }
  if (entry->saved_connections != NULL && entry->saved_connections->len > 1) {
    g_autofree char *profiles = g_strdup_printf("%u saved profiles", entry->saved_connections->len);
    append_subtitle_part(subtitle, profiles);
  }
  return g_string_free(g_steal_pointer(&subtitle), FALSE);
}

static char *
wifi_entry_title(WifiEntry *entry)
{
  g_autofree char *ssid = network_sidebar_ap_ssid_text(entry->ap);

  if (entry->active != NULL)
    return network_sidebar_active_connection_name(entry->active, ssid);
  if (entry->saved_connections != NULL && entry->saved_connections->len == 1) {
    NMRemoteConnection *saved = g_ptr_array_index(entry->saved_connections, 0);
    return network_sidebar_connection_name(NM_CONNECTION(saved), ssid);
  }
  return g_steal_pointer(&ssid);
}

static GtkWidget *
subgroup_list_with_suffix(const char *title, GtkWidget *suffix, GtkWidget **out_list)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *list = gtk_list_box_new();

  gtk_widget_add_css_class(box, "network-subgroup");
  if (title != NULL || suffix != NULL) {
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    if (title != NULL) {
      GtkWidget *label = gtk_label_new(title);

      gtk_widget_add_css_class(label, "network-subsection-label");
      gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
      gtk_widget_set_hexpand(label, TRUE);
      gtk_box_append(GTK_BOX(header), label);
    }
    if (suffix != NULL)
      gtk_box_append(GTK_BOX(header), suffix);
    gtk_box_append(GTK_BOX(box), header);
  }
  gtk_widget_add_css_class(list, "boxed-list");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_box_append(GTK_BOX(box), list);
  *out_list = list;
  return box;
}

static GtkWidget *
subgroup_list(const char *title, GtkWidget **out_list)
{
  return subgroup_list_with_suffix(title, NULL, out_list);
}

static void
on_wifi_row_activated(GtkListBoxRow *row, gpointer user_data)
{
  WifiRowAction *data = user_data;
  (void) row;

  if (data->active != NULL)
    network_sidebar_actions_deactivate(data->actions, data->active);
  else if (data->device != NULL && data->ap != NULL)
    network_sidebar_actions_connect_wifi(data->actions, data->device, data->ap);
  else if (data->saved != NULL)
    network_sidebar_actions_activate_saved_wifi_profile(data->actions, data->saved);
}

static void
on_wifi_edit_clicked(GtkButton *button, gpointer user_data)
{
  WifiRowAction *data = user_data;
  (void) button;
  if (data->saved != NULL)
    network_sidebar_actions_edit_connection(data->actions, data->saved);
}

static void
on_wifi_remove_clicked(GtkButton *button, gpointer user_data)
{
  WifiRowAction *data = user_data;
  (void) button;
  if (data->saved != NULL)
    network_sidebar_actions_confirm_delete_connection(data->actions, data->saved, "Wi-Fi");
}

static void
on_wifi_profiles_clicked(GtkButton *button, gpointer user_data)
{
  WifiRowAction *data = user_data;
  (void) button;
  network_sidebar_actions_connect_wifi(data->actions, data->device, data->ap);
}

static void
on_wifi_switch(GtkSwitch *switch_widget, GParamSpec *pspec, gpointer user_data)
{
  NetworkSidebarActions *actions = user_data;
  (void) pspec;
  network_sidebar_actions_set_wireless_enabled(actions, gtk_switch_get_active(switch_widget));
}

static void
on_add_wifi_clicked(GtkButton *button, gpointer user_data)
{
  (void) button;
  network_sidebar_actions_open_editor(user_data, (const char *const[]) { "--create", "--type=802-11-wireless", NULL });
}

static char *
saved_profile_row_subtitle(NMRemoteConnection *profile, NMActiveConnection *active, NetworkSidebarRowState row_state)
{
  g_autoptr(GString) subtitle = g_string_new(NULL);
  g_autofree char *details = network_sidebar_wifi_profile_subtitle(profile);

  if (row_state == NETWORK_SIDEBAR_ROW_STATE_ACTIVE)
    append_subtitle_part(subtitle, "Active");
  else if (row_state == NETWORK_SIDEBAR_ROW_STATE_CONNECTING)
    append_subtitle_part(subtitle, "Connecting");
  else if (active != NULL)
    append_subtitle_part(subtitle, "Active connection");
  else
    append_subtitle_part(subtitle, "Out of range");

  append_subtitle_part(subtitle, details);
  return g_string_free(g_steal_pointer(&subtitle), FALSE);
}

static char *
saved_profile_activation_note(NMClient *client, NMRemoteConnection *profile)
{
  if (!nm_client_networking_get_enabled(client))
    return g_strdup("Enable networking to connect");
  if (!nm_client_wireless_hardware_get_enabled(client))
    return g_strdup("Wi-Fi hardware is disabled");
  if (!nm_client_wireless_get_enabled(client))
    return g_strdup("Enable Wi-Fi to connect");
  if (compatible_wifi_device_for_profile(client, profile) == NULL)
    return g_strdup("No compatible Wi-Fi device");
  return NULL;
}

static void
add_saved_profile_row(GtkListBox *list, NMClient *client, NetworkSidebarActions *actions, NMRemoteConnection *profile)
{
  const char *uuid = nm_connection_get_uuid(NM_CONNECTION(profile));
  NMActiveConnection *active = active_connection_by_uuid(client, uuid);
  NetworkSidebarRowState row_state = network_sidebar_connection_row_state(active);
  g_autofree char *title = network_sidebar_connection_name(NM_CONNECTION(profile), "Wi-Fi");
  g_autofree char *subtitle = saved_profile_row_subtitle(profile, active, row_state);
  g_autofree char *activation_note = active == NULL ? saved_profile_activation_note(client, profile) : NULL;
  GtkWidget *row = network_sidebar_action_row(title, subtitle, "network-wireless-symbolic");
  WifiRowAction *row_action = g_new0(WifiRowAction, 1);
  GtkWidget *edit_button;
  GtkWidget *remove_button;
  gboolean can_activate = active != NULL || activation_note == NULL;

  row_action->actions = network_sidebar_actions_ref(actions);
  row_action->active = active != NULL ? g_object_ref(active) : NULL;
  row_action->saved = g_object_ref(profile);
  network_sidebar_apply_row_state(row, row_state);

  edit_button = network_sidebar_flat_button("document-edit-symbolic", "Edit");
  g_signal_connect(edit_button, "clicked", G_CALLBACK(on_wifi_edit_clicked), row_action);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), edit_button);

  remove_button = network_sidebar_flat_button("user-trash-symbolic", "Remove");
  g_signal_connect(remove_button, "clicked", G_CALLBACK(on_wifi_remove_clicked), row_action);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove_button);

  if (activation_note != NULL)
    gtk_widget_set_tooltip_text(row, activation_note);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), can_activate);
  g_signal_connect_data(row, "activated", G_CALLBACK(on_wifi_row_activated), row_action, wifi_row_action_closure_notify, 0);
  gtk_list_box_append(list, row);
}

static guint
unmatched_saved_profile_count(NMClient *client, GPtrArray *profiles, GHashTable *visible_saved_keys)
{
  guint count = 0;

  for (guint i = 0; profiles != NULL && i < profiles->len; i++) {
    NMRemoteConnection *profile = g_ptr_array_index(profiles, i);
    g_autofree char *key = saved_profile_key(profile);

    if (visible_saved_keys != NULL && g_hash_table_contains(visible_saved_keys, key))
      continue;
    if (saved_profile_is_connected(client, profile))
      continue;
    count++;
  }
  return count;
}

static guint
add_unmatched_connected_saved_profile_rows(GtkListBox *list,
                                          NMClient *client,
                                          NetworkSidebarActions *actions,
                                          GPtrArray *profiles,
                                          GHashTable *visible_saved_keys)
{
  guint added = 0;

  for (guint i = 0; profiles != NULL && i < profiles->len; i++) {
    NMRemoteConnection *profile = g_ptr_array_index(profiles, i);
    g_autofree char *key = saved_profile_key(profile);

    if (visible_saved_keys != NULL && g_hash_table_contains(visible_saved_keys, key))
      continue;
    if (!saved_profile_is_connected(client, profile))
      continue;
    add_saved_profile_row(list, client, actions, profile);
    added++;
  }
  return added;
}

static guint
add_unmatched_saved_profile_rows(GtkListBox *list,
                                 NMClient *client,
                                 NetworkSidebarActions *actions,
                                 GPtrArray *profiles,
                                 GHashTable *visible_saved_keys)
{
  guint added = 0;

  for (guint i = 0; profiles != NULL && i < profiles->len; i++) {
    NMRemoteConnection *profile = g_ptr_array_index(profiles, i);
    g_autofree char *key = saved_profile_key(profile);

    if (visible_saved_keys != NULL && g_hash_table_contains(visible_saved_keys, key))
      continue;
    if (saved_profile_is_connected(client, profile))
      continue;
    add_saved_profile_row(list, client, actions, profile);
    added++;
  }
  return added;
}

static void
add_empty_saved_row(GtkListBox *list, const char *title)
{
  gtk_list_box_append(list, network_sidebar_action_row(title, NULL, NULL));
}

static void
add_saved_profiles_only_group(AdwPreferencesGroup *group,
                              NMClient *client,
                              NetworkSidebarActions *actions,
                              GPtrArray *profiles)
{
  guint out_of_range_count;
  GtkWidget *saved_list = NULL;
  GtkWidget *saved_box;
  guint added;

  out_of_range_count = unmatched_saved_profile_count(client, profiles, NULL);
  if (out_of_range_count == 0)
    return;

  saved_box = subgroup_list("Saved networks", &saved_list);
  adw_preferences_group_add(group, saved_box);
  added = add_unmatched_saved_profile_rows(GTK_LIST_BOX(saved_list), client, actions, profiles, NULL);
  if (added == 0)
    add_empty_saved_row(GTK_LIST_BOX(saved_list), "No saved Wi-Fi profiles");
}

static void
add_network_row(GtkListBox *list, NMClient *client, NetworkSidebarActions *actions, WifiEntry *entry)
{
  g_autofree char *title = wifi_entry_title(entry);
  g_autofree char *subtitle = wifi_entry_subtitle(entry);
  GtkWidget *row = network_sidebar_action_row(title, subtitle, network_sidebar_signal_icon(nm_access_point_get_strength(entry->ap)));
  WifiRowAction *row_action = g_new0(WifiRowAction, 1);
  gboolean can_activate = entry->active != NULL || (nm_client_networking_get_enabled(client) && nm_client_wireless_get_enabled(client));

  row_action->actions = network_sidebar_actions_ref(actions);
  row_action->device = g_object_ref(entry->device);
  row_action->ap = g_object_ref(entry->ap);
  row_action->active = entry->active != NULL ? g_object_ref(entry->active) : NULL;
  if (entry->saved_connections != NULL && entry->saved_connections->len == 1)
    row_action->saved = g_object_ref(g_ptr_array_index(entry->saved_connections, 0));

  network_sidebar_apply_row_state(row, entry->row_state);

  if (row_action->saved != NULL) {
    GtkWidget *edit_button = network_sidebar_flat_button("document-edit-symbolic", "Edit");
    GtkWidget *remove_button;

    g_signal_connect(edit_button, "clicked", G_CALLBACK(on_wifi_edit_clicked), row_action);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), edit_button);

    remove_button = network_sidebar_flat_button("user-trash-symbolic", "Remove");
    g_signal_connect(remove_button, "clicked", G_CALLBACK(on_wifi_remove_clicked), row_action);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove_button);
  } else if (entry->saved_connections != NULL && entry->saved_connections->len > 1) {
    GtkWidget *profiles_button = network_sidebar_flat_button("document-edit-symbolic", "Saved profiles");
    g_signal_connect(profiles_button, "clicked", G_CALLBACK(on_wifi_profiles_clicked), row_action);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), profiles_button);
  }

  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), can_activate);
  g_signal_connect_data(row, "activated", G_CALLBACK(on_wifi_row_activated), row_action, wifi_row_action_closure_notify, 0);
  gtk_list_box_append(list, row);
}

void
network_sidebar_add_wifi_group(GtkBox *content, NMClient *client, NetworkSidebarActions *actions)
{
  GtkWidget *group = network_sidebar_section_group("Wi-Fi");
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *add_button = network_sidebar_flat_button("list-add-symbolic", "Add Wi-Fi network");
  const GPtrArray *devices = nm_client_get_devices(client);
  gboolean has_wifi_device = FALSE;
  g_autoptr(GPtrArray) saved_profiles = NULL;
  g_autoptr(GPtrArray) entries = NULL;
  g_autoptr(GHashTable) visible_saved_keys = NULL;
  GtkWidget *connected_list = NULL;
  GtkWidget *available_list = NULL;
  GtkWidget *saved_list = NULL;
  GtkWidget *connected_box;
  GtkWidget *available_box;
  GtkWidget *saved_box;
  guint connected_count = 0;
  guint available_count = 0;
  guint saved_count = 0;

  g_signal_connect(add_button, "clicked", G_CALLBACK(on_add_wifi_clicked), actions);
  gtk_box_append(GTK_BOX(header), add_button);

  if (nm_client_wireless_hardware_get_enabled(client)) {
    GtkWidget *wifi_switch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(wifi_switch), nm_client_wireless_get_enabled(client));
    gtk_widget_set_valign(wifi_switch, GTK_ALIGN_CENTER);
    g_signal_connect(wifi_switch, "notify::active", G_CALLBACK(on_wifi_switch), actions);
    gtk_box_append(GTK_BOX(header), wifi_switch);
  }
  adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(group), header);

  for (guint i = 0; devices != NULL && i < devices->len; i++) {
    NMDevice *device = g_ptr_array_index((GPtrArray *) devices, i);
    if (nm_device_get_device_type(device) == NM_DEVICE_TYPE_WIFI) {
      has_wifi_device = TRUE;
      break;
    }
  }
  saved_profiles = saved_wifi_profiles(client);

  if (!has_wifi_device) {
    network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "No Wi-Fi devices", "No wireless adapters are managed", "network-wireless-symbolic");
    add_saved_profiles_only_group(ADW_PREFERENCES_GROUP(group), client, actions, saved_profiles);
    gtk_box_append(content, group);
    return;
  }
  if (!nm_client_wireless_hardware_get_enabled(client)) {
    network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "Wi-Fi hardware is disabled", "Check airplane mode or the wireless hardware switch", "network-wireless-disabled-symbolic");
    add_saved_profiles_only_group(ADW_PREFERENCES_GROUP(group), client, actions, saved_profiles);
    gtk_box_append(content, group);
    return;
  }
  if (!nm_client_wireless_get_enabled(client)) {
    network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "Wi-Fi is disabled", "Enable Wi-Fi from the section header", "network-wireless-disabled-symbolic");
    add_saved_profiles_only_group(ADW_PREFERENCES_GROUP(group), client, actions, saved_profiles);
    gtk_box_append(content, group);
    return;
  }

  entries = build_wifi_entries(client);
  if (entries->len == 0 && saved_profiles->len == 0) {
    const char *scan_error = network_sidebar_actions_get_wifi_scan_error(actions);

    if (network_sidebar_actions_get_wifi_scan_in_progress(actions))
      network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "Scanning for networks...", "Updating nearby Wi-Fi access points", "view-refresh-symbolic");
    else if (scan_error == NULL)
      network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "No networks found", "No access points are currently visible", "network-wireless-signal-none-symbolic");
    if (scan_error != NULL)
      network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "Wi-Fi scan failed", scan_error, "dialog-warning-symbolic");
    gtk_box_append(content, group);
    return;
  }

  if (network_sidebar_actions_get_wifi_scan_error(actions) != NULL)
    network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "Wi-Fi scan failed", network_sidebar_actions_get_wifi_scan_error(actions), "dialog-warning-symbolic");

  visible_saved_keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (entries->len > 0)
    mark_duplicate_access_points(entries);
  for (guint i = 0; i < entries->len; i++) {
    WifiEntry *entry = g_ptr_array_index(entries, i);
    entry->saved_connections = saved_wifi_connections_for_ap(client, entry->device, entry->ap);
    g_ptr_array_sort(entry->saved_connections, saved_connection_compare);
    for (guint j = 0; entry->saved_connections != NULL && j < entry->saved_connections->len; j++) {
      NMRemoteConnection *saved = g_ptr_array_index(entry->saved_connections, j);
      g_hash_table_add(visible_saved_keys, saved_profile_key(saved));
    }
  }
  g_ptr_array_sort(entries, wifi_entry_compare);

  connected_box = subgroup_list(NULL, &connected_list);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), connected_box);

  available_box = subgroup_list("Available networks", &available_list);
  gtk_widget_add_css_class(available_box, "network-subgroup-separated");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), available_box);

  saved_box = subgroup_list("Saved networks", &saved_list);
  gtk_widget_add_css_class(saved_box, "network-subgroup-separated");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), saved_box);

  for (guint i = 0; i < entries->len; i++) {
    WifiEntry *entry = g_ptr_array_index(entries, i);
    if (row_state_preferred(entry->row_state)) {
      add_network_row(GTK_LIST_BOX(connected_list), client, actions, entry);
      connected_count++;
    } else {
      add_network_row(GTK_LIST_BOX(available_list), client, actions, entry);
      available_count++;
    }
  }
  connected_count += add_unmatched_connected_saved_profile_rows(GTK_LIST_BOX(connected_list),
                                                               client,
                                                               actions,
                                                               saved_profiles,
                                                               visible_saved_keys);
  saved_count += add_unmatched_saved_profile_rows(GTK_LIST_BOX(saved_list), client, actions, saved_profiles, visible_saved_keys);

  if (connected_count == 0)
    gtk_list_box_append(GTK_LIST_BOX(connected_list), network_sidebar_action_row("No connected networks", NULL, NULL));
  if (available_count == 0)
    gtk_list_box_append(GTK_LIST_BOX(available_list), network_sidebar_action_row("No other networks in range", NULL, NULL));
  if (saved_count == 0)
    add_empty_saved_row(GTK_LIST_BOX(saved_list), "No saved networks out of range");

  gtk_box_append(content, group);
}
