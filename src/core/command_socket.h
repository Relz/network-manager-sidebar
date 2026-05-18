#ifndef NETWORK_SIDEBAR_COMMAND_SOCKET_H
#define NETWORK_SIDEBAR_COMMAND_SOCKET_H

#include <glib.h>
#include <sys/types.h>

G_BEGIN_DECLS

#define NETWORK_SIDEBAR_SOCKET_PROBE_TIMEOUT_SECONDS 0.5
#define NETWORK_SIDEBAR_COMMAND_READ_TIMEOUT_SECONDS 0.25
#define NETWORK_SIDEBAR_COMMAND_MAX_BYTES 192

typedef enum {
  NETWORK_SIDEBAR_SOCKET_PROBE_ACKNOWLEDGED,
  NETWORK_SIDEBAR_SOCKET_PROBE_UNACKNOWLEDGED,
  NETWORK_SIDEBAR_SOCKET_PROBE_NOT_CONNECTABLE,
} NetworkSidebarSocketProbeStatus;

NetworkSidebarSocketProbeStatus network_sidebar_probe_socket(const char *path);
gboolean network_sidebar_socket_acknowledges_ping(const char *path);
void network_sidebar_send_success_response(int fd, const char *command);
gboolean network_sidebar_read_command(int fd, char **command, char **target_output_name);
const char *network_sidebar_file_type_name(mode_t mode);

G_END_DECLS

#endif
