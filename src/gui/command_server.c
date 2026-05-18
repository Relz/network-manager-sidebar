#include "gui/command_server.h"

#include "core/command_socket.h"
#include "core/ipc_commands.h"
#include "core/ipc_paths.h"
#include "gui/app.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define NETWORK_SIDEBAR_STARTUP_LOCK_TIMEOUT_SECONDS 3.0
#define NETWORK_SIDEBAR_STARTUP_LOCK_RETRY_SECONDS 0.05
#define NETWORK_SIDEBAR_ACCEPT_TIMEOUT_SECONDS 0.25

struct _NetworkSidebarCommandServer {
  NetworkSidebarGuiApp *app;
  char *path;
  char *startup_lock_path;
  int fd;
  int startup_lock_fd;
  dev_t bound_dev;
  ino_t bound_ino;
  gboolean has_bound_socket_id;
  gboolean stop;
  GThread *thread;
};

typedef struct {
  NetworkSidebarGuiApp *app;
  char *command;
  char *target_output_name;
} DispatchCommand;

static gpointer command_server_thread(gpointer user_data);
static gboolean dispatch_command_cb(gpointer user_data);
static gboolean bind_command_socket(NetworkSidebarCommandServer *server, GError **error);
static void release_startup_lock(NetworkSidebarCommandServer *server);
static void unlink_bound_socket_path(NetworkSidebarCommandServer *server);

static void
set_accept_timeout(int fd)
{
  struct timeval timeout = { 0, 250000 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

NetworkSidebarCommandServer *
network_sidebar_command_server_new(NetworkSidebarGuiApp *app, GError **error)
{
  NetworkSidebarCommandServer *server = g_new0(NetworkSidebarCommandServer, 1);

  server->app = app;
  server->fd = -1;
  server->startup_lock_fd = -1;
  server->path = network_sidebar_runtime_socket_path(error);
  if (server->path == NULL)
    goto fail;
  server->startup_lock_path = g_strdup_printf("%s.startup.lock", server->path);

  server->fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server->fd < 0) {
    int saved_errno = errno;
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_errno),
                "could not create command socket: %s. Check system socket limits.",
                g_strerror(saved_errno));
    goto fail;
  }

  if (!bind_command_socket(server, error))
    goto fail;

  server->thread = g_thread_new("nm-sidebar-command-server", command_server_thread, server);
  return server;

fail:
  if (server != NULL)
    network_sidebar_command_server_close(server);
  return NULL;
}

static gboolean
validate_socket_directory(NetworkSidebarCommandServer *server, GError **error)
{
  g_autofree char *directory = g_path_get_dirname(server->path);
  struct stat directory_stat;
  mode_t permissions;

  if (lstat(directory, &directory_stat) != 0) {
    int saved_errno = errno;
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_errno),
                "could not inspect command socket directory %s: %s. Set XDG_RUNTIME_DIR to a writable private directory.",
                directory,
                g_strerror(saved_errno));
    return FALSE;
  }
  if (S_ISLNK(directory_stat.st_mode)) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "refusing to use command socket directory %s because it is a symlink. Remove it or set XDG_RUNTIME_DIR to a private runtime directory.",
                directory);
    return FALSE;
  }
  if (!S_ISDIR(directory_stat.st_mode)) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "command socket parent %s is not a directory. Remove the conflicting path or set XDG_RUNTIME_DIR to a writable private directory.",
                directory);
    return FALSE;
  }
  if (directory_stat.st_uid != getuid()) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "refusing to use command socket directory %s because it is not owned by this user. Fix ownership or set XDG_RUNTIME_DIR to a private runtime directory.",
                directory);
    return FALSE;
  }
  permissions = directory_stat.st_mode & 0777;
  if ((permissions & 0077) != 0) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "refusing to use command socket directory %s with permissions %03o. Run 'chmod 700 %s' or set XDG_RUNTIME_DIR to a private runtime directory.",
                directory,
                permissions,
                directory);
    return FALSE;
  }
  return TRUE;
}

static gboolean
wait_for_startup_lock(int fd, const char *socket_path, GError **error)
{
  gint64 deadline = g_get_monotonic_time() + (gint64) (NETWORK_SIDEBAR_STARTUP_LOCK_TIMEOUT_SECONDS * G_USEC_PER_SEC);

  while (TRUE) {
    if (flock(fd, LOCK_EX | LOCK_NB) == 0)
      return TRUE;
    if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EACCES) {
      int saved_errno = errno;
      g_set_error(error,
                  G_IO_ERROR,
                  g_io_error_from_errno(saved_errno),
                  "could not lock command socket startup lock: %s. Check runtime directory permissions and system lock limits.",
                  g_strerror(saved_errno));
      return FALSE;
    }
    if (g_get_monotonic_time() >= deadline) {
      g_set_error(error,
                  G_IO_ERROR,
                  G_IO_ERROR_BUSY,
                  "another nm-sidebar process is still preparing command socket %s. Retry after it finishes, or stop the stuck nm-sidebar process.",
                  socket_path);
      return FALSE;
    }
    g_usleep((gulong) (NETWORK_SIDEBAR_STARTUP_LOCK_RETRY_SECONDS * G_USEC_PER_SEC));
  }
}

