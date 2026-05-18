#ifndef NETWORK_SIDEBAR_DATA_LABELS_H
#define NETWORK_SIDEBAR_DATA_LABELS_H

#include <NetworkManager.h>

G_BEGIN_DECLS

char *network_sidebar_display_text(const char *value, const char *fallback);
char *network_sidebar_enum_label(GType enum_type, int value, const char *fallback);
char *network_sidebar_connection_name(NMConnection *connection, const char *fallback);
char *network_sidebar_active_connection_name(NMActiveConnection *active, const char *fallback);
char *network_sidebar_connection_type_label(const char *connection_type);
char *network_sidebar_device_type_label(NMDeviceType device_type);
char *network_sidebar_device_state_label(NMDeviceState state);
char *network_sidebar_active_state_label(NMActiveConnectionState state);
char *network_sidebar_vpn_state_label(NMVpnConnectionState state);
char *network_sidebar_ssid_text_from_bytes(GBytes *ssid);
char *network_sidebar_ap_ssid_text(NMAccessPoint *ap);
char *network_sidebar_ap_security_label(NMAccessPoint *ap);
char *network_sidebar_wifi_profile_subtitle(NMRemoteConnection *connection);
char *network_sidebar_wifi_key_mgmt_label(const char *key_mgmt);
char *network_sidebar_frequency_label(guint32 mhz);
const char *network_sidebar_signal_icon(guint8 strength);

G_END_DECLS

#endif
