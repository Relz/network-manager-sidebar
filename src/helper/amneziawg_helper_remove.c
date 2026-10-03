#include "helper/amneziawg_helper_remove.h"

#include "helper/amneziawg_helper_tunnel.h"
#include "helper/amneziawg_helper_util.h"

#include <limits.h>

NetworkSidebarAmneziaWGHelperExit
awg_helper_remove(AwgHelperStorage *storage,
                  AwgHelperProfiles *profiles,
                  const char *name,
                  const NetworkSidebarAmneziaWGDeadlines *deadlines)
{
  char config_name[NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH + 6];
  char snapshot_path[PATH_MAX];
  AwgHelperTunnel *tunnel;
  NetworkSidebarAmneziaWGHelperExit result = awg_helper_phase_stop_status(
    deadlines->forward_at);

  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  if (!network_sidebar_amneziawg_name_is_valid(name) ||
      !awg_helper_storage_make_config_filename(name, config_name, sizeof(config_name)) ||
      !awg_helper_storage_make_runtime_config_path(name, snapshot_path, sizeof(snapshot_path)))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_NAME;
  /* Reject an unsafe saved file before interrupting connectivity. Invalid
   * configuration contents do not prevent removal using the runtime snapshot. */
  if (awg_helper_storage_secure_file_state(
        storage, AWG_HELPER_STORAGE_CONFIG, config_name, 0600) == AWG_HELPER_ENTRY_INVALID)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;

  tunnel = awg_helper_tunnel_new(storage);
  result = awg_helper_tunnel_down(tunnel, name, config_name, snapshot_path, deadlines);
  awg_helper_tunnel_free(tunnel);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  result = awg_helper_phase_stop_status(deadlines->forward_at);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;

  /* Delete rechecks file security and the absence of both the journal and
   * interface. Successful Down alone is not permission to unlink a profile. */
  return awg_helper_profiles_delete(profiles, name, config_name, deadlines->forward_at);
}
