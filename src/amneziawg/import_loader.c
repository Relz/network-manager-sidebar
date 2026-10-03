#include "amneziawg/import_loader.h"

#include "amneziawg/amneziawg.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define AWG_IMPORT_LOAD_TIMEOUT_SECONDS 30
#define AWG_IMPORT_READ_CHUNK_SIZE (64u * 1024u)

typedef struct {
  guint8 *data;
  gsize length;
} SensitiveBytes;

typedef struct {
  char *path;
  gint64 deadline;
  GBytes *config;
} ImportRead;

struct _NetworkSidebarAwgImportLoader {
  gint ref_count;
  GFile *file;
  GCancellable *cancellable;
  NetworkSidebarAwgImportLoadCallback callback;
  gpointer user_data;
  guint timeout_source;
  gboolean started;
  gboolean completed;
};

static void
import_loader_wipe_bytes(void *data, gsize length)
{
  volatile guint8 *bytes = data;

  while (length-- > 0)
    *bytes++ = 0;
}

static void
sensitive_bytes_free(gpointer user_data)
{
  SensitiveBytes *bytes = user_data;

  if (bytes == NULL)
    return;
  import_loader_wipe_bytes(bytes->data, bytes->length);
  g_free(bytes->data);
  g_free(bytes);
}

static void
import_read_free(gpointer user_data)
{
  ImportRead *work = user_data;

  g_clear_pointer(&work->config, g_bytes_unref);
  g_free(work->path);
  g_free(work);
}

NetworkSidebarAwgImportLoader *
network_sidebar_awg_import_loader_ref(NetworkSidebarAwgImportLoader *loader)
{
  if (loader == NULL)
    return NULL;
  g_atomic_int_inc(&loader->ref_count);
  return loader;
}

void
network_sidebar_awg_import_loader_unref(NetworkSidebarAwgImportLoader *loader)
{
  if (loader == NULL)
    return;
  if (!g_atomic_int_dec_and_test(&loader->ref_count))
    return;

  if (loader->timeout_source != 0)
    g_source_remove(loader->timeout_source);
  if (!loader->completed)
    g_cancellable_cancel(loader->cancellable);
  g_clear_object(&loader->file);
  g_clear_object(&loader->cancellable);
  g_free(loader);
}

NetworkSidebarAwgImportLoader *
network_sidebar_awg_import_loader_new(
  GFile *file,
  NetworkSidebarAwgImportLoadCallback callback,
  gpointer user_data)
{
  NetworkSidebarAwgImportLoader *loader;

  g_return_val_if_fail(G_IS_FILE(file), NULL);
  g_return_val_if_fail(callback != NULL, NULL);

  loader = g_new0(NetworkSidebarAwgImportLoader, 1);
  loader->ref_count = 1;
  loader->file = g_object_ref(file);
  loader->cancellable = g_cancellable_new();
  loader->callback = callback;
  loader->user_data = user_data;
  return loader;
}

static void
complete_load(NetworkSidebarAwgImportLoader *loader,
              NetworkSidebarAwgImportLoadResult result,
              GBytes *config)
{
  NetworkSidebarAwgImportLoadCallback callback;
  gpointer user_data;

  if (loader->completed)
    return;
  loader->completed = TRUE;
  if (loader->timeout_source != 0) {
    g_source_remove(loader->timeout_source);
    loader->timeout_source = 0;
  }
  callback = loader->callback;
  user_data = loader->user_data;
  loader->callback = NULL;
  loader->user_data = NULL;
  if (callback != NULL)
    callback(loader, result, config, user_data);
}

static gboolean
load_timeout_cb(gpointer user_data)
{
  NetworkSidebarAwgImportLoader *loader = user_data;

  loader->timeout_source = 0;
  if (loader->completed)
    return G_SOURCE_REMOVE;
  g_cancellable_cancel(loader->cancellable);
  complete_load(loader, NETWORK_SIDEBAR_AWG_IMPORT_LOAD_TIMED_OUT, NULL);
  return G_SOURCE_REMOVE;
}

void
network_sidebar_awg_import_loader_cancel(NetworkSidebarAwgImportLoader *loader)
{
  g_autoptr(NetworkSidebarAwgImportLoader) loader_ref =
    network_sidebar_awg_import_loader_ref(loader);

  loader = loader_ref;
  if (loader == NULL || loader->completed)
    return;
  g_cancellable_cancel(loader->cancellable);
  complete_load(loader, NETWORK_SIDEBAR_AWG_IMPORT_LOAD_CANCELLED, NULL);
}

static NetworkSidebarAwgImportLoadResult
import_read_stop_result(GCancellable *cancellable, gint64 deadline)
{
  if (g_cancellable_is_cancelled(cancellable))
    return NETWORK_SIDEBAR_AWG_IMPORT_LOAD_CANCELLED;
  if (g_get_monotonic_time() >= deadline)
    return NETWORK_SIDEBAR_AWG_IMPORT_LOAD_TIMED_OUT;
  return NETWORK_SIDEBAR_AWG_IMPORT_LOAD_SUCCESS;
}

