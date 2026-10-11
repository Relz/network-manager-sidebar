#include "data/vpn_profiles.h"

#include "data/labels.h"

typedef struct {
  GHashTable *service_types;
  gboolean discovery_failed;
} VpnPluginState;

static gboolean profile_is_wireguard(NMRemoteConnection *connection) {
  return g_strcmp0(nm_connection_get_connection_type(NM_CONNECTION(connection)),
                   NM_SETTING_WIREGUARD_SETTING_NAME) == 0;
}

static const char *profile_kind_label(NMRemoteConnection *connection) {
  return profile_is_wireguard(connection) ? "WireGuard" : "VPN";
}

static NMActiveConnection *active_by_uuid(NMClient *client, const char *uuid) {
  const GPtrArray *active_connections =
      nm_client_get_active_connections(client);

  if (uuid == NULL)
    return NULL;
  for (guint i = 0; active_connections != NULL && i < active_connections->len;
       i++) {
    NMActiveConnection *active =
        g_ptr_array_index((GPtrArray *)active_connections, i);
    if (g_strcmp0(uuid, nm_active_connection_get_uuid(active)) == 0)
      return active;
  }
  return NULL;
}

static VpnPluginState vpn_plugin_state_load(void) {
  VpnPluginState state = {0};
  GSList *plugins = nm_vpn_plugin_info_list_load();
  g_auto(GStrv) service_types = NULL;

  state.service_types =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (plugins == NULL)
    return state;

  service_types =
      nm_vpn_plugin_info_list_get_service_types(plugins, TRUE, FALSE);
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

static gboolean vpn_has_base_connection(NMClient *client) {
  const GPtrArray *active_connections =
      nm_client_get_active_connections(client);
  for (guint i = 0; active_connections != NULL && i < active_connections->len;
       i++) {
    NMActiveConnection *active =
        g_ptr_array_index((GPtrArray *)active_connections, i);
    if (!nm_active_connection_get_vpn(active) &&
        g_strcmp0(nm_active_connection_get_connection_type(active),
                  NM_SETTING_VPN_SETTING_NAME) != 0 &&
        nm_active_connection_get_state(active) ==
            NM_ACTIVE_CONNECTION_STATE_ACTIVATED)
      return TRUE;
  }
  return FALSE;
}

static NetworkSidebarVpnPluginStatus
vpn_profile_plugin_status(NMRemoteConnection *connection,
                          const VpnPluginState *state) {
  NMSettingVpn *vpn = nm_connection_get_setting_vpn(NM_CONNECTION(connection));
  const char *service_type =
      vpn != NULL ? nm_setting_vpn_get_service_type(vpn) : NULL;

  if (service_type != NULL && *service_type != '\0' &&
      g_hash_table_contains(state->service_types, service_type))
    return NETWORK_SIDEBAR_VPN_PLUGIN_AVAILABLE;
  if (state->discovery_failed)
    return NETWORK_SIDEBAR_VPN_PLUGIN_UNKNOWN;
  return NETWORK_SIDEBAR_VPN_PLUGIN_MISSING;
}

static gint vpn_connection_compare(gconstpointer left, gconstpointer right) {
  NMRemoteConnection *left_connection = *(NMRemoteConnection *const *)left;
  NMRemoteConnection *right_connection = *(NMRemoteConnection *const *)right;
  const char *left_id = nm_connection_get_id(NM_CONNECTION(left_connection));
  const char *right_id = nm_connection_get_id(NM_CONNECTION(right_connection));
  g_autofree char *left_folded =
      g_utf8_casefold(left_id != NULL ? left_id : "", -1);
  g_autofree char *right_folded =
      g_utf8_casefold(right_id != NULL ? right_id : "", -1);
  int id_compare = g_strcmp0(left_folded, right_folded);

  if (id_compare != 0)
    return id_compare;
  return g_strcmp0(nm_connection_get_uuid(NM_CONNECTION(left_connection)),
                   nm_connection_get_uuid(NM_CONNECTION(right_connection)));
}

static guint vpn_profile_name_count(GPtrArray *profiles, const char *name) {
  guint count = 0;

  for (guint i = 0; i < profiles->len; i++) {
    NMRemoteConnection *connection = g_ptr_array_index(profiles, i);
    const char *candidate = nm_connection_get_id(NM_CONNECTION(connection));
    if (g_strcmp0(candidate != NULL && *candidate != '\0'
                      ? candidate
                      : profile_kind_label(connection),
                  name) == 0)
      count++;
  }
  return count;
}

static char *vpn_connection_status(NMActiveConnection *active) {
  if (active == NULL)
    return NULL;
  if (!NM_IS_VPN_CONNECTION(active)) {
    NMActiveConnectionState state = nm_active_connection_get_state(active);

    return state != NM_ACTIVE_CONNECTION_STATE_ACTIVATED
               ? network_sidebar_active_state_label(state)
               : NULL;
  }
  switch (nm_vpn_connection_get_vpn_state(NM_VPN_CONNECTION(active))) {
  case NM_VPN_CONNECTION_STATE_FAILED:
    return g_strdup("VPN failed");
  case NM_VPN_CONNECTION_STATE_DISCONNECTED:
    return g_strdup("VPN disconnected");
  default:
    return NULL;
  }
}

static NetworkSidebarVpnActivationReason
vpn_profile_activation_reason(const NetworkSidebarVpnProfile *profile,
                              gboolean networking_enabled,
                              gboolean has_base_connection) {
  if (profile->active != NULL)
    return NETWORK_SIDEBAR_VPN_ACTIVATION_ALLOWED;
  if (!networking_enabled)
    return NETWORK_SIDEBAR_VPN_ACTIVATION_NETWORKING_DISABLED;
  if (profile->kind == NETWORK_SIDEBAR_VPN_PROFILE_PLUGIN &&
      !has_base_connection)
    return NETWORK_SIDEBAR_VPN_ACTIVATION_NO_BASE_CONNECTION;
  if (profile->plugin_status == NETWORK_SIDEBAR_VPN_PLUGIN_MISSING)
    return NETWORK_SIDEBAR_VPN_ACTIVATION_PLUGIN_MISSING;
  return NETWORK_SIDEBAR_VPN_ACTIVATION_ALLOWED;
}

static void vpn_profile_free(gpointer user_data) {
  NetworkSidebarVpnProfile *profile = user_data;

  g_object_unref(profile->connection);
  g_clear_object(&profile->active);
  g_free(profile->name);
  g_free(profile->uuid);
  g_free(profile->status);
  g_free(profile);
}

void network_sidebar_vpn_profiles_free(NetworkSidebarVpnProfiles *profiles) {
  if (profiles == NULL)
    return;
  g_ptr_array_unref(profiles->profiles);
  g_free(profiles);
}

NetworkSidebarVpnProfiles *network_sidebar_vpn_profiles_load(NMClient *client) {
  const GPtrArray *connections = nm_client_get_connections(client);
  g_autoptr(GPtrArray) sorted_connections =
      g_ptr_array_new_with_free_func(g_object_unref);
  VpnPluginState plugin_state = vpn_plugin_state_load();
  gboolean networking_enabled = nm_client_networking_get_enabled(client);
  gboolean has_base_connection = vpn_has_base_connection(client);
  NetworkSidebarVpnProfiles *snapshot = g_new0(NetworkSidebarVpnProfiles, 1);

  snapshot->profiles = g_ptr_array_new_with_free_func(vpn_profile_free);
  snapshot->plugin_status = plugin_state.discovery_failed
                                ? NETWORK_SIDEBAR_VPN_PLUGIN_UNKNOWN
                            : g_hash_table_size(plugin_state.service_types) == 0
                                ? NETWORK_SIDEBAR_VPN_PLUGIN_MISSING
                                : NETWORK_SIDEBAR_VPN_PLUGIN_AVAILABLE;
  snapshot->can_create_vpn =
      snapshot->plugin_status != NETWORK_SIDEBAR_VPN_PLUGIN_MISSING;

  for (guint i = 0; connections != NULL && i < connections->len; i++) {
    NMRemoteConnection *connection =
        g_ptr_array_index((GPtrArray *)connections, i);
    if (g_strcmp0(nm_connection_get_connection_type(NM_CONNECTION(connection)),
                  NM_SETTING_VPN_SETTING_NAME) == 0 ||
        profile_is_wireguard(connection))
      g_ptr_array_add(sorted_connections, g_object_ref(connection));
  }
  g_ptr_array_sort(sorted_connections, vpn_connection_compare);

  for (guint i = 0; i < sorted_connections->len; i++) {
    NMRemoteConnection *connection = g_ptr_array_index(sorted_connections, i);
    NetworkSidebarVpnProfile *profile = g_new0(NetworkSidebarVpnProfile, 1);
    NMActiveConnection *active;

    profile->connection = g_object_ref(connection);
    profile->kind = profile_is_wireguard(connection)
                        ? NETWORK_SIDEBAR_VPN_PROFILE_WIREGUARD
                        : NETWORK_SIDEBAR_VPN_PROFILE_PLUGIN;
    profile->name = network_sidebar_connection_name(
        NM_CONNECTION(connection), profile_kind_label(connection));
    profile->uuid = g_strdup(nm_connection_get_uuid(NM_CONNECTION(connection)));
    profile->duplicate_name =
        vpn_profile_name_count(sorted_connections, profile->name) > 1;
    active = active_by_uuid(client, profile->uuid);
    profile->active = active != NULL ? g_object_ref(active) : NULL;
    profile->status = vpn_connection_status(active);
    profile->row_state = network_sidebar_vpn_row_state(active);
    profile->plugin_status =
        profile->kind == NETWORK_SIDEBAR_VPN_PROFILE_WIREGUARD
            ? NETWORK_SIDEBAR_VPN_PLUGIN_AVAILABLE
            : vpn_profile_plugin_status(connection, &plugin_state);
    profile->activation_reason = vpn_profile_activation_reason(
        profile, networking_enabled, has_base_connection);
    profile->can_activate =
        profile->activation_reason == NETWORK_SIDEBAR_VPN_ACTIVATION_ALLOWED;
    profile->can_edit =
        profile->plugin_status != NETWORK_SIDEBAR_VPN_PLUGIN_MISSING;
    g_ptr_array_add(snapshot->profiles, profile);
  }

  g_hash_table_unref(plugin_state.service_types);
  return snapshot;
}
