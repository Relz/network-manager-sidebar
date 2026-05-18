#include "data/labels.h"

#include <string.h>

static void
append_codepoint_escape(GString *result, gunichar ch)
{
  if (ch <= 0xff)
    g_string_append_printf(result, "\\x%02x", ch);
  else if (ch <= 0xffff)
    g_string_append_printf(result, "\\u%04x", ch);
  else
    g_string_append_printf(result, "\\U%08x", ch);
}

static gboolean
unicode_category_hidden(GUnicodeType type)
{
  return type == G_UNICODE_CONTROL ||
         type == G_UNICODE_FORMAT ||
         type == G_UNICODE_UNASSIGNED ||
         type == G_UNICODE_PRIVATE_USE ||
         type == G_UNICODE_SURROGATE ||
         type == G_UNICODE_LINE_SEPARATOR ||
         type == G_UNICODE_PARAGRAPH_SEPARATOR;
}

static gboolean
next_utf8_char_allow_nul(const char *p, const char *end, gunichar *out_ch, const char **out_next)
{
  gunichar ch;

  if (p >= end)
    return FALSE;
  if (*p == '\0') {
    *out_ch = '\0';
    *out_next = p + 1;
    return TRUE;
  }

  ch = g_utf8_get_char_validated(p, end - p);
  if (ch == (gunichar) -1 || ch == (gunichar) -2)
    return FALSE;
  *out_ch = ch;
  *out_next = g_utf8_next_char(p);
  return TRUE;
}

static gboolean
utf8_bytes_validate_allow_nul(const guint8 *bytes, gsize length)
{
  const char *start;
  const char *end;

  if (bytes == NULL && length > 0)
    return FALSE;

  start = (const char *) bytes;
  end = start + length;
  for (const char *p = start; p < end;) {
    gunichar ch;
    const char *next;

    if (!next_utf8_char_allow_nul(p, end, &ch, &next))
      return FALSE;
    (void) ch;
    p = next;
  }
  return TRUE;
}

static char *
display_text_bytes(const guint8 *bytes, gsize length, const char *fallback)
{
  g_autoptr(GString) result = NULL;
  const char *start;
  const char *end;
  const char *p;
  const char *first_non_space = NULL;
  const char *last_non_space = NULL;

  if (bytes == NULL || length == 0)
    return g_strdup(fallback != NULL ? fallback : "Unknown");
  if (!utf8_bytes_validate_allow_nul(bytes, length))
    return g_strdup(fallback != NULL ? fallback : "Unknown");

  start = (const char *) bytes;
  end = start + length;
  for (p = start; p < end;) {
    gunichar ch;
    const char *next;

    if (!next_utf8_char_allow_nul(p, end, &ch, &next))
      return g_strdup(fallback != NULL ? fallback : "Unknown");
    if (!g_unichar_isspace(ch)) {
      if (first_non_space == NULL)
        first_non_space = p;
      last_non_space = p;
    }
    p = next;
  }

  if (first_non_space == NULL)
    return g_strdup(fallback != NULL ? fallback : "Unknown");

  result = g_string_new(NULL);
  for (p = start; p < end;) {
    gunichar ch;
    const char *next;
    GUnicodeType type;
    gboolean edge_space;

    if (!next_utf8_char_allow_nul(p, end, &ch, &next))
      return g_strdup(fallback != NULL ? fallback : "Unknown");
    type = g_unichar_type(ch);
    edge_space = g_unichar_isspace(ch) && (p < first_non_space || p > last_non_space);

    if (ch == '\0')
      g_string_append(result, "\\x00");
    else if (ch == '\n')
      g_string_append(result, "\\n");
    else if (ch == '\r')
      g_string_append(result, "\\r");
    else if (ch == '\t')
      g_string_append(result, "\\t");
    else if (edge_space || unicode_category_hidden(type))
      append_codepoint_escape(result, ch);
    else
      g_string_append_len(result, p, next - p);
    p = next;
  }

  return g_string_free(g_steal_pointer(&result), FALSE);
}

