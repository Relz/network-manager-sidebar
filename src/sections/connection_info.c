#include "sections/connection_info.h"

#include "data/labels.h"
#include "sections/helpers.h"

static void
add_info_row(AdwPreferencesGroup *group, const char *label, const char *value)
{
  GtkWidget *row;

  if (value == NULL || *value == '\0')
    return;
  row = network_sidebar_action_row(label, value, NULL);
  adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
  adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
  adw_preferences_group_add(group, row);
}

static char *
active_title(NMActiveConnection *active, NMDevice *device)
{
  const char *id = active != NULL ? nm_active_connection_get_id(active) : NULL;
  const char *iface = device != NULL ? nm_device_get_iface(device) : NULL;
  const char *ip_iface = device != NULL ? nm_device_get_ip_iface(device) : NULL;

  if (id != NULL && *id != '\0')
    return network_sidebar_display_text(id, "Connection Information");
  if (iface != NULL && *iface != '\0')
    return network_sidebar_display_text(iface, "Unknown interface");
  if (ip_iface != NULL && *ip_iface != '\0')
    return network_sidebar_display_text(ip_iface, "Unknown interface");
  return network_sidebar_connection_type_label(active != NULL ? nm_active_connection_get_connection_type(active) : NULL);
}

static char *
addresses_label(NMIPConfig *config)
{
  GPtrArray *addresses;
  g_autoptr(GString) value = g_string_new(NULL);

  if (config == NULL)
    return NULL;
  addresses = nm_ip_config_get_addresses(config);
  for (guint i = 0; addresses != NULL && i < addresses->len; i++) {
    NMIPAddress *address = g_ptr_array_index(addresses, i);
    const char *address_text = nm_ip_address_get_address(address);
    if (address_text == NULL || *address_text == '\0')
      continue;
    if (value->len > 0)
      g_string_append(value, ", ");
    g_string_append_printf(value, "%s/%u", address_text, nm_ip_address_get_prefix(address));
  }
  return value->len > 0 ? g_string_free(g_steal_pointer(&value), FALSE) : NULL;
}

static char *
route_text(NMIPRoute *route)
{
  const char *dest = nm_ip_route_get_dest(route);
  guint prefix = nm_ip_route_get_prefix(route);
  const char *next_hop = nm_ip_route_get_next_hop(route);
  gint64 metric = nm_ip_route_get_metric(route);
  g_autoptr(GString) value = g_string_new(NULL);

  if (prefix == 0 && (dest == NULL || *dest == '\0' || g_strcmp0(dest, "0.0.0.0") == 0 || g_strcmp0(dest, "::") == 0))
    g_string_append(value, "default");
  else if (dest != NULL && *dest != '\0')
    g_string_append_printf(value, "%s/%u", dest, prefix);
  else
    g_string_append(value, "route");

  if (next_hop != NULL && *next_hop != '\0')
    g_string_append_printf(value, " via %s", next_hop);
  if (metric >= 0)
    g_string_append_printf(value, " metric %" G_GINT64_FORMAT, metric);
  return g_string_free(g_steal_pointer(&value), FALSE);
}

static char *
routes_label(NMIPConfig *config)
{
  GPtrArray *routes;
  g_autoptr(GString) value = g_string_new(NULL);

  if (config == NULL)
    return NULL;
  routes = nm_ip_config_get_routes(config);
  for (guint i = 0; routes != NULL && i < routes->len; i++) {
    NMIPRoute *route = g_ptr_array_index(routes, i);
    g_autofree char *text = route_text(route);
    if (text == NULL || *text == '\0')
      continue;
    if (value->len > 0)
      g_string_append(value, ", ");
    g_string_append(value, text);
  }
  return value->len > 0 ? g_string_free(g_steal_pointer(&value), FALSE) : NULL;
}

static char *
strv_join_label(const char *const *values)
{
  if (values == NULL || values[0] == NULL)
    return NULL;
  return g_strjoinv(", ", (char **) values);
}

