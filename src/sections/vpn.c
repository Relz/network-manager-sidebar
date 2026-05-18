#include "sections/vpn.h"

#include "data/connection_state.h"
#include "data/labels.h"
#include "sections/helpers.h"

typedef struct {
  NetworkSidebarActions *actions;
  NMRemoteConnection *connection;
  NMActiveConnection *active;
  gboolean can_activate;
} VpnRowAction;

typedef struct {
  GHashTable *service_types;
  gboolean discovery_failed;
} VpnPluginState;

static void
vpn_row_action_free(VpnRowAction *data)
{
  network_sidebar_actions_unref(data->actions);
  g_clear_object(&data->connection);
  g_clear_object(&data->active);
  g_free(data);
}

static void
vpn_row_action_closure_notify(gpointer data, GClosure *closure)
{
  (void) closure;
  vpn_row_action_free(data);
}

static NMActiveConnection *
active_by_uuid(NMClient *client, const char *uuid)
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

static void
on_vpn_row_activated(GtkListBoxRow *row, gpointer user_data)
{
  VpnRowAction *data = user_data;
  (void) row;

  if (data->active != NULL)
    network_sidebar_actions_deactivate(data->actions, data->active);
  else if (!data->can_activate)
    return;
  else
    network_sidebar_actions_activate_vpn_connection(data->actions, data->connection);
}

static void
on_vpn_edit_clicked(GtkButton *button, gpointer user_data)
{
  VpnRowAction *data = user_data;
  (void) button;
  network_sidebar_actions_edit_connection(data->actions, data->connection);
}

static void
on_vpn_remove_clicked(GtkButton *button, gpointer user_data)
{
  VpnRowAction *data = user_data;
  (void) button;
  network_sidebar_actions_confirm_delete_connection(data->actions, data->connection, "VPN");
}

static void
on_create_vpn_clicked(GtkButton *button, gpointer user_data)
{
  (void) button;
  network_sidebar_actions_open_editor(user_data, (const char *const[]) { "--create", "--type=vpn", NULL });
}

static VpnPluginState
vpn_plugin_state_load(void)
{
  VpnPluginState state = { 0 };
  GSList *plugins = nm_vpn_plugin_info_list_load();
  g_auto(GStrv) service_types = NULL;

  state.service_types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (plugins == NULL)
    return state;

  service_types = nm_vpn_plugin_info_list_get_service_types(plugins, TRUE, FALSE);
  for (guint i = 0; service_types != NULL && service_types[i] != NULL; i++)
    g_hash_table_add(state.service_types, g_strdup(service_types[i]));
  for (GSList *iter = plugins; iter != NULL; iter = iter->next) {
    const char *service = nm_vpn_plugin_info_get_service(iter->data);
    if (service != NULL && *service != '\0')
      g_hash_table_add(state.service_types, g_strdup(service));
  }
  g_slist_free_full(plugins, g_object_unref);
  return state;
}

static void
vpn_plugin_state_clear(VpnPluginState *state)
{
  g_clear_pointer(&state->service_types, g_hash_table_unref);
}

static gboolean
vpn_has_base_connection(NMClient *client)
{
  const GPtrArray *active_connections = nm_client_get_active_connections(client);
  for (guint i = 0; active_connections != NULL && i < active_connections->len; i++) {
    NMActiveConnection *active = g_ptr_array_index((GPtrArray *) active_connections, i);
    if (!nm_active_connection_get_vpn(active) &&
        g_strcmp0(nm_active_connection_get_connection_type(active), NM_SETTING_VPN_SETTING_NAME) != 0 &&
        nm_active_connection_get_state(active) == NM_ACTIVE_CONNECTION_STATE_ACTIVATED)
      return TRUE;
  }
  return FALSE;
}