static char *
ssid_whitespace_text(const guint8 *bytes, gsize length)
{
  g_autoptr(GString) result = g_string_new(NULL);
  const char *start = (const char *) bytes;
  const char *end = start + length;

  for (const char *p = start; p < end;) {
    gunichar ch = g_utf8_get_char(p);
    if (ch == '\0')
      g_string_append(result, "\\x00");
    else if (ch == '\n')
      g_string_append(result, "\\n");
    else if (ch == '\r')
      g_string_append(result, "\\r");
    else if (ch == '\t')
      g_string_append(result, "\\t");
    else
      append_codepoint_escape(result, ch);
    p = g_utf8_next_char(p);
  }
  return g_string_free(g_steal_pointer(&result), FALSE);
}

static char *
title_from_nick(const char *nick, const char *fallback)
{
  g_auto(GStrv) parts = NULL;
  g_autoptr(GString) result = NULL;
  g_autofree char *normalized = NULL;

  if (nick == NULL || *nick == '\0')
    return g_strdup(fallback != NULL ? fallback : "Unknown");

  normalized = g_strdup(nick);
  g_strdelimit(normalized, "_", '-');
  parts = g_strsplit(normalized, "-", -1);
  result = g_string_new(NULL);
  for (guint i = 0; parts[i] != NULL; i++) {
    const char *part = parts[i];

    if (*part == '\0')
      continue;
    if (result->len > 0)
      g_string_append_c(result, ' ');
    if (g_ascii_strcasecmp(part, "ap") == 0)
      g_string_append(result, "AP");
    else if (g_ascii_strcasecmp(part, "dhcp") == 0)
      g_string_append(result, "DHCP");
    else if (g_ascii_strcasecmp(part, "dns") == 0)
      g_string_append(result, "DNS");
    else if (g_ascii_strcasecmp(part, "ip") == 0)
      g_string_append(result, "IP");
    else if (g_ascii_strcasecmp(part, "ipv4") == 0)
      g_string_append(result, "IPv4");
    else if (g_ascii_strcasecmp(part, "ipv6") == 0)
      g_string_append(result, "IPv6");
    else if (g_ascii_strcasecmp(part, "mac") == 0)
      g_string_append(result, "MAC");
    else if (g_ascii_strcasecmp(part, "mtu") == 0)
      g_string_append(result, "MTU");
    else if (g_ascii_strcasecmp(part, "ppp") == 0)
      g_string_append(result, "PPP");
    else if (g_ascii_strcasecmp(part, "vpn") == 0)
      g_string_append(result, "VPN");
    else if (g_ascii_strcasecmp(part, "wpa") == 0)
      g_string_append(result, "WPA");
    else if (g_ascii_strcasecmp(part, "ssid") == 0)
      g_string_append(result, "SSID");
    else {
      g_autofree char *lower = g_ascii_strdown(part, -1);
      if (result->len == 0)
        lower[0] = g_ascii_toupper(lower[0]);
      g_string_append(result, lower);
    }
  }

  if (result->len == 0)
    return g_strdup(fallback != NULL ? fallback : "Unknown");
  return g_string_free(g_steal_pointer(&result), FALSE);
}

static char *
title_case_words(const char *value, const char *fallback)
{
  g_auto(GStrv) parts = NULL;
  g_autoptr(GString) result = NULL;
  g_autofree char *normalized = NULL;

  if (value == NULL || *value == '\0')
    return g_strdup(fallback != NULL ? fallback : "Unknown");

  normalized = g_strdup(value);
  g_strdelimit(normalized, "-_", ' ');
  parts = g_strsplit(normalized, " ", -1);
  result = g_string_new(NULL);
  for (guint i = 0; parts[i] != NULL; i++) {
    const char *part = parts[i];
    g_autofree char *lower = NULL;

    if (*part == '\0')
      continue;
    lower = g_ascii_strdown(part, -1);
    lower[0] = g_ascii_toupper(lower[0]);
    if (result->len > 0)
      g_string_append_c(result, ' ');
    g_string_append(result, lower);
  }

  if (result->len == 0)
    return g_strdup(fallback != NULL ? fallback : "Unknown");
  return g_string_free(g_steal_pointer(&result), FALSE);
}

char *
network_sidebar_display_text(const char *value, const char *fallback)
{
  if (value == NULL)
    return g_strdup(fallback != NULL ? fallback : "Unknown");
  return display_text_bytes((const guint8 *) value, strlen(value), fallback);
}

