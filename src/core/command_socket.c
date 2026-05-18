#include "core/command_socket.h"

#include "core/ipc.h"
#include "core/ipc_commands.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static void
set_socket_timeout(int fd, double seconds)
{
  struct timeval timeout;

  timeout.tv_sec = (time_t) seconds;
  timeout.tv_usec = (suseconds_t) ((seconds - timeout.tv_sec) * 1000000.0);
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static gboolean
send_all(int fd, const char *data, gsize length)
{
  while (length > 0) {
    ssize_t written = send(fd, data, length, MSG_NOSIGNAL);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      return FALSE;
    data += written;
    length -= (gsize) written;
  }
  return TRUE;
}

NetworkSidebarSocketProbeStatus
network_sidebar_probe_socket(const char *path)
{
  g_autofree char *expected = network_sidebar_success_response(NETWORK_SIDEBAR_COMMAND_PING);
  char response[128];
  gsize used = 0;
  struct sockaddr_un address;
  int fd = -1;
  NetworkSidebarSocketProbeStatus status = NETWORK_SIDEBAR_SOCKET_PROBE_UNACKNOWLEDGED;

  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return NETWORK_SIDEBAR_SOCKET_PROBE_UNACKNOWLEDGED;

  set_socket_timeout(fd, NETWORK_SIDEBAR_SOCKET_PROBE_TIMEOUT_SECONDS);

  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(address.sun_path))
    goto out;
  g_strlcpy(address.sun_path, path, sizeof(address.sun_path));

  if (connect(fd, (struct sockaddr *) &address, sizeof(address)) != 0) {
    if (errno == ECONNREFUSED || errno == ENOENT)
      status = NETWORK_SIDEBAR_SOCKET_PROBE_NOT_CONNECTABLE;
    goto out;
  }

  if (!send_all(fd, "ping\n", 5))
    goto out;

  memset(response, 0, sizeof(response));
  while (used < strlen(expected)) {
    ssize_t received = recv(fd, response + used, strlen(expected) - used, 0);
    if (received < 0) {
      if (errno == EINTR)
        continue;
      goto out;
    }
    if (received == 0)
      goto out;
    if (memchr(response + used, '\n', (gsize) received) != NULL) {
      used += (gsize) received;
      break;
    }
    used += (gsize) received;
  }

  response[MIN(used, sizeof(response) - 1)] = '\0';
  if (g_strcmp0(response, expected) == 0)
    status = NETWORK_SIDEBAR_SOCKET_PROBE_ACKNOWLEDGED;

out:
  close(fd);
  return status;
}

gboolean
network_sidebar_socket_acknowledges_ping(const char *path)
{
  return network_sidebar_probe_socket(path) == NETWORK_SIDEBAR_SOCKET_PROBE_ACKNOWLEDGED;
}

void
network_sidebar_send_success_response(int fd, const char *command)
{
  g_autofree char *response = network_sidebar_success_response(command);
  send_all(fd, response, strlen(response));
}

gboolean
network_sidebar_read_command(int fd, char **command, char **target_output_name)
{
  char data[NETWORK_SIDEBAR_COMMAND_MAX_BYTES + 1];
  gsize used = 0;
  char *newline = NULL;

  g_return_val_if_fail(command != NULL, FALSE);
  g_return_val_if_fail(target_output_name != NULL, FALSE);

  *command = NULL;
  *target_output_name = NULL;
  memset(data, 0, sizeof(data));
  set_socket_timeout(fd, NETWORK_SIDEBAR_COMMAND_READ_TIMEOUT_SECONDS);

  while (used < NETWORK_SIDEBAR_COMMAND_MAX_BYTES) {
    ssize_t received = recv(fd, data + used, NETWORK_SIDEBAR_COMMAND_MAX_BYTES - used, 0);
    if (received < 0) {
      if (errno == EINTR)
        continue;
      return FALSE;
    }
    if (received == 0)
      return FALSE;
    newline = memchr(data + used, '\n', (gsize) received);
    used += (gsize) received;
    if (newline != NULL)
      break;
  }

  if (newline == NULL)
    return FALSE;
  *newline = '\0';

  if (!g_utf8_validate(data, -1, NULL))
    return FALSE;
  if (!network_sidebar_parse_command_line(data, command, target_output_name))
    return FALSE;
  if (!network_sidebar_command_is_socket(*command)) {
    g_clear_pointer(command, g_free);
    g_clear_pointer(target_output_name, g_free);
    return FALSE;
  }

  return TRUE;
}

const char *
network_sidebar_file_type_name(mode_t mode)
{
  if (S_ISDIR(mode))
    return "directory";
  if (S_ISLNK(mode))
    return "symlink";
  if (S_ISREG(mode))
    return "regular file";
  return "filesystem object";
}
