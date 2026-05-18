#include "cli/cli_args.h"

#include "core/config.h"
#include "core/ipc.h"
#include "core/ipc_commands.h"
#include "core/target_output.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define NETWORK_SIDEBAR_QUIT_WAIT_TIMEOUT_SECONDS 3.0
#define NETWORK_SIDEBAR_QUIT_WAIT_POLL_SECONDS 0.05

static int report_unacknowledged_command(const char *command);
static int report_no_running_instance(const char *command);
static int finish_quit_command(void);
static int run_application_command(const char *argv0, const char *command);

int
main(int argc, char **argv)
{
  NetworkSidebarCliArgs args = { 0 };
  g_autoptr(GError) error = NULL;
  g_autofree char *target_output_name = NULL;
  NetworkSidebarIPCCommandStatus status;

  if (!network_sidebar_cli_args_parse(&argc, &argv, &args, &error)) {
    g_printerr("%s\n", error != NULL ? error->message : "nm-sidebar: error: failed to parse command line");
    return 2;
  }
  if (args.should_exit) {
    int result = args.exit_status;
    network_sidebar_cli_args_clear(&args);
    return result;
  }

  target_output_name = network_sidebar_target_output_from_environment();

  if (g_strcmp0(args.command, NETWORK_SIDEBAR_COMMAND_BACKGROUND) == 0) {
    status = network_sidebar_send_command(NETWORK_SIDEBAR_COMMAND_PING, NULL, &error);
    if (error != NULL) {
      g_printerr("nm-sidebar: %s\n", error->message);
      network_sidebar_cli_args_clear(&args);
      return 1;
    }
    if (status == NETWORK_SIDEBAR_IPC_COMMAND_ACKNOWLEDGED) {
      network_sidebar_cli_args_clear(&args);
      return 0;
    }
    if (status == NETWORK_SIDEBAR_IPC_COMMAND_DELIVERED) {
      int result = report_unacknowledged_command(args.command);
      network_sidebar_cli_args_clear(&args);
      return result;
    }
  } else if (network_sidebar_command_is_ipc_only(args.command)) {
    status = network_sidebar_send_command(args.command, target_output_name, &error);
    if (error != NULL) {
      g_printerr("nm-sidebar: %s\n", error->message);
      network_sidebar_cli_args_clear(&args);
      return 1;
    }
    if (status == NETWORK_SIDEBAR_IPC_COMMAND_ACKNOWLEDGED) {
      int result = g_strcmp0(args.command, NETWORK_SIDEBAR_COMMAND_QUIT) == 0 ? finish_quit_command() : 0;
      network_sidebar_cli_args_clear(&args);
      return result;
    }
    if (status == NETWORK_SIDEBAR_IPC_COMMAND_DELIVERED) {
      int result = report_unacknowledged_command(args.command);
      network_sidebar_cli_args_clear(&args);
      return result;
    }
  } else if (network_sidebar_command_is_startup(args.command)) {
    status = network_sidebar_send_command(args.command, target_output_name, &error);
    if (error != NULL) {
      g_printerr("nm-sidebar: %s\n", error->message);
      network_sidebar_cli_args_clear(&args);
      return 1;
    }
    if (status == NETWORK_SIDEBAR_IPC_COMMAND_ACKNOWLEDGED) {
      network_sidebar_cli_args_clear(&args);
      return 0;
    }
    if (status == NETWORK_SIDEBAR_IPC_COMMAND_DELIVERED) {
      int result = report_unacknowledged_command(args.command);
      network_sidebar_cli_args_clear(&args);
      return result;
    }
  }

  if (network_sidebar_command_is_ipc_only(args.command)) {
    int result = report_no_running_instance(args.command);
    network_sidebar_cli_args_clear(&args);
    return result;
  }

  if (network_sidebar_command_is_startup(args.command)) {
    int result = run_application_command(argv[0], args.command);
    network_sidebar_cli_args_clear(&args);
    return result;
  }

  network_sidebar_cli_args_clear(&args);
  return 0;
}

static int
report_unacknowledged_command(const char *command)
{
  g_printerr("nm-sidebar: --%s reached a listener but did not receive a nm-sidebar acknowledgement; retry, or remove the socket only if that process is gone\n",
             command);
  return 1;
}

static int
report_no_running_instance(const char *command)
{
  g_printerr("nm-sidebar: no running instance reached for --%s\n", command);
  return 1;
}

static int
report_quit_timeout(void)
{
  g_printerr("nm-sidebar: timed out waiting for --quit to stop the running instance\n");
  return 1;
}

static int
finish_quit_command(void)
{
  gint64 deadline = g_get_monotonic_time() + (gint64) (NETWORK_SIDEBAR_QUIT_WAIT_TIMEOUT_SECONDS * G_USEC_PER_SEC);

  while (TRUE) {
    g_autoptr(GError) error = NULL;
    NetworkSidebarIPCCommandStatus status = network_sidebar_send_command(NETWORK_SIDEBAR_COMMAND_PING, NULL, &error);
    if (error != NULL) {
      g_printerr("nm-sidebar: %s\n", error->message);
      return 1;
    }
    if (status == NETWORK_SIDEBAR_IPC_COMMAND_NOT_DELIVERED)
      return 0;
    if (g_get_monotonic_time() >= deadline)
      return report_quit_timeout();
    g_usleep((gulong) (NETWORK_SIDEBAR_QUIT_WAIT_POLL_SECONDS * G_USEC_PER_SEC));
  }
}

static char *
gui_path_from_argv0(const char *argv0)
{
  g_autofree char *dirname = NULL;
  g_autofree char *candidate = NULL;

  if (argv0 != NULL && strchr(argv0, G_DIR_SEPARATOR) != NULL) {
    dirname = g_path_get_dirname(argv0);
    candidate = g_build_filename(dirname, "nm-sidebar-gui", NULL);
    if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE))
      return g_steal_pointer(&candidate);
  }

  if (g_file_test(NETWORK_SIDEBAR_INSTALLED_GUI_PATH, G_FILE_TEST_IS_EXECUTABLE))
    return g_strdup(NETWORK_SIDEBAR_INSTALLED_GUI_PATH);

  return g_strdup("nm-sidebar-gui");
}

static int
run_application_command(const char *argv0, const char *command)
{
  g_autofree char *gui_path = gui_path_from_argv0(argv0);
  char *const child_argv[] = { gui_path, (char *) command, NULL };

  execv(gui_path, child_argv);

  if (errno == ENOENT && strchr(gui_path, G_DIR_SEPARATOR) == NULL)
    execvp(gui_path, child_argv);

  g_printerr("nm-sidebar: could not start GUI helper %s: %s\n", gui_path, g_strerror(errno));
  return 1;
}