char *
network_sidebar_enum_label(GType enum_type, int value, const char *fallback)
{
  GEnumClass *enum_class = g_type_class_ref(enum_type);
  GEnumValue *enum_value = NULL;
  char *label;

  if (enum_class != NULL)
    enum_value = g_enum_get_value(enum_class, value);
  label = title_from_nick(enum_value != NULL ? enum_value->value_nick : NULL, fallback);
  if (enum_class != NULL)
    g_type_class_unref(enum_class);
  return label;
}

char *
network_sidebar_connection_name(NMConnection *connection, const char *fallback)
{
  return network_sidebar_display_text(connection != NULL ? nm_connection_get_id(connection) : NULL, fallback);
}

char *
network_sidebar_active_connection_name(NMActiveConnection *active, const char *fallback)
{
  return network_sidebar_display_text(active != NULL ? nm_active_connection_get_id(active) : NULL, fallback);
}

char *
network_sidebar_connection_type_label(const char *connection_type)
{
  if (g_strcmp0(connection_type, NM_SETTING_WIRED_SETTING_NAME) == 0)
    return g_strdup("Ethernet");
  if (g_strcmp0(connection_type, NM_SETTING_WIRELESS_SETTING_NAME) == 0)
    return g_strdup("Wi-Fi");
  if (g_strcmp0(connection_type, NM_SETTING_VPN_SETTING_NAME) == 0)
    return g_strdup("VPN");
  if (g_strcmp0(connection_type, "loopback") == 0)
    return g_strdup("Loopback");
  if (g_strcmp0(connection_type, "wireguard") == 0)
    return g_strdup("WireGuard");
  if (g_strcmp0(connection_type, "ovs-bridge") == 0)
    return g_strdup("OVS bridge");
  if (g_strcmp0(connection_type, "ovs-interface") == 0)
    return g_strdup("OVS interface");
  if (g_strcmp0(connection_type, "ovs-patch") == 0)
    return g_strdup("OVS patch");
  if (g_strcmp0(connection_type, "ovs-port") == 0)
    return g_strdup("OVS port");
  if (g_strcmp0(connection_type, "pppoe") == 0)
    return g_strdup("PPPoE");
  if (g_strcmp0(connection_type, "vlan") == 0)
    return g_strdup("VLAN");
  return title_case_words(connection_type, "Unknown");
}

char *
network_sidebar_device_type_label(NMDeviceType device_type)
{
  if (device_type == NM_DEVICE_TYPE_ETHERNET)
    return g_strdup("Ethernet");
  if (device_type == NM_DEVICE_TYPE_WIFI)
    return g_strdup("Wi-Fi");
  return network_sidebar_enum_label(NM_TYPE_DEVICE_TYPE, device_type, "Unknown");
}

char *
network_sidebar_device_state_label(NMDeviceState state)
{
  return network_sidebar_enum_label(NM_TYPE_DEVICE_STATE, state, "Unknown");
}

char *
network_sidebar_active_state_label(NMActiveConnectionState state)
{
  return network_sidebar_enum_label(NM_TYPE_ACTIVE_CONNECTION_STATE, state, "Unknown");
}

char *
network_sidebar_vpn_state_label(NMVpnConnectionState state)
{
  return network_sidebar_enum_label(NM_TYPE_VPN_CONNECTION_STATE, state, "Unknown");
}

static char *
hex_bytes(const guint8 *bytes, gsize length)
{
  g_autoptr(GString) hex = g_string_new(NULL);

  for (gsize i = 0; i < length; i++) {
    if (i > 0)
      g_string_append_c(hex, ' ');
    g_string_append_printf(hex, "%02x", bytes[i]);
  }
  return g_string_free(g_steal_pointer(&hex), FALSE);
}

char *
network_sidebar_ssid_text_from_bytes(GBytes *ssid)
{
  const guint8 *bytes;
  gsize length = 0;

  if (ssid == NULL)
    return g_strdup("Hidden network");
  bytes = g_bytes_get_data(ssid, &length);
  if (bytes == NULL || length == 0)
    return g_strdup("Hidden network");
  if (memchr(bytes, '\0', length) == NULL) {
    g_autofree char *nm_text = nm_utils_ssid_to_utf8(bytes, length);
    if (nm_text != NULL && *nm_text != '\0') {
      if (!utf8_bytes_validate_allow_nul(bytes, length)) {
        g_autofree char *hex = hex_bytes(bytes, length);
        g_autofree char *safe = network_sidebar_display_text(nm_text, "");
        return g_strdup_printf("%s (hex: %s)", safe != NULL && *safe != '\0' ? safe : nm_text, hex);
      }
    }
  }
  if (!utf8_bytes_validate_allow_nul(bytes, length)) {
    g_autofree char *hex = hex_bytes(bytes, length);
    return g_strdup_printf("Invalid UTF-8 SSID (hex: %s)", hex);
  }
  {
    g_autofree char *safe = display_text_bytes(bytes, length, "");
    if (safe != NULL && *safe != '\0')
      return g_steal_pointer(&safe);
    return ssid_whitespace_text(bytes, length);
  }
}

