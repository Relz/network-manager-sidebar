#define _GNU_SOURCE

#include "amneziawg/amneziawg.h"
#include "amneziawg/deadline.h"
#include "helper/amneziawg_dns.h"
#include "helper/amneziawg_helper_profiles.h"
#include "helper/amneziawg_helper_remove.h"
#include "helper/amneziawg_helper_storage.h"
#include "helper/amneziawg_helper_switch.h"
#include "helper/amneziawg_helper_tunnel.h"
#include "helper/amneziawg_helper_util.h"
#include "helper/amneziawg_process.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

G_STATIC_ASSERT(NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INPUT_FAILED == 16);
G_STATIC_ASSERT(NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SWITCH_CLEANUP_FAILED < 128);

static gboolean
parse_absolute_deadline(const char *text, gint64 *deadline)
{
  char *end = NULL;
  guint64 value;

  if (text == NULL || text[0] == '\0' || deadline == NULL)
    return FALSE;
  for (const char *character = text; *character != '\0'; character++) {
    if (!g_ascii_isdigit(*character))
      return FALSE;
  }
  errno = 0;
  value = g_ascii_strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value > G_MAXINT64)
    return FALSE;
  *deadline = (gint64) value;
  return TRUE;
}

int
main(int argc, char **argv)
{
  AwgHelperStorage *storage = NULL;
  AwgHelperProfiles *profiles = NULL;
  AwgHelperTunnel *tunnel = NULL;
  AwgHelperProfileInput *input = NULL;
  NetworkSidebarAmneziaWGHelperExit result;
  NetworkSidebarAmneziaWGDeadlines deadlines = { 0 };
  char config_name[NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH + 6];
  char snapshot_path[PATH_MAX];
  const char *command;
  const char *name;
  gboolean list_profiles_requested;
  struct sigaction signal_action = { 0 };

  umask(0077);
  if (argc != 5)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_USAGE;
  command = argv[1];
  name = argv[2];
  if (!parse_absolute_deadline(argv[3], &deadlines.forward_at) ||
      !parse_absolute_deadline(argv[4], &deadlines.cleanup_at) ||
      deadlines.forward_at > deadlines.cleanup_at)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_USAGE;
  deadlines.hard_at = deadlines.cleanup_at;
  list_profiles_requested = strcmp(
    command, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_LIST_PROFILES) == 0;
  if (strcmp(command, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_IMPORT) != 0 &&
      strcmp(command, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_REPLACE) != 0 &&
      strcmp(command, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_UP) != 0 &&
      strcmp(command, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_DOWN) != 0 &&
      strcmp(command, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_DELETE) != 0 &&
      !list_profiles_requested)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_USAGE;
  if ((list_profiles_requested && name[0] != '\0') ||
      (!list_profiles_requested &&
       !network_sidebar_amneziawg_name_is_valid(name)))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_NAME;
  if (geteuid() != 0)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_NOT_ROOT;
  if (!network_sidebar_amneziawg_process_init(
        NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_FD, deadlines.cleanup_at))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED;
  signal_action.sa_handler = SIG_IGN;
  sigemptyset(&signal_action.sa_mask);
  if (sigaction(SIGPIPE, &signal_action, NULL) != 0)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED;
  if (!network_sidebar_amneziawg_process_install_handlers())
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED;
  if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED;
  if (!sanitize_gio_environment())
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED;
  if (!list_profiles_requested &&
      (!awg_helper_storage_make_config_filename(name,
                                                 config_name,
                                                 sizeof(config_name)) ||
       !awg_helper_storage_make_runtime_config_path(name,
                                                     snapshot_path,
                                                     sizeof(snapshot_path))))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_NAME;

  if (strcmp(command, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_IMPORT) == 0 ||
      strcmp(command, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_REPLACE) == 0) {
    input = awg_helper_profiles_read_input(deadlines.forward_at, &result);
    if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
      goto out;
  }

  storage = awg_helper_storage_open_runtime(deadlines.forward_at);
  if (storage == NULL) {
    result = awg_helper_phase_failure_or(
      deadlines.forward_at, NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED);
    goto out;
  }
  if (strcmp(command, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_DOWN) == 0) {
    tunnel = awg_helper_tunnel_new(storage);
    result = awg_helper_tunnel_down(tunnel,
                                    name,
                                    config_name,
                                    snapshot_path,
                                    &deadlines);
    goto out;
  }
  if (!awg_helper_storage_open_profiles(storage)) {
    result = awg_helper_phase_failure_or(
      deadlines.forward_at, NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED);
    goto out;
  }
  profiles = awg_helper_profiles_new(storage);
  if (list_profiles_requested) {
    result = awg_helper_profiles_list(profiles, deadlines.forward_at);
  } else if (strcmp(command,
                    NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_IMPORT) == 0) {
    result = awg_helper_profiles_import(profiles,
                                        name,
                                        config_name,
                                        input,
                                        deadlines.forward_at);
  } else if (strcmp(command,
                    NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_REPLACE) == 0) {
    result = awg_helper_profiles_replace(profiles,
                                         name,
                                         config_name,
                                         input,
                                         deadlines.forward_at);
  } else if (strcmp(command,
                    NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_UP) == 0) {
    result = awg_helper_switch_up(storage, profiles, name, &deadlines);
  } else {
    result = awg_helper_remove(storage, profiles, name, &deadlines);
  }

out:
  awg_helper_tunnel_free(tunnel);
  awg_helper_profiles_free(profiles);
  awg_helper_storage_free(storage);
  awg_helper_profile_input_free(input);
  if (!network_sidebar_amneziawg_process_is_committed() &&
      (result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS ||
       result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVENTORY_PARTIAL)) {
    NetworkSidebarAmneziaWGHelperExit stop = awg_helper_phase_stop_status(
      deadlines.forward_at);

    if (stop != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
      result = stop;
  }
  while (waitpid(-1, NULL, WNOHANG) > 0)
    ;
  return result;
}
