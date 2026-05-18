#include "cli/cli_args.h"

#include "core/ipc_commands.h"
#include "core/config.h"

static const char *
command_for_option(const char *option)
{
  if (g_strcmp0(option, "--toggle") == 0)
    return NETWORK_SIDEBAR_COMMAND_TOGGLE;
  if (g_strcmp0(option, "--show") == 0)
    return NETWORK_SIDEBAR_COMMAND_SHOW;
  if (g_strcmp0(option, "--hide") == 0)
    return NETWORK_SIDEBAR_COMMAND_HIDE;
  if (g_strcmp0(option, "--quit") == 0)
    return NETWORK_SIDEBAR_COMMAND_QUIT;
  if (g_strcmp0(option, "--background") == 0)
    return NETWORK_SIDEBAR_COMMAND_BACKGROUND;
  return NULL;
}

static void
print_help(const char *program)
{
  g_print("usage: %s [-h] [--toggle | --show | --hide | --quit |\n", program);
  g_print("                       --background]\n\n");
  g_print("%s for Wayland desktops\n\n", NETWORK_SIDEBAR_APP_NAME);
  g_print("options:\n");
  g_print("  -h, --help    show this help message and exit\n");
  g_print("  --toggle      toggle sidebar\n");
  g_print("  --show        show sidebar\n");
  g_print("  --hide        hide sidebar\n");
  g_print("  --quit        quit the running instance\n");
  g_print("  --background  verify an existing instance or start one without showing the\n");
  g_print("                sidebar\n\n");
  g_print("Running without options defaults to --toggle.\n");
}

static char *
usage_error(const char *program, const char *detail)
{
  return g_strdup_printf("usage: %s [-h] [--toggle | --show | --hide | --quit |\n"
                         "                       --background]\n"
                         "%s: error: %s",
                         program,
                         program,
                         detail);
}

gboolean
network_sidebar_cli_args_parse(int *argc,
                               char ***argv,
                               NetworkSidebarCliArgs *args,
                               GError **error)
{
  g_autofree char *program = NULL;
  const char *selected_option = NULL;

  g_return_val_if_fail(args != NULL, FALSE);
  args->command = NULL;
  args->should_exit = FALSE;
  args->exit_status = 0;

  program = argv != NULL && *argv != NULL && (*argv)[0] != NULL ? g_path_get_basename((*argv)[0]) : g_strdup("nm-sidebar");

  for (int i = 1; argc != NULL && i < *argc; i++) {
    const char *arg = (*argv)[i];
    const char *command;

    if (g_strcmp0(arg, "-h") == 0 || g_strcmp0(arg, "--help") == 0) {
      print_help(program);
      args->should_exit = TRUE;
      args->exit_status = 0;
      return TRUE;
    }

    command = command_for_option(arg);
    if (command == NULL) {
      g_autofree char *detail = g_strdup_printf("unrecognized arguments: %s", arg);
      g_autofree char *message = usage_error(program, detail);
      g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, message);
      return FALSE;
    }
    if (selected_option != NULL) {
      g_autofree char *detail = g_strdup_printf("argument %s: not allowed with argument %s", arg, selected_option);
      g_autofree char *message = usage_error(program, detail);
      g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, message);
      return FALSE;
    }

    selected_option = arg;
    args->command = g_strdup(command);
  }

  if (args->command == NULL)
    args->command = g_strdup(NETWORK_SIDEBAR_DEFAULT_APPLICATION_COMMAND);

  return TRUE;
}

void
network_sidebar_cli_args_clear(NetworkSidebarCliArgs *args)
{
  if (args == NULL)
    return;
  g_clear_pointer(&args->command, g_free);
}