static gboolean
add_ip_rows(AdwPreferencesGroup *group, const char *prefix, NMIPConfig *config)
{
  g_autofree char *addresses = addresses_label(config);
  g_autofree char *routes = routes_label(config);
  g_autofree char *address_label = g_strdup_printf("%s address", prefix);
  g_autofree char *gateway_label = g_strdup_printf("%s gateway", prefix);
  g_autofree char *routes_row_label = g_strdup_printf("%s routes", prefix);
  g_autofree char *dns_label = g_strdup_printf("%s DNS", prefix);
  g_autofree char *domains_label = g_strdup_printf("%s domains", prefix);
  g_autofree char *search_label = g_strdup_printf("%s search domains", prefix);
  g_autofree char *dns = config != NULL ? strv_join_label(nm_ip_config_get_nameservers(config)) : NULL;
  g_autofree char *domains = config != NULL ? strv_join_label(nm_ip_config_get_domains(config)) : NULL;
  g_autofree char *searches = config != NULL ? strv_join_label(nm_ip_config_get_searches(config)) : NULL;
  gboolean added = FALSE;

  if (config == NULL)
    return FALSE;
  if (addresses != NULL) {
    add_info_row(group, address_label, addresses);
    added = TRUE;
  }
  if (nm_ip_config_get_gateway(config) != NULL && *nm_ip_config_get_gateway(config) != '\0') {
    add_info_row(group, gateway_label, nm_ip_config_get_gateway(config));
    added = TRUE;
  }
  if (routes != NULL) {
    add_info_row(group, routes_row_label, routes);
    added = TRUE;
  }
  if (dns != NULL) {
    add_info_row(group, dns_label, dns);
    added = TRUE;
  }
  if (domains != NULL) {
    add_info_row(group, domains_label, domains);
    added = TRUE;
  }
  if (searches != NULL) {
    add_info_row(group, search_label, searches);
    added = TRUE;
  }
  return added;
}

static gboolean
add_ip_rows_with_fallback(AdwPreferencesGroup *group, const char *prefix, NMIPConfig *primary, NMIPConfig *fallback)
{
  if (add_ip_rows(group, prefix, primary))
    return TRUE;
  if (fallback != NULL && fallback != primary)
    return add_ip_rows(group, prefix, fallback);
  return FALSE;
}

static gboolean
add_active_ip_rows(AdwPreferencesGroup *group, NMActiveConnection *active, gboolean include_ip_iface)
{
  gboolean added = FALSE;

  if (include_ip_iface && g_object_class_find_property(G_OBJECT_GET_CLASS(active), "ip-iface") != NULL) {
    g_autofree char *ip_iface = NULL;
    g_object_get(active, "ip-iface", &ip_iface, NULL);
    add_info_row(group, "IP interface", ip_iface);
    if (ip_iface != NULL && *ip_iface != '\0')
      added = TRUE;
  }
  added |= add_ip_rows(group, "IPv4", nm_active_connection_get_ip4_config(active));
  added |= add_ip_rows(group, "IPv6", nm_active_connection_get_ip6_config(active));
  return added;
}