char *
network_sidebar_ap_ssid_text(NMAccessPoint *ap)
{
  if (ap == NULL)
    return g_strdup("Hidden network");
  return network_sidebar_ssid_text_from_bytes(nm_access_point_get_ssid(ap));
}

char *
network_sidebar_ap_security_label(NMAccessPoint *ap)
{
  NM80211ApFlags flags;
  NM80211ApSecurityFlags wpa;
  NM80211ApSecurityFlags rsn;

  if (ap == NULL)
    return g_strdup("Unknown security");

  flags = nm_access_point_get_flags(ap);
  wpa = nm_access_point_get_wpa_flags(ap);
  rsn = nm_access_point_get_rsn_flags(ap);

  gboolean has_wpa = wpa != NM_802_11_AP_SEC_NONE;
  gboolean has_rsn = rsn != NM_802_11_AP_SEC_NONE;
  NM80211ApSecurityFlags security = wpa | rsn;

  if (security & NM_802_11_AP_SEC_KEY_MGMT_OWE_TM)
    return g_strdup("Enhanced Open transition");
  if (security & NM_802_11_AP_SEC_KEY_MGMT_OWE)
    return g_strdup("Enhanced Open");
  if (security & NM_802_11_AP_SEC_KEY_MGMT_SAE)
    return g_strdup((security & NM_802_11_AP_SEC_KEY_MGMT_PSK) ? "WPA2/3 Personal" : "WPA3 Personal");
  if (security & NM_802_11_AP_SEC_KEY_MGMT_PSK) {
    if (has_rsn && has_wpa)
      return g_strdup("WPA/WPA2 Personal");
    return g_strdup(has_rsn ? "WPA2 Personal" : "WPA Personal");
  }
#ifdef NM_802_11_AP_SEC_KEY_MGMT_EAP_SUITE_B_192
  if (security & NM_802_11_AP_SEC_KEY_MGMT_EAP_SUITE_B_192)
    return g_strdup((security & NM_802_11_AP_SEC_KEY_MGMT_802_1X) ? "WPA2/3 Enterprise" : "WPA3 Enterprise");
#endif
  if (security & NM_802_11_AP_SEC_KEY_MGMT_802_1X) {
    if (has_rsn && has_wpa)
      return g_strdup("WPA/WPA2 Enterprise");
    return g_strdup(has_rsn ? "WPA2 Enterprise" : "WPA Enterprise");
  }
  if (has_rsn && has_wpa)
    return g_strdup("WPA/WPA2 Advanced");
  if (has_rsn)
    return g_strdup("WPA2 Advanced");
  if (has_wpa)
    return g_strdup("WPA Advanced");
  if (flags & NM_802_11_AP_FLAGS_PRIVACY)
    return g_strdup("WEP");
  return g_strdup("Open");
}

static void
append_profile_detail(GString *details, const char *detail)
{
  if (detail == NULL || *detail == '\0')
    return;
  if (details->len > 0)
    g_string_append(details, " - ");
  g_string_append(details, detail);
}

