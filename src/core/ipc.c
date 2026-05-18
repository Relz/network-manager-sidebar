#include "core/ipc.h"

#include "core/ipc_paths.h"
#include "core/target_output.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

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

static void
set_socket_timeout(int fd, double seconds)
{
  struct timeval timeout;

  timeout.tv_sec = (time_t) seconds;
  timeout.tv_usec = (suseconds_t) ((seconds - timeout.tv_sec) * 1000000.0);
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static char *
utf8_strip_copy(const char *value)
{
  const char *start;
  const char *end;

  if (value == NULL)
    return g_strdup("");
  if (!g_utf8_validate(value, -1, NULL))
    return g_strdup("");

  start = value;
  end = value + strlen(value);

  while (start < end) {
    gunichar ch = g_utf8_get_char(start);
    if (!g_unichar_isspace(ch))
      break;
    start = g_utf8_next_char(start);
  }

  while (end > start) {
    const char *previous = g_utf8_prev_char(end);
    gunichar ch = g_utf8_get_char(previous);
    if (!g_unichar_isspace(ch))
      break;
    end = previous;
  }

  return g_strndup(start, end - start);
}

char *
network_sidebar_normalize_command(const char *command)
{
  g_autofree char *trimmed = NULL;

  trimmed = utf8_strip_copy(command);
  return g_utf8_strdown(trimmed, -1);
}

char *
network_sidebar_encode_command(const char *command, const char *target_output_name)
{
  g_autofree char *normalized_command = network_sidebar_normalize_command(command);
  g_autofree char *normalized_output = network_sidebar_normalize_target_output(target_output_name);

  if (normalized_output == NULL)
    return g_strdup_printf("%s\n", normalized_command);
  return g_strdup_printf("%s\t%s\n", normalized_command, normalized_output);
}

gboolean
network_sidebar_parse_command_line(const char *command_line,
                                   char **command,
                                   char **target_output_name)
{
  const char *separator = NULL;

  g_return_val_if_fail(command != NULL, FALSE);
  g_return_val_if_fail(target_output_name != NULL, FALSE);

  *command = NULL;
  *target_output_name = NULL;

  if (command_line == NULL)
    return FALSE;

  separator = strchr(command_line, '\t');
  if (separator == NULL) {
    *command = network_sidebar_normalize_command(command_line);
    return TRUE;
  }

  {
    g_autofree char *raw_command = g_strndup(command_line, separator - command_line);
    *command = network_sidebar_normalize_command(raw_command);
  }
  *target_output_name = network_sidebar_normalize_target_output(separator + 1);
  return TRUE;
}

char *
network_sidebar_success_response(const char *command)
{
  g_autofree char *normalized_command = network_sidebar_normalize_command(command);
  return g_strdup_printf("%s ok %s\n", NETWORK_SIDEBAR_IPC_PROTOCOL, normalized_command);
}

static NetworkSidebarIPCCommandStatus
read_response_status(int fd, const char *command)
{
  g_autofree char *expected = network_sidebar_success_response(command);
  char response[NETWORK_SIDEBAR_IPC_MAX_RESPONSE_BYTES + 1];
  gsize used = 0;

  memset(response, 0, sizeof(response));
  while (used < NETWORK_SIDEBAR_IPC_MAX_RESPONSE_BYTES) {
    ssize_t received = recv(fd, response + used, NETWORK_SIDEBAR_IPC_MAX_RESPONSE_BYTES - used, 0);
    if (received < 0) {
      if (errno == EINTR)
        continue;
      return NETWORK_SIDEBAR_IPC_COMMAND_DELIVERED;
    }
    if (received == 0)
      return NETWORK_SIDEBAR_IPC_COMMAND_DELIVERED;

    if (memchr(response + used, '\n', (gsize) received) != NULL) {
      used += (gsize) received;
      response[used] = '\0';
      if (g_strcmp0(response, expected) == 0)
        return NETWORK_SIDEBAR_IPC_COMMAND_ACKNOWLEDGED;
      return NETWORK_SIDEBAR_IPC_COMMAND_DELIVERED;
    }
    used += (gsize) received;
  }

  return NETWORK_SIDEBAR_IPC_COMMAND_DELIVERED;
}

NetworkSidebarIPCCommandStatus
network_sidebar_send_command(const char *command, const char *target_output_name, GError **error)
{
  g_autofree char *normalized_command = network_sidebar_normalize_command(command);
  g_autofree char *path = NULL;
  g_autofree char *payload = NULL;
  struct sockaddr_un address;
  int fd = -1;
  NetworkSidebarIPCCommandStatus status = NETWORK_SIDEBAR_IPC_COMMAND_NOT_DELIVERED;

  if (*normalized_command == '\0')
    return NETWORK_SIDEBAR_IPC_COMMAND_NOT_DELIVERED;

  path = network_sidebar_runtime_socket_path(error);
  if (path == NULL)
    return NETWORK_SIDEBAR_IPC_COMMAND_NOT_DELIVERED;

  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return NETWORK_SIDEBAR_IPC_COMMAND_NOT_DELIVERED;

  set_socket_timeout(fd, NETWORK_SIDEBAR_IPC_RESPONSE_TIMEOUT_SECONDS);

  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(address.sun_path))
    goto out;
  g_strlcpy(address.sun_path, path, sizeof(address.sun_path));

  if (connect(fd, (struct sockaddr *) &address, sizeof(address)) != 0)
    goto out;

  payload = network_sidebar_encode_command(normalized_command, target_output_name);
  if (!send_all(fd, payload, strlen(payload)))
    goto out;

  status = read_response_status(fd, normalized_command);

out:
  close(fd);
  return status;
}