static gboolean
acquire_startup_lock(NetworkSidebarCommandServer *server, GError **error)
{
  int flags = O_RDWR | O_CREAT | O_CLOEXEC;
  struct stat lock_stat;

#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif

  server->startup_lock_fd = open(server->startup_lock_path, flags, 0600);
  if (server->startup_lock_fd < 0) {
    int saved_errno = errno;
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_errno),
                "could not open command socket startup lock %s: %s. Remove conflicting paths or check runtime directory permissions.",
                server->startup_lock_path,
                g_strerror(saved_errno));
    return FALSE;
  }

  if (fstat(server->startup_lock_fd, &lock_stat) != 0) {
    int saved_errno = errno;
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_errno),
                "could not inspect command socket startup lock %s: %s.",
                server->startup_lock_path,
                g_strerror(saved_errno));
    return FALSE;
  }
  if (!S_ISREG(lock_stat.st_mode)) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "command socket startup lock %s is a %s. Remove it or set XDG_RUNTIME_DIR to a different private runtime directory.",
                server->startup_lock_path,
                network_sidebar_file_type_name(lock_stat.st_mode));
    return FALSE;
  }
  if (lock_stat.st_uid != getuid()) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "refusing command socket startup lock %s because it is not owned by this user. Fix ownership or set XDG_RUNTIME_DIR to a private runtime directory.",
                server->startup_lock_path);
    return FALSE;
  }

  return wait_for_startup_lock(server->startup_lock_fd, server->path, error);
}

static gboolean
socket_path_is_unchanged(const char *path, const struct stat *existing_stat, gboolean *disappeared, GError **error)
{
  struct stat current_stat;

  if (disappeared != NULL)
    *disappeared = FALSE;
  if (lstat(path, &current_stat) != 0) {
    if (errno == ENOENT) {
      if (disappeared != NULL)
        *disappeared = TRUE;
      return FALSE;
    }
    int saved_errno = errno;
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_errno),
                "could not inspect command socket path %s: %s. Remove it manually or set XDG_RUNTIME_DIR to a writable private directory.",
                path,
                g_strerror(saved_errno));
    return FALSE;
  }
  if (!S_ISSOCK(current_stat.st_mode) || current_stat.st_dev != existing_stat->st_dev || current_stat.st_ino != existing_stat->st_ino) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "command socket path %s changed while startup was preparing it. Try again or remove the conflicting path manually.",
                path);
    return FALSE;
  }
  return TRUE;
}

static gboolean
remove_stale_socket_path(NetworkSidebarCommandServer *server, GError **error)
{
  struct stat existing_stat;
  NetworkSidebarSocketProbeStatus probe_status;
  gboolean disappeared = FALSE;

  if (lstat(server->path, &existing_stat) != 0) {
    if (errno == ENOENT)
      return TRUE;
    int saved_errno = errno;
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_errno),
                "could not inspect command socket path %s: %s. Remove the path or set XDG_RUNTIME_DIR to a writable private directory.",
                server->path,
                g_strerror(saved_errno));
    return FALSE;
  }

  if (!S_ISSOCK(existing_stat.st_mode)) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "%s is a %s and blocks the command socket. Remove it or set XDG_RUNTIME_DIR to a different private runtime directory.",
                server->path,
                network_sidebar_file_type_name(existing_stat.st_mode));
    return FALSE;
  }

  probe_status = network_sidebar_probe_socket(server->path);
  if (probe_status == NETWORK_SIDEBAR_SOCKET_PROBE_ACKNOWLEDGED) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_EXISTS,
                "another process is already listening on command socket %s. Use --quit to stop the running instance or remove the socket if that process is gone.",
                server->path);
    return FALSE;
  }
  if (probe_status == NETWORK_SIDEBAR_SOCKET_PROBE_UNACKNOWLEDGED) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_BUSY,
                "command socket %s exists but did not acknowledge a ping. A running instance may be busy or a foreign listener may be using the path; retry, or remove the socket only if that process is gone.",
                server->path);
    return FALSE;
  }

  if (!socket_path_is_unchanged(server->path, &existing_stat, &disappeared, error))
    return disappeared;

  if (unlink(server->path) != 0 && errno != ENOENT) {
    int saved_errno = errno;
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_errno),
                "command socket %s exists but is not accepting commands and could not be removed: %s. Remove it only if no nm-sidebar process is running, or set XDG_RUNTIME_DIR to a different private runtime directory.",
                server->path,
                g_strerror(saved_errno));
    return FALSE;
  }
  return TRUE;
}

