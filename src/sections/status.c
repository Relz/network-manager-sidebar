#include "sections/status.h"

#include "data/connection_state.h"
#include "data/labels.h"
#include "sections/helpers.h"

void
network_sidebar_add_status_group(GtkBox *content, NMClient *client, NetworkSidebarActions *actions)
{
  GtkWidget *group = NULL;
  const GPtrArray *devices = nm_client_get_devices(client);
  gboolean added = FALSE;
  (void) actions;

  for (guint i = 0; devices != NULL && i < devices->len; i++) {
    NMDevice *device = g_ptr_array_index((GPtrArray *) devices, i);
    NMActiveConnection *active;
    g_autofree char *title = NULL;
    g_autofree char *iface = NULL;
    g_autofree char *state = NULL;
    g_autofree char *subtitle = NULL;
    GtkWidget *row;

    if (nm_device_get_device_type(device) != NM_DEVICE_TYPE_ETHERNET)
      continue;
    active = nm_device_get_active_connection(device);
    if (network_sidebar_device_row_state(device, active) != NETWORK_SIDEBAR_ROW_STATE_ACTIVE)
      continue;
    if (group == NULL)
      group = network_sidebar_section_group("Wired");

    iface = network_sidebar_display_text(nm_device_get_iface(device), "Unknown interface");
    title = active != NULL ? network_sidebar_active_connection_name(active, iface) : g_strdup(iface);
    state = network_sidebar_device_state_label(nm_device_get_state(device));
    subtitle = g_strdup_printf("%s - %s", iface, state);
    row = network_sidebar_action_row(title, subtitle, "network-wired-symbolic");
    network_sidebar_apply_row_state(row, network_sidebar_device_row_state(device, active));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    added = TRUE;
  }

  if (added)
    gtk_box_append(content, group);
}
