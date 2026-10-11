#ifndef NETWORK_SIDEBAR_ACTIONS_NETWORK_ACTIONS_H
#define NETWORK_SIDEBAR_ACTIONS_NETWORK_ACTIONS_H

#include <adwaita.h>
#include <NetworkManager.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _NetworkSidebarActions NetworkSidebarActions;

typedef enum {
  NETWORK_SIDEBAR_PROFILE_WIFI,
  NETWORK_SIDEBAR_PROFILE_VPN,
  NETWORK_SIDEBAR_PROFILE_WIREGUARD,
} NetworkSidebarProfileKind;

typedef AdwToast *(*NetworkSidebarToastCallback)(const char *message, gpointer user_data);
typedef void (*NetworkSidebarScheduleRefreshCallback)(guint delay_ms, gpointer user_data);
typedef void (*NetworkSidebarRequestEditorCallback)(const char *const *args, gpointer user_data);

NetworkSidebarActions *network_sidebar_actions_new(NMClient *client,
                                                   NetworkSidebarToastCallback toast,
                                                   NetworkSidebarScheduleRefreshCallback schedule_refresh,
                                                   NetworkSidebarRequestEditorCallback request_editor,
                                                   gpointer user_data);
NetworkSidebarActions *network_sidebar_actions_ref(NetworkSidebarActions *actions);
void network_sidebar_actions_unref(NetworkSidebarActions *actions);
void network_sidebar_actions_set_parent(NetworkSidebarActions *actions, GtkWindow *parent);

gboolean network_sidebar_actions_get_wifi_scan_in_progress(NetworkSidebarActions *actions);
const char *network_sidebar_actions_get_wifi_scan_error(NetworkSidebarActions *actions);
void network_sidebar_actions_set_networking_enabled(NetworkSidebarActions *actions, gboolean enabled);
void network_sidebar_actions_set_wireless_enabled(NetworkSidebarActions *actions, gboolean enabled);
void network_sidebar_actions_request_scan(NetworkSidebarActions *actions);
void network_sidebar_actions_schedule_refresh(NetworkSidebarActions *actions, guint delay_ms);
void network_sidebar_actions_activate_connection(NetworkSidebarActions *actions,
                                                 NMConnection *connection,
                                                 NMDevice *device,
                                                 const char *specific_object);
void network_sidebar_actions_connect_wifi(NetworkSidebarActions *actions,
                                           NMDeviceWifi *device,
                                           NMAccessPoint *ap);
void network_sidebar_actions_activate_saved_wifi_profile(NetworkSidebarActions *actions, NMRemoteConnection *connection);
void network_sidebar_actions_activate_saved_wifi_profile_on_device(NetworkSidebarActions *actions,
                                                                   NMRemoteConnection *connection,
                                                                   NMDeviceWifi *device,
                                                                   NMAccessPoint *ap);
void network_sidebar_actions_activate_vpn_connection(NetworkSidebarActions *actions, NMRemoteConnection *connection);
void network_sidebar_actions_activate_wireguard_connection(NetworkSidebarActions *actions, NMRemoteConnection *connection);
void network_sidebar_actions_deactivate(NetworkSidebarActions *actions, NMActiveConnection *active);
void network_sidebar_actions_delete_connection(NetworkSidebarActions *actions, NMRemoteConnection *connection);
void network_sidebar_actions_confirm_delete_connection(NetworkSidebarActions *actions,
                                                       NMRemoteConnection *connection,
                                                       const char *profile_kind);
/* Request adapters only; the profile-editor presenter owns the workflow. */
void network_sidebar_actions_edit_connection(NetworkSidebarActions *actions, NMRemoteConnection *connection);
void network_sidebar_actions_create_profile(NetworkSidebarActions *actions, NetworkSidebarProfileKind kind);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarActions, network_sidebar_actions_unref)

G_END_DECLS

#endif