static gboolean
bind_command_socket(NetworkSidebarCommandServer *server, GError **error)
{
  struct sockaddr_un address;
  struct stat bound_stat;

  if (!validate_socket_directory(server, error))
    return FALSE;
  if (!acquire_startup_lock(server, error))
    return FALSE;
  if (!remove_stale_socket_path(server, error))
    return FALSE;

  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (strlen(server->path) >= sizeof(address.sun_path)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FILENAME_TOO_LONG, "command socket path is too long: %s", server->path);
    return FALSE;
  }
  g_strlcpy(address.sun_path, server->path, sizeof(address.sun_path));

  if (bind(server->fd, (struct sockaddr *) &address, sizeof(address)) != 0) {
    int saved_errno = errno;
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_errno),
                "could not bind command socket at %s: %s. Check that the runtime directory is writable and the socket path is not blocked.",
                server->path,
                g_strerror(saved_errno));
    return FALSE;
  }
  if (lstat(server->path, &bound_stat) == 0) {
    server->bound_dev = bound_stat.st_dev;
    server->bound_ino = bound_stat.st_ino;
    server->has_bound_socket_id = TRUE;
  }
  if (chmod(server->path, 0600) != 0) {
    int saved_errno = errno;
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_errno),
                "could not set permissions on command socket %s: %s. Check ownership and permissions on the runtime directory.",
                server->path,
                g_strerror(saved_errno));
    return FALSE;
  }
  if (listen(server->fd, 4) != 0) {
    int saved_errno = errno;
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_errno),
                "could not listen on command socket %s: %s. Remove stale sockets or set XDG_RUNTIME_DIR to a writable private directory.",
                server->path,
                g_strerror(saved_errno));
    return FALSE;
  }

  set_accept_timeout(server->fd);
  release_startup_lock(server);
  return TRUE;
}

static gboolean
path_matches_bound_socket(NetworkSidebarCommandServer *server)
{
  struct stat current_stat;

  if (!server->has_bound_socket_id)
    return FALSE;
  if (lstat(server->path, &current_stat) != 0)
    return FALSE;
  if (!S_ISSOCK(current_stat.st_mode))
    return FALSE;
  return current_stat.st_dev == server->bound_dev && current_stat.st_ino == server->bound_ino;
}

gboolean
network_sidebar_command_server_is_healthy(NetworkSidebarCommandServer *server)
{
  if (server == NULL)
    return FALSE;
  if (!path_matches_bound_socket(server))
    return FALSE;
  if (!network_sidebar_socket_acknowledges_ping(server->path))
    return FALSE;
  return path_matches_bound_socket(server);
}

static gpointer
command_server_thread(gpointer user_data)
{
  NetworkSidebarCommandServer *server = user_data;

  while (!server->stop) {
    int conn = accept(server->fd, NULL, NULL);
    if (conn < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        continue;
      break;
    }

    g_autofree char *command = NULL;
    g_autofree char *target_output_name = NULL;
    if (network_sidebar_read_command(conn, &command, &target_output_name)) {
      if (g_strcmp0(command, NETWORK_SIDEBAR_COMMAND_PING) == 0) {
        network_sidebar_send_success_response(conn, command);
      } else {
        DispatchCommand *dispatch = g_new0(DispatchCommand, 1);
        guint source_id;
        dispatch->app = server->app;
        dispatch->command = g_steal_pointer(&command);
        dispatch->target_output_name = g_steal_pointer(&target_output_name);
        source_id = g_idle_add(dispatch_command_cb, dispatch);
        if (source_id != 0)
          network_sidebar_send_success_response(conn, dispatch->command);
        else {
          g_free(dispatch->command);
          g_free(dispatch->target_output_name);
          g_free(dispatch);
        }
      }
    }
    close(conn);
  }
  return NULL;
}

static gboolean
dispatch_command_cb(gpointer user_data)
{
  DispatchCommand *dispatch = user_data;
  network_sidebar_gui_app_handle_command(dispatch->app, dispatch->command, dispatch->target_output_name);
  g_free(dispatch->command);
  g_free(dispatch->target_output_name);
  g_free(dispatch);
  return G_SOURCE_REMOVE;
}

static void
release_startup_lock(NetworkSidebarCommandServer *server)
{
  if (server->startup_lock_fd < 0)
    return;
  flock(server->startup_lock_fd, LOCK_UN);
  close(server->startup_lock_fd);
  server->startup_lock_fd = -1;
}

static void
unlink_bound_socket_path(NetworkSidebarCommandServer *server)
{
  gboolean acquired_lock = FALSE;

  if (server == NULL || !server->has_bound_socket_id)
    return;
  if (server->startup_lock_fd < 0) {
    if (!acquire_startup_lock(server, NULL))
      return;
    acquired_lock = TRUE;
  }
  if (path_matches_bound_socket(server))
    unlink(server->path);
  if (acquired_lock)
    release_startup_lock(server);
}

void
network_sidebar_command_server_close(NetworkSidebarCommandServer *server)
{
  if (server == NULL)
    return;

  server->stop = TRUE;
  if (server->fd >= 0) {
    shutdown(server->fd, SHUT_RDWR);
    close(server->fd);
    server->fd = -1;
  }
  if (server->thread != NULL) {
    g_thread_join(server->thread);
    server->thread = NULL;
  }
  unlink_bound_socket_path(server);
  release_startup_lock(server);
  g_free(server->path);
  g_free(server->startup_lock_path);
  g_free(server);
}
