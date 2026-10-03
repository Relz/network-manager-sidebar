#define _GNU_SOURCE

#include "helper/amneziawg_helper_storage.h"

#include "helper/amneziawg_helper_util.h"
#include "helper/amneziawg_process.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <unistd.h>

#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1 << 1)
#endif
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

static const char temp_config_alphabet[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";

struct _AwgHelperStorage {
  int config;
  int staging;
  int runtime;
  int runtime_configs;
  int lock;
};

struct _AwgHelperStorageIterator {
  DIR *directory;
};

static gboolean
directory_is_trusted(int fd)
{
  struct stat status;

  return fstat(fd, &status) == 0 &&
         S_ISDIR(status.st_mode) &&
         status.st_uid == 0 &&
         status.st_gid == 0 &&
         (status.st_mode & 0022) == 0;
}

static int
open_existing_dir_at(int parent, const char *name)
{
  int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

  if (fd < 0)
    return -1;
  if (!directory_is_trusted(fd)) {
    close(fd);
    errno = EPERM;
    return -1;
  }
  return fd;
}

static int
ensure_dir_at(int parent, const char *name, mode_t mode, mode_t required_access)
{
  gboolean created = FALSE;
  struct stat status;
  int fd;

  if (mkdirat(parent, name, mode) == 0) {
    created = TRUE;
  } else if (errno != EEXIST) {
    return -1;
  }

  fd = open_existing_dir_at(parent, name);
  if (fd < 0)
    return -1;
  if (fstat(fd, &status) != 0 ||
      (!created && (status.st_mode & required_access) != required_access)) {
    close(fd);
    errno = EPERM;
    return -1;
  }
  if (created && fchmod(fd, mode) != 0) {
    close(fd);
    return -1;
  }
  if (created && fsync(parent) != 0) {
    close(fd);
    return -1;
  }
  if (fsync(fd) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static gboolean
lock_helper_until(int fd, gint64 deadline)
{
  for (;;) {
    NetworkSidebarAmneziaWGHelperExit stop = awg_helper_phase_stop_status(deadline);

    if (stop != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
      errno = stop == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_TIMED_OUT ? ETIMEDOUT : EINTR;
      return FALSE;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) == 0)
      return TRUE;
    if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
      return FALSE;
    if (network_sidebar_amneziawg_process_poll(NULL, 0, 25) < 0 && errno != EINTR)
      return FALSE;
  }
}

static int
open_helper_lock(int runtime, gint64 deadline)
{
  int flags = O_RDWR | O_NOFOLLOW | O_CLOEXEC;
  int fd = openat(runtime, ".helper.lock", flags | O_CREAT | O_EXCL, 0600);
  gboolean created = fd >= 0;
  struct stat status;
  int saved_errno;

  if (fd < 0 && errno == EEXIST)
    fd = openat(runtime, ".helper.lock", flags);
  if (fd < 0)
    return -1;
  if (fstat(fd, &status) != 0)
    goto fail;
  if (!S_ISREG(status.st_mode) || status.st_uid != 0 || status.st_gid != 0 ||
      status.st_nlink != 1 || (!created && (status.st_mode & 07777) != 0600)) {
    errno = EPERM;
    goto fail;
  }
  if ((created && fchmod(fd, 0600) != 0) || !lock_helper_until(fd, deadline))
    goto fail;
  return fd;

fail:
  saved_errno = errno;
  close(fd);
  errno = saved_errno;
  return -1;
}

AwgHelperStorage *
awg_helper_storage_open_runtime(gint64 deadline)
{
  AwgHelperStorage *storage = g_new0(AwgHelperStorage, 1);
  int root = -1;
  int run = -1;
  int run_sidebar = -1;
  int saved_errno;

  *storage = (AwgHelperStorage) { -1, -1, -1, -1, -1 };
  root = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (root < 0 || !directory_is_trusted(root))
    goto fail;

  run = open_existing_dir_at(root, "run");
  if (run < 0)
    goto fail;
  run_sidebar = ensure_dir_at(run, "nm-sidebar", 0755, 0055);
  if (run_sidebar < 0)
    goto fail;
  storage->runtime = ensure_dir_at(run_sidebar, "amneziawg", 0755, 0055);
  if (storage->runtime < 0)
    goto fail;
  storage->runtime_configs = ensure_dir_at(storage->runtime,
                                           ".configs",
                                           0700,
                                           0700);
  if (storage->runtime_configs < 0)
    goto fail;
  {
    struct stat status;

    if (fstat(storage->runtime_configs, &status) != 0 ||
        (status.st_mode & 0777) != 0700)
      goto fail;
  }

  storage->lock = open_helper_lock(storage->runtime, deadline);
  if (storage->lock < 0)
    goto fail;

  awg_helper_close_fd(&run_sidebar);
  awg_helper_close_fd(&run);
  awg_helper_close_fd(&root);
  return storage;

fail:
  saved_errno = errno;
  awg_helper_close_fd(&run_sidebar);
  awg_helper_close_fd(&run);
  awg_helper_close_fd(&root);
  awg_helper_storage_free(storage);
  errno = saved_errno;
  return NULL;
}

gboolean
awg_helper_storage_open_profiles(AwgHelperStorage *storage)
{
  int root = -1;
  int etc = -1;
  int amnezia = -1;
  int config = -1;
  int staging = -1;
  int saved_errno;
  struct stat status;

  if (storage == NULL || storage->lock < 0) {
    errno = EINVAL;
    return FALSE;
  }
  if (storage->config >= 0 && storage->staging >= 0)
    return TRUE;

  root = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (root < 0 || !directory_is_trusted(root))
    goto fail;
  etc = open_existing_dir_at(root, "etc");
  if (etc < 0)
    goto fail;
  amnezia = ensure_dir_at(etc, "amnezia", 0755, 0);
  if (amnezia < 0)
    goto fail;
  config = ensure_dir_at(amnezia, "amneziawg", 0700, 0);
  if (config < 0)
    goto fail;
  staging = ensure_dir_at(config, ".staging", 0700, 0700);
  if (staging < 0 || fstat(staging, &status) != 0)
    goto fail;
  if ((status.st_mode & 0777) != 0700) {
    errno = EPERM;
    goto fail;
  }

  storage->config = config;
  storage->staging = staging;
  awg_helper_close_fd(&amnezia);
  awg_helper_close_fd(&etc);
  awg_helper_close_fd(&root);
  return TRUE;

fail:
  saved_errno = errno;
  awg_helper_close_fd(&staging);
  awg_helper_close_fd(&config);
  awg_helper_close_fd(&amnezia);
  awg_helper_close_fd(&etc);
  awg_helper_close_fd(&root);
  errno = saved_errno;
  return FALSE;
}

void
awg_helper_storage_free(AwgHelperStorage *storage)
{
  if (storage == NULL)
    return;
  awg_helper_close_fd(&storage->runtime_configs);
  awg_helper_close_fd(&storage->runtime);
  awg_helper_close_fd(&storage->staging);
  awg_helper_close_fd(&storage->config);
  /* Keep the lock inode in place so every helper locks the same object. */
  awg_helper_close_fd(&storage->lock);
  g_free(storage);
}

gboolean
awg_helper_storage_make_config_filename(const char *name,
                                        char *filename,
                                        gsize size)
{
  int written = snprintf(filename, size, "%s.conf", name);

  return written > 0 && (gsize) written < size;
}

gboolean
awg_helper_storage_make_runtime_config_path(const char *name,
                                            char *path,
                                            gsize size)
{
  int written = snprintf(path,
                         size,
                         "%s/%s.conf",
                         NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_CONFIG_DIR,
                         name);

  return written > 0 && (gsize) written < size;
}

gboolean
awg_helper_storage_make_dns_filename(const char *name,
                                     char *filename,
                                     gsize size)
{
  int written = snprintf(filename, size, "%s.dns", name);

  return written > 0 && (gsize) written < size;
}

static int
storage_area_fd(const AwgHelperStorage *storage, AwgHelperStorageArea area)
{
  switch (area) {
  case AWG_HELPER_STORAGE_CONFIG:
    return storage->config;
  case AWG_HELPER_STORAGE_STAGING:
    return storage->staging;
  case AWG_HELPER_STORAGE_RUNTIME:
    return storage->runtime;
  case AWG_HELPER_STORAGE_RUNTIME_CONFIGS:
    return storage->runtime_configs;
  default:
    return -1;
  }
}

static AwgHelperEntryState
secure_file_state_at(int dir, const char *name, mode_t mode)
{
  struct stat before;
  struct stat after;
  int fd;

  if (fstatat(dir, name, &before, AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT ? AWG_HELPER_ENTRY_MISSING :
                            AWG_HELPER_ENTRY_INVALID;
  if (!S_ISREG(before.st_mode) || before.st_uid != 0 || before.st_gid != 0 ||
      before.st_nlink != 1 || (before.st_mode & 0777) != mode)
    return AWG_HELPER_ENTRY_INVALID;

  fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return AWG_HELPER_ENTRY_INVALID;
  if (fstat(fd, &after) != 0 || after.st_dev != before.st_dev ||
      after.st_ino != before.st_ino || !S_ISREG(after.st_mode) ||
      after.st_uid != 0 || after.st_gid != 0 || after.st_nlink != 1 ||
      (after.st_mode & 0777) != mode) {
    close(fd);
    return AWG_HELPER_ENTRY_INVALID;
  }
  close(fd);
  return AWG_HELPER_ENTRY_VALID;
}

AwgHelperEntryState
awg_helper_storage_secure_file_state(const AwgHelperStorage *storage,
                                     AwgHelperStorageArea area,
                                     const char *name,
                                     mode_t mode)
{
  return secure_file_state_at(storage_area_fd(storage, area), name, mode);
}

AwgHelperEntryState
awg_helper_storage_config_security_state(const AwgHelperStorage *storage,
                                         const char *config_name)
{
  struct stat status;

  if (fstatat(storage->config,
              config_name,
              &status,
              AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT ? AWG_HELPER_ENTRY_MISSING :
                            AWG_HELPER_ENTRY_INVALID;
  if (!S_ISREG(status.st_mode) || status.st_uid != 0 || status.st_gid != 0 ||
      status.st_nlink != 1 || (status.st_mode & 0777) != 0600)
    return AWG_HELPER_ENTRY_INVALID;
  return AWG_HELPER_ENTRY_VALID;
}

static gboolean
read_secure_file_at(int dir,
                    const char *name,
                    mode_t mode,
                    gsize maximum_size,
                    guint8 **contents,
                    gsize *length)
{
  struct stat before;
  struct stat after;
  guint8 *buffer = NULL;
  gsize used = 0;
  int fd = -1;

  *contents = NULL;
  *length = 0;
  if (fstatat(dir, name, &before, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISREG(before.st_mode) || before.st_uid != 0 || before.st_gid != 0 ||
      before.st_nlink != 1 || (before.st_mode & 0777) != mode ||
      before.st_size < 0 || (guint64) before.st_size > maximum_size)
    return FALSE;
  fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0 || fstat(fd, &after) != 0 || after.st_dev != before.st_dev ||
      after.st_ino != before.st_ino)
    goto fail;
  buffer = g_malloc(MAX((gsize) before.st_size, 1u));
  while (used < (gsize) before.st_size) {
    ssize_t count = read(fd, buffer + used, (gsize) before.st_size - used);

    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      goto fail;
    used += (gsize) count;
  }
  if (fstat(fd, &after) != 0 || after.st_dev != before.st_dev ||
      after.st_ino != before.st_ino || after.st_size != before.st_size)
    goto fail;
  close(fd);
  *contents = buffer;
  *length = used;
  return TRUE;

fail:
  awg_helper_close_fd(&fd);
  network_sidebar_amneziawg_secret_free(buffer,
                                         MAX((gsize) before.st_size, 1u));
  return FALSE;
}

gboolean
awg_helper_storage_read_secure_file(const AwgHelperStorage *storage,
                                    AwgHelperStorageArea area,
                                    const char *name,
                                    mode_t mode,
                                    gsize maximum_size,
                                    guint8 **contents,
                                    gsize *length)
{
  return read_secure_file_at(storage_area_fd(storage, area),
                             name,
                             mode,
                             maximum_size,
                             contents,
                             length);
}

static gboolean
same_config_metadata(const struct stat *left, const struct stat *right)
{
  return left->st_dev == right->st_dev &&
         left->st_ino == right->st_ino &&
         left->st_mode == right->st_mode &&
         left->st_uid == right->st_uid &&
         left->st_gid == right->st_gid &&
         left->st_nlink == right->st_nlink &&
         left->st_size == right->st_size &&
         left->st_mtim.tv_sec == right->st_mtim.tv_sec &&
         left->st_mtim.tv_nsec == right->st_mtim.tv_nsec &&
         left->st_ctim.tv_sec == right->st_ctim.tv_sec &&
         left->st_ctim.tv_nsec == right->st_ctim.tv_nsec;
}

NetworkSidebarAmneziaWGHelperExit
awg_helper_storage_read_config(const AwgHelperStorage *storage,
                               const char *config_name,
                               guint8 **contents_out,
                               gsize *length_out,
                               AwgHelperStorageIdentity *identity)
{
  struct stat before;
  struct stat opened;
  struct stat after;
  struct stat current;
  guint8 *contents = NULL;
  gsize used = 0;
  NetworkSidebarAmneziaWGHelperExit result =
    NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
  int fd = -1;

  *contents_out = NULL;
  *length_out = 0;
  if (fstatat(storage->config,
              config_name,
              &before,
              AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT ? NETWORK_SIDEBAR_AMNEZIAWG_HELPER_NOT_FOUND :
                            NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
  if (!S_ISREG(before.st_mode) || before.st_uid != 0 || before.st_gid != 0 ||
      before.st_nlink != 1 || (before.st_mode & 0777) != 0600)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
  if (before.st_size < 0 ||
      (guint64) before.st_size > NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_CONFIG;

  fd = openat(storage->config,
              config_name,
              O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0 || fstat(fd, &opened) != 0 ||
      !same_config_metadata(&before, &opened))
    goto out;

  contents = g_malloc(NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE + 1);
  while (used <= NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE) {
    ssize_t count = read(fd,
                         contents + used,
                         NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE + 1 - used);

    if (count < 0) {
      if (errno == EINTR)
        continue;
      result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INPUT_FAILED;
      goto out;
    }
    if (count == 0)
      break;
    used += (gsize) count;
  }
  if (used > NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE) {
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_CONFIG;
    goto out;
  }
  if (used != (gsize) opened.st_size ||
      fstat(fd, &after) != 0 || !same_config_metadata(&opened, &after) ||
      fstatat(storage->config,
              config_name,
              &current,
              AT_SYMLINK_NOFOLLOW) != 0 ||
      !same_config_metadata(&after, &current))
    goto out;
  if (identity != NULL)
    identity->status = current;
  *contents_out = contents;
  *length_out = used;
  contents = NULL;
  result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;

out:
  awg_helper_close_fd(&fd);
  if (contents != NULL) {
    awg_helper_wipe_bytes(contents,
                          NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE + 1);
    g_free(contents);
  }
  return result;
}

static gboolean remove_entry_and_sync_at(int dir, const char *name);

static int
create_temp_config_at(int dir, char *name, gsize name_size)
{
  if (name_size <
      NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH + sizeof(".conf")) {
    errno = ENAMETOOLONG;
    return -1;
  }

  for (unsigned int attempt = 0; attempt < 128; attempt++) {
    guint8 random_bytes[NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH - 1];
    gsize random_offset = 0;
    int fd;

    while (random_offset < sizeof(random_bytes)) {
      ssize_t count = getrandom(random_bytes + random_offset,
                                sizeof(random_bytes) - random_offset,
                                0);
      if (count < 0) {
        if (errno == EINTR)
          continue;
        return -1;
      }
      if (count == 0) {
        errno = EIO;
        return -1;
      }
      random_offset += (gsize) count;
    }

    name[0] = 'n';
    for (gsize i = 0; i < sizeof(random_bytes); i++)
      name[i + 1] = temp_config_alphabet[random_bytes[i] & 63];
    memcpy(name + NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH,
           ".conf",
           sizeof(".conf"));
    fd = openat(dir,
                name,
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                0600);
    if (fd >= 0)
      return fd;
    if (errno != EEXIST)
      return -1;
  }
  errno = EEXIST;
  return -1;
}

gboolean
awg_helper_storage_write_staging_candidate(
  const AwgHelperStorage *storage,
  const guint8 *contents,
  gsize length,
  char *temp_name,
  gsize temp_name_size,
  gboolean *candidate_created)
{
  int temp;

  *candidate_created = FALSE;
  temp = create_temp_config_at(storage->staging, temp_name, temp_name_size);
  if (temp < 0)
    return FALSE;
  *candidate_created = TRUE;
  if (fchown(temp, 0, 0) != 0 || fchmod(temp, 0600) != 0 ||
      !awg_helper_write_all(temp, contents, length) || fsync(temp) != 0) {
    close(temp);
    return FALSE;
  }
  close(temp);
  return TRUE;
}

static gboolean
atomic_write_at(int dir,
                const char *target,
                const guint8 *contents,
                gsize length,
                mode_t mode)
{
  char temp_name[64] = { 0 };
  int temp = create_temp_config_at(dir, temp_name, sizeof(temp_name));

  if (temp < 0)
    return FALSE;
  if (fchown(temp, 0, 0) != 0 || fchmod(temp, mode) != 0 ||
      !awg_helper_write_all(temp, contents, length) || fsync(temp) != 0) {
    close(temp);
    remove_entry_and_sync_at(dir, temp_name);
    return FALSE;
  }
  if (close(temp) != 0 || renameat(dir, temp_name, dir, target) != 0 ||
      fsync(dir) != 0) {
    remove_entry_and_sync_at(dir, temp_name);
    return FALSE;
  }
  return secure_file_state_at(dir, target, mode) == AWG_HELPER_ENTRY_VALID;
}

gboolean
awg_helper_storage_atomic_write(const AwgHelperStorage *storage,
                                AwgHelperStorageArea area,
                                const char *target,
                                const guint8 *contents,
                                gsize length,
                                mode_t mode)
{
  return atomic_write_at(storage_area_fd(storage, area),
                         target,
                         contents,
                         length,
                         mode);
}

static gboolean
remove_entry_and_sync_at(int dir, const char *name)
{
  if (unlinkat(dir, name, 0) != 0) {
    if (errno != ENOENT)
      return FALSE;
  }
  return fsync(dir) == 0;
}

gboolean
awg_helper_storage_remove_and_sync(const AwgHelperStorage *storage,
                                   AwgHelperStorageArea area,
                                   const char *name)
{
  return remove_entry_and_sync_at(storage_area_fd(storage, area), name);
}

gboolean
awg_helper_storage_unlink_staging(const AwgHelperStorage *storage,
                                  const char *name)
{
  return unlinkat(storage->staging, name, 0) == 0;
}

gboolean
awg_helper_storage_rename_staging_noreplace(const AwgHelperStorage *storage,
                                            const char *temp_name,
                                            const char *config_name)
{
  return renameat2(storage->staging,
                   temp_name,
                   storage->config,
                   config_name,
                   RENAME_NOREPLACE) == 0;
}

gboolean
awg_helper_storage_exchange_staging_config(const AwgHelperStorage *storage,
                                           const char *temp_name,
                                           const char *config_name)
{
  return renameat2(storage->staging,
                   temp_name,
                   storage->config,
                   config_name,
                   RENAME_EXCHANGE) == 0;
}

gboolean
awg_helper_storage_sync_config_and_staging(const AwgHelperStorage *storage)
{
  return fsync(storage->config) == 0 && fsync(storage->staging) == 0;
}

gboolean
awg_helper_storage_sync_staging(const AwgHelperStorage *storage)
{
  return fsync(storage->staging) == 0;
}

gboolean
awg_helper_storage_get_config_identity(const AwgHelperStorage *storage,
                                       const char *config_name,
                                       AwgHelperStorageIdentity *identity)
{
  struct stat *status = &identity->status;

  return fstatat(storage->config,
                 config_name,
                 status,
                 AT_SYMLINK_NOFOLLOW) == 0 &&
         S_ISREG(status->st_mode) && status->st_uid == 0 &&
         status->st_gid == 0 && status->st_nlink == 1 &&
         (status->st_mode & 0777) == 0600;
}

gboolean
awg_helper_storage_config_matches_identity(
  const AwgHelperStorage *storage,
  const char *config_name,
  const AwgHelperStorageIdentity *identity)
{
  struct stat current;

  return fstatat(storage->config,
                 config_name,
                 &current,
                 AT_SYMLINK_NOFOLLOW) == 0 &&
         same_config_metadata(&identity->status, &current);
}

gboolean
awg_helper_storage_displaced_matches_identity(
  const AwgHelperStorage *storage,
  const char *temp_name,
  const AwgHelperStorageIdentity *identity)
{
  const struct stat *original = &identity->status;
  struct stat displaced;

  return fstatat(storage->staging,
                 temp_name,
                 &displaced,
                 AT_SYMLINK_NOFOLLOW) == 0 &&
         displaced.st_dev == original->st_dev &&
         displaced.st_ino == original->st_ino &&
         displaced.st_mode == original->st_mode &&
         displaced.st_uid == original->st_uid &&
         displaced.st_gid == original->st_gid &&
         displaced.st_nlink == original->st_nlink &&
         displaced.st_size == original->st_size &&
         displaced.st_mtim.tv_sec == original->st_mtim.tv_sec &&
         displaced.st_mtim.tv_nsec == original->st_mtim.tv_nsec;
}

AwgHelperStorageIterator *
awg_helper_storage_iterator_new(const AwgHelperStorage *storage,
                                AwgHelperStorageArea area)
{
  AwgHelperStorageIterator *iterator;
  int scan_fd = openat(storage_area_fd(storage, area),
                       ".",
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

  if (scan_fd < 0)
    return NULL;
  iterator = g_new0(AwgHelperStorageIterator, 1);
  iterator->directory = fdopendir(scan_fd);
  if (iterator->directory == NULL) {
    close(scan_fd);
    g_free(iterator);
    return NULL;
  }
  return iterator;
}

AwgHelperStorageIteratorResult
awg_helper_storage_iterator_next(AwgHelperStorageIterator *iterator,
                                 const char **entry_name)
{
  struct dirent *entry;

  errno = 0;
  entry = readdir(iterator->directory);
  if (entry == NULL)
    return errno == 0 ? AWG_HELPER_STORAGE_ITERATOR_DONE :
                        AWG_HELPER_STORAGE_ITERATOR_FAILED;
  *entry_name = entry->d_name;
  return AWG_HELPER_STORAGE_ITERATOR_ENTRY;
}

gboolean
awg_helper_storage_iterator_free(AwgHelperStorageIterator *iterator)
{
  gboolean result;

  if (iterator == NULL)
    return TRUE;
  result = closedir(iterator->directory) == 0;
  g_free(iterator);
  return result;
}

static gboolean
runtime_entry_is_internal(const char *name)
{
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
      strcmp(name, ".configs") == 0 || strcmp(name, ".helper.lock") == 0)
    return TRUE;

  /* atomic_write_at() can leave a candidate after an interrupted write. Its
   * 20-byte name cannot also be a valid (at most 15-byte) interface name. */
  if (strlen(name) != NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH + 5 ||
      name[0] != 'n' ||
      strcmp(name + NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH, ".conf") != 0)
    return FALSE;
  for (guint i = 1; i < NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH; i++) {
    if (strchr(temp_config_alphabet, name[i]) == NULL)
      return FALSE;
  }
  return TRUE;
}

static gint
runtime_name_compare(gconstpointer left, gconstpointer right)
{
  return strcmp(*(const char * const *) left, *(const char * const *) right);
}

NetworkSidebarAmneziaWGHelperExit
awg_helper_storage_list_runtime_names(const AwgHelperStorage *storage,
                                      gint64 deadline,
                                      GPtrArray **names_out)
{
  g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
  AwgHelperStorageIterator *iterator;
  NetworkSidebarAmneziaWGHelperExit result;
  guint entries = 0;

  *names_out = NULL;
  result = awg_helper_phase_stop_status(deadline);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  iterator = awg_helper_storage_iterator_new(storage, AWG_HELPER_STORAGE_RUNTIME);
  if (iterator == NULL)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  for (;;) {
    const char *name;
    char *owned_name;
    AwgHelperStorageIteratorResult next;

    result = awg_helper_phase_stop_status(deadline);
    if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
      break;
    next = awg_helper_storage_iterator_next(iterator, &name);
    if (next == AWG_HELPER_STORAGE_ITERATOR_DONE)
      break;
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
    if (next != AWG_HELPER_STORAGE_ITERATOR_ENTRY ||
        ++entries > AWG_HELPER_MAX_RUNTIME_DIRECTORY_ENTRIES)
      break;
    if (runtime_entry_is_internal(name))
      continue;
    if (!network_sidebar_amneziawg_name_is_valid(name) ||
        names->len == AWG_HELPER_MAX_RUNTIME_SESSIONS ||
        g_hash_table_contains(seen, name))
      break;
    owned_name = g_strdup(name);
    g_ptr_array_add(names, owned_name);
    g_hash_table_add(seen, owned_name);
  }
  if (!awg_helper_storage_iterator_free(iterator))
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED;
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  result = awg_helper_phase_stop_status(deadline);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  g_ptr_array_sort(names, runtime_name_compare);
  *names_out = g_steal_pointer(&names);
  return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
}

AwgHelperEntryState
awg_helper_storage_read_runtime_marker(
  const AwgHelperStorage *storage,
  const char *name,
  NetworkSidebarAmneziaWGRuntimeMarker *marker)
{
  guint8 buffer[NETWORK_SIDEBAR_AMNEZIAWG_MAX_RUNTIME_MARKER_SIZE];
  gsize used = 0;
  struct stat before;
  struct stat after;
  int fd;

  if (fstatat(storage->runtime, name, &before, AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT ? AWG_HELPER_ENTRY_MISSING :
                            AWG_HELPER_ENTRY_INVALID;
  if (!S_ISREG(before.st_mode) || before.st_uid != 0 || before.st_gid != 0 ||
      before.st_nlink != 1 || (before.st_mode & 0777) != 0644)
    return AWG_HELPER_ENTRY_INVALID;
  fd = openat(storage->runtime, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return AWG_HELPER_ENTRY_INVALID;
  if (fstat(fd, &after) != 0 || after.st_dev != before.st_dev ||
      after.st_ino != before.st_ino || !S_ISREG(after.st_mode) ||
      after.st_uid != 0 || after.st_gid != 0 || after.st_nlink != 1 ||
      (after.st_mode & 0777) != 0644) {
    close(fd);
    return AWG_HELPER_ENTRY_INVALID;
  }
  while (used < sizeof(buffer)) {
    ssize_t count = read(fd, buffer + used, sizeof(buffer) - used);

    if (count < 0) {
      if (errno == EINTR)
        continue;
      close(fd);
      return AWG_HELPER_ENTRY_INVALID;
    }
    if (count == 0)
      break;
    used += (gsize) count;
  }
  if (used == sizeof(buffer)) {
    guint8 extra;
    ssize_t count;

    do
      count = read(fd, &extra, 1);
    while (count < 0 && errno == EINTR);
    if (count != 0) {
      close(fd);
      return AWG_HELPER_ENTRY_INVALID;
    }
  }
  close(fd);
  return network_sidebar_amneziawg_runtime_marker_parse(buffer, used, marker) ?
    AWG_HELPER_ENTRY_VALID : AWG_HELPER_ENTRY_INVALID;
}

gboolean
awg_helper_storage_write_runtime_marker(
  const AwgHelperStorage *storage,
  const char *name,
  const NetworkSidebarAmneziaWGRuntimeMarker *marker)
{
  guint8 *contents = NULL;
  gsize length = 0;
  gboolean result;

  if (!network_sidebar_amneziawg_runtime_marker_serialize(marker,
                                                           &contents,
                                                           &length))
    return FALSE;
  result = atomic_write_at(storage->runtime, name, contents, length, 0644);
  network_sidebar_amneziawg_secret_free(contents, length);
  return result;
}