static gboolean
vpn_profile_plugin_available(NMRemoteConnection *connection, const VpnPluginState *state, gboolean *unknown)
{
  NMSettingVpn *vpn = nm_connection_get_setting_vpn(NM_CONNECTION(connection));
  const char *service_type = vpn != NULL ? nm_setting_vpn_get_service_type(vpn) : NULL;

  *unknown = FALSE;
  if (service_type == NULL || *service_type == '\0') {
    if (state->discovery_failed) {
      *unknown = TRUE;
      return TRUE;
    }
    return FALSE;
  }
  if (state->service_types != NULL && g_hash_table_contains(state->service_types, service_type))
    return TRUE;
  if (state->discovery_failed) {
    *unknown = TRUE;
    return TRUE;
  }
  return FALSE;
}

static gint
vpn_connection_compare(gconstpointer left, gconstpointer right)
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

static guint
vpn_profile_name_count(GPtrArray *profiles, const char *name)
{
  guint count = 0;

  for (guint i = 0; profiles != NULL && i < profiles->len; i++) {
    NMRemoteConnection *connection = g_ptr_array_index(profiles, i);
    const char *candidate = nm_connection_get_id(NM_CONNECTION(connection));
    if (g_strcmp0(candidate != NULL && *candidate != '\0' ? candidate : "VPN", name) == 0)
      count++;
  }
  return count;
}

static char *
vpn_connection_status(NMActiveConnection *active)
{
  if (active == NULL || !NM_IS_VPN_CONNECTION(active))
    return NULL;
  switch (nm_vpn_connection_get_vpn_state(NM_VPN_CONNECTION(active))) {
  case NM_VPN_CONNECTION_STATE_FAILED:
    return g_strdup("VPN failed");
  case NM_VPN_CONNECTION_STATE_DISCONNECTED:
    return g_strdup("VPN disconnected");
  default:
    return NULL;
  }
}

static char *
join_subtitle_parts(const char *first, const char *second)
{
  if (first != NULL && *first != '\0' && second != NULL && *second != '\0')
    return g_strdup_printf("%s - %s", first, second);
  if (first != NULL && *first != '\0')
    return g_strdup(first);
  if (second != NULL && *second != '\0')
    return g_strdup(second);
  return NULL;
}