static void
add_device_rows(AdwPreferencesGroup *group, NMActiveConnection *active, NMDevice *device, gboolean include_active_ip_config)
{
  const char *iface = nm_device_get_iface(device);
  const char *ip_iface = nm_device_get_ip_iface(device);
  const char *type_description = nm_device_get_type_description(device);
  g_autofree char *fallback_type = network_sidebar_connection_type_label(nm_active_connection_get_connection_type(active));
  g_autofree char *state = network_sidebar_device_state_label(nm_device_get_state(device));

  add_info_row(group, "Interface", iface != NULL && *iface != '\0' ? iface : "Unknown");
  add_info_row(group, "Device type", type_description != NULL && *type_description != '\0' ? type_description : fallback_type);
  add_info_row(group, "Device state", state);
  if (ip_iface != NULL && *ip_iface != '\0' && g_strcmp0(ip_iface, iface) != 0)
    add_info_row(group, "IP interface", ip_iface);
  add_info_row(group, "Hardware address", nm_device_get_hw_address(device));
  add_info_row(group, "Driver", nm_device_get_driver(device));

  if (NM_IS_DEVICE_ETHERNET(device)) {
    guint32 speed = nm_device_ethernet_get_speed(NM_DEVICE_ETHERNET(device));
    if (speed != 0) {
      g_autofree char *speed_text = g_strdup_printf("%u Mb/s", speed);
      add_info_row(group, "Speed", speed_text);
    }
  }

  if (NM_IS_DEVICE_WIFI(device)) {
    NMAccessPoint *ap = nm_device_wifi_get_active_access_point(NM_DEVICE_WIFI(device));
    if (ap != NULL) {
      guint32 frequency_mhz = nm_access_point_get_frequency(ap);
      g_autofree char *ssid = network_sidebar_ap_ssid_text(ap);
      g_autofree char *signal = g_strdup_printf("%u%%", nm_access_point_get_strength(ap));
      g_autofree char *security = network_sidebar_ap_security_label(ap);
      g_autofree char *frequency = network_sidebar_frequency_label(frequency_mhz);
      g_autofree char *raw_frequency = frequency_mhz != 0 ? g_strdup_printf("%u MHz", frequency_mhz) : NULL;
      g_autofree char *frequency_detail = frequency_mhz != 0 && g_strcmp0(frequency, raw_frequency) != 0 ? g_strdup_printf("%s (%u MHz)", frequency, frequency_mhz) : g_strdup(frequency);
      add_info_row(group, "SSID", ssid);
      add_info_row(group, "Signal", signal);
      add_info_row(group, "Security", security);
      add_info_row(group, "Frequency", frequency_detail);
      add_info_row(group, "BSSID", nm_access_point_get_bssid(ap));
    }
  }

  if (nm_active_connection_get_vpn(active)) {
    if (include_active_ip_config)
      add_active_ip_rows(group, active, FALSE);
  } else {
    NMIPConfig *fallback_ip4 = include_active_ip_config ? nm_active_connection_get_ip4_config(active) : NULL;
    NMIPConfig *fallback_ip6 = include_active_ip_config ? nm_active_connection_get_ip6_config(active) : NULL;
    add_ip_rows_with_fallback(group, "IPv4", nm_device_get_ip4_config(device), fallback_ip4);
    add_ip_rows_with_fallback(group, "IPv6", nm_device_get_ip6_config(device), fallback_ip6);
  }
}

static void
add_connection_rows(AdwPreferencesGroup *group, NMActiveConnection *active, NMDevice *device, gboolean include_active_ip_config)
{
  g_autofree char *connection = network_sidebar_active_connection_name(active, "Unknown");
  g_autofree char *type = network_sidebar_connection_type_label(nm_active_connection_get_connection_type(active));
  g_autofree char *state = network_sidebar_active_state_label(nm_active_connection_get_state(active));
  g_autoptr(GString) default_routes = g_string_new(NULL);

  add_info_row(group, "Connection", connection);
  add_info_row(group, "Type", type);
  add_info_row(group, "State", state);
  if (nm_active_connection_get_vpn(active) && NM_IS_VPN_CONNECTION(active)) {
    g_autofree char *vpn_state = network_sidebar_vpn_state_label(nm_vpn_connection_get_vpn_state(NM_VPN_CONNECTION(active)));
    add_info_row(group, "VPN state", vpn_state);
  }
  add_info_row(group, "UUID", nm_active_connection_get_uuid(active));
  if (nm_active_connection_get_default(active))
    g_string_append(default_routes, "IPv4");
  if (nm_active_connection_get_default6(active)) {
    if (default_routes->len > 0)
      g_string_append(default_routes, ", ");
    g_string_append(default_routes, "IPv6");
  }
  add_info_row(group, "Default route", default_routes->str);

  if (device != NULL)
    add_device_rows(group, active, device, include_active_ip_config);
  else if (include_active_ip_config)
    add_active_ip_rows(group, active, TRUE);
}

