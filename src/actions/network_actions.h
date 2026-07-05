#ifndef NETWORK_SIDEBAR_ACTIONS_NETWORK_ACTIONS_H
#define NETWORK_SIDEBAR_ACTIONS_NETWORK_ACTIONS_H

#include <adwaita.h>
#include <NetworkManager.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _NetworkSidebarActions NetworkSidebarActions;

typedef AdwToast *(*NetworkSidebarToastCallback)(const char *message, gpointer user_data);
typedef void (*NetworkSidebarScheduleRefreshCallback)(guint delay_ms, gpointer user_data);

NetworkSidebarActions *network_sidebar_actions_new(NMClient *client,
                                                   NetworkSidebarToastCallback toast,
                                                   NetworkSidebarScheduleRefreshCallback schedule_refresh,
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
void network_sidebar_actions_activate_saved_wifi_profile_for_ap(NetworkSidebarActions *actions,
                                                               NMRemoteConnection *connection,
                                                               NMDeviceWifi *device,
                                                               NMAccessPoint *ap);
void network_sidebar_actions_activate_vpn_connection(NetworkSidebarActions *actions, NMRemoteConnection *connection);
void network_sidebar_actions_deactivate(NetworkSidebarActions *actions, NMActiveConnection *active);
void network_sidebar_actions_delete_connection(NetworkSidebarActions *actions, NMRemoteConnection *connection);
void network_sidebar_actions_confirm_delete_connection(NetworkSidebarActions *actions,
                                                       NMRemoteConnection *connection,
                                                       const char *profile_kind);
void network_sidebar_actions_edit_connection(NetworkSidebarActions *actions, NMRemoteConnection *connection);
void network_sidebar_actions_open_editor(NetworkSidebarActions *actions, const char *const *args);
gboolean network_sidebar_actions_connection_editor_available(void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarActions, network_sidebar_actions_unref)

G_END_DECLS

#endif