static void
import_read_worker(GTask *task,
                   gpointer source_object,
                   gpointer task_data,
                   GCancellable *cancellable)
{
  ImportRead *work = task_data;
  SensitiveBytes *bytes = NULL;
  NetworkSidebarAwgImportLoadResult result;
  struct stat status;
  int fd = -1;
  (void) source_object;

  for (;;) {
    result = import_read_stop_result(cancellable, work->deadline);
    if (result != NETWORK_SIDEBAR_AWG_IMPORT_LOAD_SUCCESS)
      goto out;
    fd = open(work->path,
              O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd >= 0)
      break;
    if (errno == EINTR)
      continue;
    result = errno == ELOOP ? NETWORK_SIDEBAR_AWG_IMPORT_LOAD_NOT_REGULAR :
                             NETWORK_SIDEBAR_AWG_IMPORT_LOAD_OPEN_FAILED;
    goto out;
  }

  /* Validate and read the same opened object; never reopen its pathname. */
  if (fstat(fd, &status) != 0) {
    result = NETWORK_SIDEBAR_AWG_IMPORT_LOAD_INSPECT_FAILED;
    goto out;
  }
  if (!S_ISREG(status.st_mode)) {
    result = NETWORK_SIDEBAR_AWG_IMPORT_LOAD_NOT_REGULAR;
    goto out;
  }
  if (status.st_size < 0 ||
      (guint64) status.st_size > NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE) {
    result = NETWORK_SIDEBAR_AWG_IMPORT_LOAD_TOO_LARGE;
    goto out;
  }

  bytes = g_new0(SensitiveBytes, 1);
  bytes->data = g_malloc(NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE + 1);
  for (;;) {
    gsize remaining = NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE + 1 -
                      bytes->length;
    ssize_t count;

    result = import_read_stop_result(cancellable, work->deadline);
    if (result != NETWORK_SIDEBAR_AWG_IMPORT_LOAD_SUCCESS)
      goto out;
    count = read(fd, bytes->data + bytes->length,
                  MIN(remaining, AWG_IMPORT_READ_CHUNK_SIZE));
    if (count < 0) {
      if (errno == EINTR)
        continue;
      result = NETWORK_SIDEBAR_AWG_IMPORT_LOAD_READ_FAILED;
      goto out;
    }
    if (count == 0) {
      if (bytes->length == 0)
        result = NETWORK_SIDEBAR_AWG_IMPORT_LOAD_EMPTY;
      break;
    }
    bytes->length += (gsize) count;
    if (bytes->length > NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE) {
      result = NETWORK_SIDEBAR_AWG_IMPORT_LOAD_TOO_LARGE;
      goto out;
    }
  }

out:
  if (fd >= 0)
    close(fd);
  if (result == NETWORK_SIDEBAR_AWG_IMPORT_LOAD_SUCCESS)
    result = import_read_stop_result(cancellable, work->deadline);
  if (result == NETWORK_SIDEBAR_AWG_IMPORT_LOAD_SUCCESS) {
    work->config = g_bytes_new_with_free_func(bytes->data, bytes->length,
                                              sensitive_bytes_free, bytes);
  } else {
    sensitive_bytes_free(bytes);
  }
  g_task_return_int(task, result);
}

static void
import_read_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(NetworkSidebarAwgImportLoader) loader = user_data;
  GTask *task = G_TASK(result);
  ImportRead *work = g_task_get_task_data(task);
  gssize value = g_task_propagate_int(task, NULL);
  NetworkSidebarAwgImportLoadResult status = value < 0 ?
    NETWORK_SIDEBAR_AWG_IMPORT_LOAD_CANCELLED :
    (NetworkSidebarAwgImportLoadResult) value;
  (void) source;

  if (!loader->completed) {
    if (g_get_monotonic_time() >= work->deadline)
      status = NETWORK_SIDEBAR_AWG_IMPORT_LOAD_TIMED_OUT;
    complete_load(loader, status,
                  status == NETWORK_SIDEBAR_AWG_IMPORT_LOAD_SUCCESS ?
                    work->config : NULL);
  }
  g_clear_pointer(&work->config, g_bytes_unref);
}

void
network_sidebar_awg_import_loader_start(NetworkSidebarAwgImportLoader *loader)
{
  g_autoptr(NetworkSidebarAwgImportLoader) loader_ref =
    network_sidebar_awg_import_loader_ref(loader);
  g_autofree char *path = NULL;
  g_autoptr(GTask) task = NULL;
  ImportRead *work;

  loader = loader_ref;
  if (loader == NULL || loader->started || loader->completed)
    return;
  loader->started = TRUE;
  path = g_file_get_path(loader->file);
  if (path == NULL) {
    complete_load(loader, NETWORK_SIDEBAR_AWG_IMPORT_LOAD_NOT_LOCAL, NULL);
    return;
  }

  work = g_new0(ImportRead, 1);
  work->path = g_steal_pointer(&path);
  work->deadline = g_get_monotonic_time() +
                   AWG_IMPORT_LOAD_TIMEOUT_SECONDS * G_USEC_PER_SEC;
  task = g_task_new(NULL, loader->cancellable, import_read_cb,
                    network_sidebar_awg_import_loader_ref(loader));
  g_task_set_task_data(task, work, import_read_free);
  /* Cancellation completes the UI immediately, but the worker retains its
   * private buffer and descriptor until its I/O has actually returned. */
  loader->timeout_source = g_timeout_add_full(
    G_PRIORITY_DEFAULT,
    AWG_IMPORT_LOAD_TIMEOUT_SECONDS * 1000,
    load_timeout_cb,
    network_sidebar_awg_import_loader_ref(loader),
    (GDestroyNotify) network_sidebar_awg_import_loader_unref);
  g_task_run_in_thread(task, import_read_worker);
}
