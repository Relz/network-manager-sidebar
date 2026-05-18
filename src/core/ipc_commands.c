#include "core/ipc_commands.h"

static gboolean
command_equals(const char *command, const char *expected)
{
  return g_strcmp0(command, expected) == 0;
}

gboolean
network_sidebar_command_is_application(const char *command)
{
  return command_equals(command, NETWORK_SIDEBAR_COMMAND_TOGGLE) ||
         command_equals(command, NETWORK_SIDEBAR_COMMAND_SHOW) ||
         command_equals(command, NETWORK_SIDEBAR_COMMAND_HIDE) ||
         command_equals(command, NETWORK_SIDEBAR_COMMAND_QUIT) ||
         command_equals(command, NETWORK_SIDEBAR_COMMAND_BACKGROUND);
}

gboolean
network_sidebar_command_is_startup(const char *command)
{
  return command_equals(command, NETWORK_SIDEBAR_COMMAND_TOGGLE) ||
         command_equals(command, NETWORK_SIDEBAR_COMMAND_SHOW) ||
         command_equals(command, NETWORK_SIDEBAR_COMMAND_BACKGROUND);
}

gboolean
network_sidebar_command_is_ipc_only(const char *command)
{
  return command_equals(command, NETWORK_SIDEBAR_COMMAND_HIDE) ||
         command_equals(command, NETWORK_SIDEBAR_COMMAND_QUIT);
}

gboolean
network_sidebar_command_is_socket(const char *command)
{
  return command_equals(command, NETWORK_SIDEBAR_COMMAND_TOGGLE) ||
         command_equals(command, NETWORK_SIDEBAR_COMMAND_SHOW) ||
         command_equals(command, NETWORK_SIDEBAR_COMMAND_HIDE) ||
         command_equals(command, NETWORK_SIDEBAR_COMMAND_QUIT) ||
         command_equals(command, NETWORK_SIDEBAR_COMMAND_PING);
}
