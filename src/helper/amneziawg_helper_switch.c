#include "helper/amneziawg_helper_switch.h"

#include "helper/amneziawg_helper_util.h"
#include "helper/amneziawg_helper_session.h"

#include <limits.h>

static NetworkSidebarAmneziaWGHelperExit
previous_session_failure(NetworkSidebarAmneziaWGHelperExit result)
{
  if (result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_TIMED_OUT ||
      result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SIGNALED)
    return result;
  return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SWITCH_CLEANUP_FAILED;
}

static NetworkSidebarAmneziaWGHelperExit
disconnect_previous(AwgHelperStorage *storage,
                     const char *name,
                     const NetworkSidebarAmneziaWGDeadlines *deadlines)
{
  char config_name[NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH + 6];
  char snapshot_path[PATH_MAX];
  AwgHelperTunnel *tunnel;
  gboolean marker;
  gboolean interface;
  NetworkSidebarAmneziaWGHelperExit result = awg_helper_phase_stop_status(
    deadlines->forward_at);

  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  if (!awg_helper_storage_make_config_filename(name, config_name, sizeof(config_name)) ||
      !awg_helper_storage_make_runtime_config_path(name, snapshot_path, sizeof(snapshot_path)))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_NAME;
  /* DNS continuity/rollback state belongs to this session, not the switch. */
  tunnel = awg_helper_tunnel_new(storage);
  result = awg_helper_tunnel_down(tunnel, name, config_name, snapshot_path, deadlines);
  awg_helper_tunnel_free(tunnel);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  result = awg_helper_phase_stop_status(deadlines->forward_at);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  result = awg_helper_session_runtime_state(storage, name, &marker, &interface, NULL);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  return marker || interface ? NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED :
                               NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
}

NetworkSidebarAmneziaWGHelperExit
awg_helper_switch_up(AwgHelperStorage *storage,
                      AwgHelperProfiles *profiles,
                      const char *name,
                      const NetworkSidebarAmneziaWGDeadlines *deadlines)
{
  g_autoptr(GPtrArray) previous = NULL;
  g_autoptr(GPtrArray) remaining = NULL;
  AwgHelperActivation *activation = NULL;
  AwgHelperTunnel *target;
  NetworkSidebarAmneziaWGHelperExit result;

  if (!network_sidebar_amneziawg_name_is_valid(name))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_NAME;
  result = awg_helper_storage_list_runtime_names(storage, deadlines->forward_at, &previous);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return previous_session_failure(result);
  target = awg_helper_tunnel_new(storage);
  result = awg_helper_tunnel_prepare_up(target, profiles, name,
                                        deadlines->forward_at, &activation);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    goto out;

  /* Finish the whole admission pass before disconnecting even the first
   * session. In particular, a malformed later journal must not cause a
   * partially applied switch. Runtime-only sessions need no saved profile. */
  for (guint i = 0; i < previous->len; i++) {
    result = awg_helper_tunnel_check_down(target, g_ptr_array_index(previous, i),
                                          deadlines->forward_at);
    if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
      goto previous_failed;
  }
  for (guint i = 0; i < previous->len; i++) {
    result = disconnect_previous(storage, g_ptr_array_index(previous, i), deadlines);
    if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
      goto previous_failed;
  }

  /* The lock serializes helpers, but a privileged external writer can still
   * change runtime storage. Do not start the target with a remaining journal. */
  result = awg_helper_storage_list_runtime_names(storage, deadlines->forward_at, &remaining);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    goto previous_failed;
  if (remaining->len != 0) {
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
    goto previous_failed;
  }
  result = awg_helper_phase_stop_status(deadlines->forward_at);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    goto out;
  /* One deadline and supervision lifetime span every phase. Successful Down
   * does not commit or release that lifetime; only the final Up may do so. */
  result = awg_helper_tunnel_activate(target, activation, deadlines);
  goto out;

previous_failed:
  result = previous_session_failure(result);
out:
  awg_helper_activation_free(activation);
  awg_helper_tunnel_free(target);
  return result;
}
