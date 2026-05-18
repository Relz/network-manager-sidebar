#ifndef NETWORK_SIDEBAR_IPC_COMMANDS_H
#define NETWORK_SIDEBAR_IPC_COMMANDS_H

#include <glib.h>

G_BEGIN_DECLS

#define NETWORK_SIDEBAR_COMMAND_TOGGLE "toggle"
#define NETWORK_SIDEBAR_COMMAND_SHOW "show"
#define NETWORK_SIDEBAR_COMMAND_HIDE "hide"
#define NETWORK_SIDEBAR_COMMAND_QUIT "quit"
#define NETWORK_SIDEBAR_COMMAND_BACKGROUND "background"
#define NETWORK_SIDEBAR_COMMAND_PING "ping"
#define NETWORK_SIDEBAR_DEFAULT_APPLICATION_COMMAND NETWORK_SIDEBAR_COMMAND_TOGGLE

gboolean network_sidebar_command_is_application(const char *command);
gboolean network_sidebar_command_is_startup(const char *command);
gboolean network_sidebar_command_is_ipc_only(const char *command);
gboolean network_sidebar_command_is_socket(const char *command);

G_END_DECLS

#endif
