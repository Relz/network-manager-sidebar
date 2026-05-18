#include "core/ipc_paths.h"

#include "core/config.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

GQuark
network_sidebar_ipc_path_error_quark(void)
{
  return g_quark_from_static_string("nm-sidebar-ipc-path-error");
}

static gboolean
validate_runtime_dir(const char *path, const char *label, GError **error)
{
  struct stat status;
  uid_t uid;
  mode_t mode;

  if (lstat(path, &status) != 0) {
    if (errno == ENOENT) {
      g_set_error(error,
                  NETWORK_SIDEBAR_IPC_PATH_ERROR,
                  NETWORK_SIDEBAR_IPC_PATH_ERROR_FAILED,
                  "%s %s disappeared",
                  label,
                  path);
    } else {
      int saved_errno = errno;
      g_set_error(error,
                  NETWORK_SIDEBAR_IPC_PATH_ERROR,
                  NETWORK_SIDEBAR_IPC_PATH_ERROR_FAILED,
                  "cannot inspect %s %s: %s",
                  label,
                  path,
                  g_strerror(saved_errno));
    }
    return FALSE;
  }

  if (S_ISLNK(status.st_mode)) {
    g_set_error(error,
                NETWORK_SIDEBAR_IPC_PATH_ERROR,
                NETWORK_SIDEBAR_IPC_PATH_ERROR_FAILED,
                "refusing %s %s: path is a symlink",
                label,
                path);
    return FALSE;
  }
  if (!S_ISDIR(status.st_mode)) {
    g_set_error(error,
                NETWORK_SIDEBAR_IPC_PATH_ERROR,
                NETWORK_SIDEBAR_IPC_PATH_ERROR_FAILED,
                "refusing %s %s: path is not a directory",
                label,
                path);
    return FALSE;
  }

  uid = getuid();
  if (status.st_uid != uid) {
    g_set_error(error,
                NETWORK_SIDEBAR_IPC_PATH_ERROR,
                NETWORK_SIDEBAR_IPC_PATH_ERROR_FAILED,
                "refusing %s %s: owned by uid %lu, expected %lu",
                label,
                path,
                (unsigned long) status.st_uid,
                (unsigned long) uid);
    return FALSE;
  }

  mode = status.st_mode & 0777;
  if ((mode & 0077) != 0) {
    g_set_error(error,
                NETWORK_SIDEBAR_IPC_PATH_ERROR,
                NETWORK_SIDEBAR_IPC_PATH_ERROR_FAILED,
                "refusing %s %s: permissions are %04o, expected no group/world access",
                label,
                path,
                mode);
    return FALSE;
  }

  return TRUE;
}

static gboolean
ensure_xdg_runtime_dir(const char *path, GError **error)
{
  if (g_mkdir_with_parents(path, 0700) != 0) {
    int saved_errno = errno;
    g_set_error(error,
                NETWORK_SIDEBAR_IPC_PATH_ERROR,
                NETWORK_SIDEBAR_IPC_PATH_ERROR_FAILED,
                "cannot use XDG_RUNTIME_DIR %s: %s",
                path,
                g_strerror(saved_errno));
    return FALSE;
  }

  return validate_runtime_dir(path, "XDG_RUNTIME_DIR", error);
}

static gboolean
ensure_fallback_runtime_dir(const char *path, GError **error)
{
  gboolean created = FALSE;

  if (mkdir(path, 0700) != 0) {
    if (errno != EEXIST) {
      int saved_errno = errno;
      g_set_error(error,
                  NETWORK_SIDEBAR_IPC_PATH_ERROR,
                  NETWORK_SIDEBAR_IPC_PATH_ERROR_FAILED,
                  "cannot create fallback runtime directory %s: %s",
                  path,
                  g_strerror(saved_errno));
      return FALSE;
    }
  } else {
    created = TRUE;
  }

  if (created && chmod(path, 0700) != 0) {
    int saved_errno = errno;
    g_set_error(error,
                NETWORK_SIDEBAR_IPC_PATH_ERROR,
                NETWORK_SIDEBAR_IPC_PATH_ERROR_FAILED,
                "cannot set secure permissions on fallback runtime directory %s: %s",
                path,
                g_strerror(saved_errno));
    return FALSE;
  }

  return validate_runtime_dir(path, "fallback runtime directory", error);
}

char *
network_sidebar_runtime_socket_path(GError **error)
{
  const char *runtime_dir = g_getenv("XDG_RUNTIME_DIR");
  g_autofree char *runtime_path = NULL;

  if (runtime_dir != NULL && *runtime_dir != '\0') {
    if (!g_path_is_absolute(runtime_dir)) {
      g_set_error(error,
                  NETWORK_SIDEBAR_IPC_PATH_ERROR,
                  NETWORK_SIDEBAR_IPC_PATH_ERROR_FAILED,
                  "refusing XDG_RUNTIME_DIR %s: path must be absolute",
                  runtime_dir);
      return NULL;
    }
    runtime_path = g_strdup(runtime_dir);
    if (!ensure_xdg_runtime_dir(runtime_path, error))
      return NULL;
  } else {
    runtime_path = g_strdup_printf("/tmp/nm-sidebar-%lu", (unsigned long) getuid());
    if (!ensure_fallback_runtime_dir(runtime_path, error))
      return NULL;
  }

  return g_build_filename(runtime_path, NETWORK_SIDEBAR_SOCKET_NAME, NULL);
}
