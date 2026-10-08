#define _GNU_SOURCE

#include "helper/amneziawg_executable.h"

#include "amneziawg_build_config.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static gboolean
configured_store_path(const char *path)
{
  g_auto(GStrv) roots = g_strsplit(AWG_STORE_PATHS, ":", -1);

  for (guint i = 0; roots[i] != NULL; i++) {
    gsize length = strlen(roots[i]);

    if (length > 0 && strncmp(path, roots[i], length) == 0 && path[length] == '/')
      return TRUE;
  }
  return FALSE;
}

static gboolean
trusted_directory(int fd, gboolean store_directory)
{
  struct stat status;

  if (fstat(fd, &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != 0)
    return FALSE;
  /* Multi-user Nix may give nixbld group write access to /nix/store itself.
   * The sticky bit protects existing root-owned entries. All directories
   * beneath the selected output still require root:root and no write access. */
  if (store_directory && (status.st_mode & S_ISVTX) != 0 &&
      (status.st_mode & 0002) == 0)
    return TRUE;
  return status.st_gid == 0 && (status.st_mode & 0022) == 0;
}

int
awg_executable_open(const char *path, gboolean resolve_symlinks)
{
  g_autofree char *canonical = NULL;
  g_auto(GStrv) components = NULL;
  struct stat status;
  gboolean store_path;
  int directory = -1;
  int executable = -1;

  if (path == NULL || path[0] != '/' || lstat(path, &status) != 0 ||
      status.st_uid != 0 || status.st_gid != 0 ||
      (!resolve_symlinks && S_ISLNK(status.st_mode)))
    return -1;
  canonical = realpath(path, NULL);
  if (canonical == NULL)
    return -1;
  store_path = configured_store_path(canonical);
  components = g_strsplit(canonical + 1, "/", -1);
  directory = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory < 0 || !trusted_directory(directory, FALSE))
    goto out;
  for (guint i = 0; components[i] != NULL; i++) {
    int next;
    gboolean store_directory = store_path && i == 1 &&
      strcmp(components[0], "nix") == 0 && strcmp(components[1], "store") == 0;

    if (components[i + 1] == NULL) {
      executable = openat(directory, components[i],
                          O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
      break;
    }
    next = openat(directory, components[i],
                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(directory);
    directory = next;
    if (directory < 0 || !trusted_directory(directory, store_directory))
      goto out;
    /* Store output contents are immutable, including optimized hard links. */
    if (store_path && i >= 2 &&
        (fstat(directory, &status) != 0 || (status.st_mode & 0222) != 0))
      goto out;
  }
  if (executable >= 0 &&
      (fstat(executable, &status) != 0 || !S_ISREG(status.st_mode) ||
       status.st_uid != 0 || status.st_gid != 0 ||
       (!store_path && status.st_nlink != 1) || (status.st_mode & 0111) == 0 ||
       (status.st_mode & (store_path ? 0222 : 0022)) != 0)) {
    close(executable);
    executable = -1;
  }
out:
  if (directory >= 0)
    close(directory);
  return executable;
}
