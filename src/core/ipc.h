#ifndef NETWORK_SIDEBAR_IPC_H
#define NETWORK_SIDEBAR_IPC_H

#include <glib.h>

G_BEGIN_DECLS

#define NETWORK_SIDEBAR_IPC_PROTOCOL "nm-sidebar-ipc-v1"
#define NETWORK_SIDEBAR_IPC_RESPONSE_TIMEOUT_SECONDS 1.0
#define NETWORK_SIDEBAR_IPC_MAX_RESPONSE_BYTES 128

typedef enum {
  NETWORK_SIDEBAR_IPC_COMMAND_NOT_DELIVERED,
  NETWORK_SIDEBAR_IPC_COMMAND_DELIVERED,
  NETWORK_SIDEBAR_IPC_COMMAND_ACKNOWLEDGED,
} NetworkSidebarIPCCommandStatus;

char *network_sidebar_normalize_command(const char *command);
char *network_sidebar_encode_command(const char *command, const char *target_output_name);
gboolean network_sidebar_parse_command_line(const char *command_line,
                                            char **command,
                                            char **target_output_name);
char *network_sidebar_success_response(const char *command);
NetworkSidebarIPCCommandStatus network_sidebar_send_command(const char *command,
                                                            const char *target_output_name,
                                                            GError **error);

G_END_DECLS

#endif