void
network_sidebar_add_connection_info_content(GtkBox *content, NMClient *client)
{
  const GPtrArray *active_connections = nm_client_get_active_connections(client);
  gboolean added = FALSE;

  for (guint i = 0; active_connections != NULL && i < active_connections->len; i++) {
    NMActiveConnection *active = g_ptr_array_index((GPtrArray *) active_connections, i);
    const GPtrArray *raw_devices = nm_active_connection_get_devices(active);
    g_autoptr(GPtrArray) devices = g_ptr_array_new();
    gboolean devices_available = raw_devices != NULL;
    gboolean has_multiple_devices;

    if (g_strcmp0(nm_active_connection_get_connection_type(active), "loopback") == 0)
      continue;

    for (guint j = 0; raw_devices != NULL && j < raw_devices->len; j++) {
      NMDevice *device = g_ptr_array_index((GPtrArray *) raw_devices, j);
      if (g_strcmp0(nm_device_get_iface(device), "lo") != 0)
        g_ptr_array_add(devices, device);
    }

    if (nm_active_connection_get_state(active) == NM_ACTIVE_CONNECTION_STATE_ACTIVATED &&
        devices_available &&
        devices->len == 0 &&
        !nm_active_connection_get_vpn(active) &&
        nm_active_connection_get_ip4_config(active) == NULL &&
        nm_active_connection_get_ip6_config(active) == NULL)
      continue;

    has_multiple_devices = devices->len > 1;
    if (devices->len > 0) {
      for (guint j = 0; j < devices->len; j++) {
        NMDevice *device = g_ptr_array_index(devices, j);
        g_autofree char *title = active_title(active, device);
        GtkWidget *group;

        if (has_multiple_devices) {
          const char *device_label = nm_device_get_iface(device);
          if (device_label == NULL || *device_label == '\0')
            device_label = nm_device_get_ip_iface(device);
          if (device_label == NULL || *device_label == '\0')
            device_label = nm_device_get_type_description(device);
          if (device_label != NULL && *device_label != '\0') {
            g_autofree char *with_device = g_strdup_printf("%s (%s)", title, device_label);
            g_free(title);
            title = g_steal_pointer(&with_device);
          }
        }

        group = network_sidebar_section_group(title);
        add_connection_rows(ADW_PREFERENCES_GROUP(group), active, device, !has_multiple_devices);
        gtk_box_append(content, group);
        added = TRUE;
      }
      if (has_multiple_devices) {
        g_autofree char *title = active_title(active, NULL);
        g_autofree char *ip_title = g_strdup_printf("%s (IP configuration)", title);
        GtkWidget *group = network_sidebar_section_group(ip_title);
        if (add_active_ip_rows(ADW_PREFERENCES_GROUP(group), active, TRUE)) {
          gtk_box_append(content, group);
          added = TRUE;
        }
      }
    } else {
      g_autofree char *title = NULL;
      GtkWidget *group;
      const char *id = nm_active_connection_get_id(active);

      if (id != NULL && *id != '\0')
        title = network_sidebar_display_text(id, "Connection Information");
      else if (nm_active_connection_get_vpn(active))
        title = g_strdup("VPN");
      else
        title = network_sidebar_connection_type_label(nm_active_connection_get_connection_type(active));

      group = network_sidebar_section_group(title);
      add_connection_rows(ADW_PREFERENCES_GROUP(group), active, NULL, TRUE);
      gtk_box_append(content, group);
      added = TRUE;
    }
  }

  if (!added) {
    GtkWidget *group = network_sidebar_section_group("Connection Information");
    network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "No connection details available", "NetworkManager has no connection details to show", "dialog-information-symbolic");
    gtk_box_append(content, group);
  }
}