void
network_sidebar_add_vpn_group(GtkBox *content, NMClient *client, NetworkSidebarActions *actions)
{
  GtkWidget *group = network_sidebar_section_group("VPN");
  GtkWidget *header_suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *create_button = network_sidebar_flat_button("list-add-symbolic", "Create VPN");
  const GPtrArray *connections = nm_client_get_connections(client);
  g_autoptr(GPtrArray) profiles = g_ptr_array_new_with_free_func(g_object_unref);
  gboolean editor_available = network_sidebar_actions_connection_editor_available();
  VpnPluginState plugin_state = vpn_plugin_state_load();
  gboolean plugins_known_missing = !plugin_state.discovery_failed && g_hash_table_size(plugin_state.service_types) == 0;
  gboolean has_base_connection = vpn_has_base_connection(client);
  gboolean added = FALSE;

  gtk_widget_set_sensitive(create_button, editor_available && !plugins_known_missing);
  if (!editor_available)
    gtk_widget_set_tooltip_text(create_button, "Install nm-connection-editor to create VPN profiles");
  else if (plugins_known_missing)
    gtk_widget_set_tooltip_text(create_button, "Install a NetworkManager VPN plugin to create profiles");
  g_signal_connect(create_button, "clicked", G_CALLBACK(on_create_vpn_clicked), actions);
  gtk_box_append(GTK_BOX(header_suffix), create_button);
  adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(group), header_suffix);

  if (!editor_available)
    network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "VPN editor unavailable", "Install nm-connection-editor to import, create, or edit VPN profiles", "dialog-warning-symbolic");
  if (plugin_state.discovery_failed)
    network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "VPN plugin status unavailable", "Could not verify installed NetworkManager VPN plugins; VPN actions remain available", "dialog-warning-symbolic");
  else if (plugins_known_missing)
    network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "No VPN plugins installed", "Install a NetworkManager VPN plugin to import, create, or activate VPN profiles", "dialog-warning-symbolic");

  for (guint i = 0; connections != NULL && i < connections->len; i++) {
    NMRemoteConnection *connection = g_ptr_array_index((GPtrArray *) connections, i);
    if (g_strcmp0(nm_connection_get_connection_type(NM_CONNECTION(connection)), NM_SETTING_VPN_SETTING_NAME) == 0)
      g_ptr_array_add(profiles, g_object_ref(connection));
  }
  g_ptr_array_sort(profiles, vpn_connection_compare);

  for (guint i = 0; i < profiles->len; i++) {
    NMRemoteConnection *connection = g_ptr_array_index(profiles, i);
    const char *uuid;
    NMActiveConnection *active;
    g_autofree char *title = NULL;
    g_autofree char *subtitle = NULL;
    GtkWidget *row;
    GtkWidget *edit_button;
    GtkWidget *remove_button;
    VpnRowAction *row_action;
    gboolean plugin_unknown = FALSE;
    gboolean plugin_available;
    gboolean can_activate;
    g_autofree char *activation_note = NULL;
    g_autofree char *duplicate_note = NULL;
    g_autofree char *status = NULL;

    uuid = nm_connection_get_uuid(NM_CONNECTION(connection));
    active = active_by_uuid(client, uuid);
    title = network_sidebar_connection_name(NM_CONNECTION(connection), "VPN");
    if (vpn_profile_name_count(profiles, title) > 1 && uuid != NULL)
      duplicate_note = g_strdup_printf("UUID %s", uuid);
    plugin_available = vpn_profile_plugin_available(connection, &plugin_state, &plugin_unknown);
    if (active == NULL && !nm_client_networking_get_enabled(client))
      activation_note = g_strdup("Networking is disabled");
    else if (active == NULL && !has_base_connection)
      activation_note = g_strdup("VPN needs an active network connection");
    else if (active == NULL && !plugin_available)
      activation_note = g_strdup("VPN plugin is not installed for this profile");
    can_activate = active != NULL || activation_note == NULL;
    status = vpn_connection_status(active);
    subtitle = activation_note != NULL ? join_subtitle_parts(activation_note, duplicate_note) : join_subtitle_parts(status, duplicate_note);
    row = network_sidebar_action_row(title, subtitle, "network-vpn-symbolic");
    network_sidebar_apply_row_state(row, network_sidebar_vpn_row_state(active));
    if (activation_note != NULL)
      gtk_widget_set_tooltip_text(row, activation_note);

    row_action = g_new0(VpnRowAction, 1);
    row_action->actions = network_sidebar_actions_ref(actions);
    row_action->connection = g_object_ref(connection);
    row_action->active = active != NULL ? g_object_ref(active) : NULL;
    row_action->can_activate = can_activate;

    edit_button = network_sidebar_flat_button("document-edit-symbolic", "Edit");
    if (!editor_available)
      gtk_widget_set_tooltip_text(edit_button, "Install nm-connection-editor to edit VPN profiles");
    else if (!plugin_available)
      gtk_widget_set_tooltip_text(edit_button, "Install the NetworkManager VPN plugin for this profile");
    else if (plugin_unknown)
      gtk_widget_set_tooltip_text(edit_button, "VPN plugin support could not be verified");
    gtk_widget_set_sensitive(edit_button, editor_available && plugin_available);
    g_signal_connect(edit_button, "clicked", G_CALLBACK(on_vpn_edit_clicked), row_action);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), edit_button);

    remove_button = network_sidebar_flat_button("user-trash-symbolic", "Remove");
    g_signal_connect(remove_button, "clicked", G_CALLBACK(on_vpn_remove_clicked), row_action);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove_button);

    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), can_activate);
    g_signal_connect_data(row, "activated", G_CALLBACK(on_vpn_row_activated), row_action, vpn_row_action_closure_notify, 0);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    added = TRUE;
  }

  if (!added) {
    const char *empty_subtitle;

    if (editor_available && !plugins_known_missing)
      empty_subtitle = "Use Import or Create to add one";
    else if (!editor_available)
      empty_subtitle = "Install nm-connection-editor to add one";
    else
      empty_subtitle = "Install a NetworkManager VPN plugin to add one";
    network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "No VPN profiles", empty_subtitle, "network-vpn-symbolic");
  }

  gtk_box_append(content, group);
  vpn_plugin_state_clear(&plugin_state);
}