char *
network_sidebar_wifi_profile_subtitle(NMRemoteConnection *connection)
{
  g_autoptr(GString) details = g_string_new(NULL);
  NMSettingWireless *wireless = nm_connection_get_setting_wireless(NM_CONNECTION(connection));
  NMSettingWirelessSecurity *security = nm_connection_get_setting_wireless_security(NM_CONNECTION(connection));
  NMSettingConnection *setting_connection = nm_connection_get_setting_connection(NM_CONNECTION(connection));

  if (wireless != NULL) {
    GBytes *ssid = nm_setting_wireless_get_ssid(wireless);
    gsize ssid_length = 0;
    const char *bssid = nm_setting_wireless_get_bssid(wireless);
    const char *band = nm_setting_wireless_get_band(wireless);
    guint32 channel = nm_setting_wireless_get_channel(wireless);

    if (ssid != NULL)
      g_bytes_get_data(ssid, &ssid_length);
    if (ssid_length > 0) {
      g_autofree char *ssid_text = network_sidebar_ssid_text_from_bytes(ssid);
      g_autofree char *ssid_detail = g_strdup_printf("SSID %s", ssid_text);
      append_profile_detail(details, ssid_detail);
    }
    if (bssid != NULL && *bssid != '\0') {
      g_autofree char *bssid_detail = g_strdup_printf("BSSID %s", bssid);
      append_profile_detail(details, bssid_detail);
    }
    if (band != NULL && *band != '\0') {
      g_autofree char *band_detail = g_strdup_printf("Band %s", band);
      append_profile_detail(details, band_detail);
    }
    if (channel != 0) {
      g_autofree char *channel_detail = g_strdup_printf("Channel %u", channel);
      append_profile_detail(details, channel_detail);
    }
  }

  if (security != NULL) {
    g_autofree char *security_label = network_sidebar_wifi_key_mgmt_label(nm_setting_wireless_security_get_key_mgmt(security));
    append_profile_detail(details, security_label);
  } else {
    append_profile_detail(details, "Open");
  }

  if (setting_connection != NULL) {
    const char *interface_name = nm_setting_connection_get_interface_name(setting_connection);
    if (interface_name != NULL && *interface_name != '\0') {
      g_autofree char *interface_detail = g_strdup_printf("Interface %s", interface_name);
      append_profile_detail(details, interface_detail);
    }
    if (!nm_setting_connection_get_autoconnect(setting_connection))
      append_profile_detail(details, "Autoconnect off");
  }

  if (details->len > 0)
    return g_string_free(g_steal_pointer(&details), FALSE);

  {
    const char *uuid = nm_connection_get_uuid(NM_CONNECTION(connection));
    return uuid != NULL && *uuid != '\0' ? g_strdup_printf("UUID %s", uuid) : g_strdup("");
  }
}

char *
network_sidebar_wifi_key_mgmt_label(const char *key_mgmt)
{
  g_autofree char *normalized = NULL;

  if (key_mgmt == NULL || *key_mgmt == '\0')
    return g_strdup("Unknown security");
  if (g_strcmp0(key_mgmt, "none") == 0)
    return g_strdup("WEP");
  if (g_strcmp0(key_mgmt, "ieee8021x") == 0)
    return g_strdup("802.1X");
  if (g_strcmp0(key_mgmt, "owe") == 0)
    return g_strdup("Enhanced Open");
  if (g_strcmp0(key_mgmt, "sae") == 0)
    return g_strdup("WPA3 Personal");
  if (g_strcmp0(key_mgmt, "wpa-eap") == 0)
    return g_strdup("WPA/WPA2 Enterprise");
  if (g_strcmp0(key_mgmt, "wpa-eap-suite-b-192") == 0)
    return g_strdup("WPA3 Enterprise");
  if (g_strcmp0(key_mgmt, "wpa-psk") == 0)
    return g_strdup("WPA/WPA2 Personal");

  normalized = g_strdup(key_mgmt);
  g_strdelimit(normalized, "-_", ' ');
  return g_utf8_strup(normalized, -1);
}

char *
network_sidebar_frequency_label(guint32 mhz)
{
  if (mhz == 0)
    return g_strdup("Unknown band");
  if (mhz >= 5925 && mhz <= 7125)
    return g_strdup("6 GHz");
  if (mhz >= 4900 && mhz < 5925)
    return g_strdup("5 GHz");
  if (mhz >= 2400 && mhz < 2500)
    return g_strdup("2.4 GHz");
  return g_strdup_printf("%u MHz", mhz);
}

const char *
network_sidebar_signal_icon(guint8 strength)
{
  if (strength >= 80)
    return "network-wireless-signal-excellent-symbolic";
  if (strength >= 60)
    return "network-wireless-signal-good-symbolic";
  if (strength >= 40)
    return "network-wireless-signal-ok-symbolic";
  if (strength >= 20)
    return "network-wireless-signal-weak-symbolic";
  return "network-wireless-signal-none-symbolic";
}
