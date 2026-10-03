#include "amneziawg/runtime_source.h"

#include "amneziawg/link_identity.h"
#include "amneziawg/runtime_journal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define AWG_RUNTIME_MONITOR_DEBOUNCE_MSEC 150
#define AWG_RUNTIME_MAX_NAMES 1024u
#define AWG_RUNTIME_MAX_DIRECTORY_ENTRIES 4096u
#define AWG_LINK_EVENT_BUFFER_SIZE (64u * 1024u)
#define AWG_LINK_EVENT_DRAIN_LIMIT 64u

typedef enum {
  RUNTIME_DIRECTORY_MISSING,
  RUNTIME_DIRECTORY_TRUSTED,
  RUNTIME_DIRECTORY_UNKNOWN,
} RuntimeDirectoryState;

typedef enum {
  RUNTIME_MARKER_ABSENT,
  RUNTIME_MARKER_VALID,
  RUNTIME_MARKER_INVALID,
  RUNTIME_MARKER_UNKNOWN,
} RuntimeMarkerState;

typedef struct {
  GPtrArray *names;
  GPtrArray *previous_names;
  guint64 generation;
} RuntimeScanRequest;

typedef struct {
  GPtrArray *records;
  NetworkSidebarAmneziaWGLinkEventFilter *link_filter;
  guint64 generation;
  gboolean complete;
} RuntimeScanResult;

typedef struct {
  NetworkSidebarAwgRuntimeSource *source;
  RuntimeScanResult *result;
} RuntimeScanCompletion;

struct _NetworkSidebarAwgRuntimeSource {
  gint ref_count;
  NetworkSidebarAwgRuntimeChangedCallback changed;
  gpointer user_data;
  GCancellable *lifecycle_cancellable;
  GFileMonitor *monitor;
  char *monitor_path;
  char *monitor_expected_child;
  GPtrArray *names;
  GPtrArray *records;
  NetworkSidebarAmneziaWGLinkEventFilter *link_filter;
  guint monitor_debounce_source;
  guint retry_source;
  guint scan_publish_source;
  guint retry_attempt;
  guint link_monitor_source;
  int link_monitor_fd;
  guint64 refresh_generation;
  gboolean records_fresh;
  gboolean refresh_pending;
  gboolean scan_running;
  gboolean scan_queued;
  NetworkSidebarAwgRefreshPurpose queued_purpose;
  NetworkSidebarAwgRefreshPurpose scan_purpose;
  NetworkSidebarAwgRefreshPurpose retry_purpose;
  gboolean mutation_active;
  gboolean reconciliation_active;
  gboolean monitor_enabled;
  gboolean started;
  gboolean stopping;
};

static void start_scan(NetworkSidebarAwgRuntimeSource *source);
static gboolean ensure_monitors(NetworkSidebarAwgRuntimeSource *source,
                                gboolean *ready);

static gboolean
runtime_source_is_running(NetworkSidebarAwgRuntimeSource *source)
{
  return source != NULL && source->started && !source->stopping;
}

gboolean
network_sidebar_awg_runtime_source_is_checking(
  NetworkSidebarAwgRuntimeSource *source)
{
  return runtime_source_is_running(source) && source->monitor_enabled &&
         (source->monitor_debounce_source != 0 || source->scan_running);
}

gboolean
network_sidebar_awg_runtime_source_is_loading(
  NetworkSidebarAwgRuntimeSource *source)
{
  return network_sidebar_awg_runtime_source_is_checking(source) &&
         ((source->scan_running &&
           source->scan_purpose == NETWORK_SIDEBAR_AWG_REFRESH_VISIBLE) ||
          (source->refresh_pending &&
           source->queued_purpose == NETWORK_SIDEBAR_AWG_REFRESH_VISIBLE));
}

static NetworkSidebarAwgRefreshPurpose
automatic_refresh_purpose(NetworkSidebarAwgRuntimeSource *source)
{
  if (source->mutation_active || source->reconciliation_active)
    return NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
  return NETWORK_SIDEBAR_AWG_REFRESH_VISIBLE;
}

void
network_sidebar_awg_runtime_source_set_mutation_active(
  NetworkSidebarAwgRuntimeSource *source, gboolean active)
{
  if (source != NULL && !source->stopping)
    source->mutation_active = active;
}

NetworkSidebarAwgRuntimeSource *
network_sidebar_awg_runtime_source_ref(NetworkSidebarAwgRuntimeSource *source)
{
  if (source == NULL)
    return NULL;
  g_atomic_int_inc(&source->ref_count);
  return source;
}

static void
clear_source_id(guint *source_id)
{
  guint id = *source_id;

  *source_id = 0;
  if (id != 0)
    g_source_remove(id);
}

static void
clear_monitor(NetworkSidebarAwgRuntimeSource *source)
{
  if (source->monitor == NULL)
    return;
  g_signal_handlers_disconnect_by_data(source->monitor, source);
  g_file_monitor_cancel(source->monitor);
  g_clear_object(&source->monitor);
  g_clear_pointer(&source->monitor_path, g_free);
  g_clear_pointer(&source->monitor_expected_child, g_free);
}

static void
clear_link_monitor(NetworkSidebarAwgRuntimeSource *source)
{
  clear_source_id(&source->link_monitor_source);
  if (source->link_monitor_fd >= 0)
    close(source->link_monitor_fd);
  source->link_monitor_fd = -1;
}

static void
runtime_source_stop_internal(NetworkSidebarAwgRuntimeSource *source)
{
  if (source->stopping)
    return;
  source->stopping = TRUE;
  source->started = FALSE;

  clear_source_id(&source->monitor_debounce_source);
  clear_source_id(&source->retry_source);
  clear_source_id(&source->scan_publish_source);
  if (source->lifecycle_cancellable != NULL)
    g_cancellable_cancel(source->lifecycle_cancellable);
  clear_monitor(source);
  clear_link_monitor(source);
  g_clear_object(&source->lifecycle_cancellable);
  source->changed = NULL;
  source->user_data = NULL;
  source->monitor_enabled = FALSE;
  source->records_fresh = FALSE;
  source->refresh_pending = FALSE;
  source->scan_queued = FALSE;
}

void
network_sidebar_awg_runtime_source_unref(NetworkSidebarAwgRuntimeSource *source)
{
  if (source == NULL)
    return;
  if (!g_atomic_int_dec_and_test(&source->ref_count))
    return;
  runtime_source_stop_internal(source);
  g_clear_pointer(&source->names, g_ptr_array_unref);
  g_clear_pointer(&source->records, g_ptr_array_unref);
  g_clear_pointer(&source->link_filter,
                  network_sidebar_amneziawg_link_event_filter_free);
  g_free(source);
}

NetworkSidebarAwgRuntimeSource *
network_sidebar_awg_runtime_source_new(
  NetworkSidebarAwgRuntimeChangedCallback changed,
  gpointer user_data)
{
  NetworkSidebarAwgRuntimeSource *source = g_new0(
    NetworkSidebarAwgRuntimeSource, 1);

  source->ref_count = 1;
  source->changed = changed;
  source->user_data = user_data;
  source->link_monitor_fd = -1;
  source->names = g_ptr_array_new_with_free_func(g_free);
  source->records = g_ptr_array_new_with_free_func(
    (GDestroyNotify) network_sidebar_awg_runtime_record_free);
  return source;
}

static gint
string_pointer_compare(gconstpointer left, gconstpointer right)
{
  return strcmp(*(char * const *) left, *(char * const *) right);
}

static GPtrArray *
normalize_names(const GPtrArray *names)
{
  g_autoptr(GHashTable) unique = g_hash_table_new_full(g_str_hash,
                                                        g_str_equal,
                                                        g_free,
                                                        NULL);
  GPtrArray *normalized = g_ptr_array_new_with_free_func(g_free);
  GHashTableIter iter;
  gpointer key;

  for (guint i = 0; names != NULL && i < names->len; i++) {
    const char *name = g_ptr_array_index((GPtrArray *) names, i);

    if (network_sidebar_amneziawg_name_is_valid(name) &&
        (g_hash_table_contains(unique, name) ||
         g_hash_table_size(unique) < AWG_RUNTIME_MAX_NAMES))
      g_hash_table_add(unique, g_strdup(name));
  }
  g_hash_table_iter_init(&iter, unique);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    g_ptr_array_add(normalized, g_strdup(key));
  g_ptr_array_sort(normalized, string_pointer_compare);
  return normalized;
}

static GPtrArray *
copy_names(const GPtrArray *names)
{
  GPtrArray *copy = g_ptr_array_new_with_free_func(g_free);

  for (guint i = 0; names != NULL && i < names->len; i++)
    g_ptr_array_add(copy, g_strdup(g_ptr_array_index((GPtrArray *) names, i)));
  return copy;
}

static gboolean
names_equal(const GPtrArray *left, const GPtrArray *right)
{
  if (left == right)
    return TRUE;
  if (left == NULL || right == NULL || left->len != right->len)
    return FALSE;
  for (guint i = 0; i < left->len; i++) {
    if (strcmp(g_ptr_array_index((GPtrArray *) left, i),
               g_ptr_array_index((GPtrArray *) right, i)) != 0)
      return FALSE;
  }
  return TRUE;
}

static RuntimeDirectoryState
open_runtime_directory(int *directory_fd)
{
  struct stat status;
  int fd;

  *directory_fd = -1;
  fd = open(NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_DIR,
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return errno == ENOENT ? RUNTIME_DIRECTORY_MISSING :
                            RUNTIME_DIRECTORY_UNKNOWN;
  if (fstat(fd, &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != 0 || status.st_gid != 0 ||
      (status.st_mode & 0022) != 0) {
    close(fd);
    return RUNTIME_DIRECTORY_UNKNOWN;
  }
  *directory_fd = fd;
  return RUNTIME_DIRECTORY_TRUSTED;
}

static gboolean
add_runtime_names(int directory_fd,
                  GHashTable *names,
                  GCancellable *cancellable)
{
  DIR *directory;
  int iterator_fd;
  gboolean complete = TRUE;
  guint examined = 0;

  iterator_fd = fcntl(directory_fd, F_DUPFD_CLOEXEC, 3);
  if (iterator_fd < 0)
    return FALSE;
  directory = fdopendir(iterator_fd);
  if (directory == NULL) {
    close(iterator_fd);
    return FALSE;
  }

  for (;;) {
    struct dirent *entry;

    if ((cancellable != NULL && g_cancellable_is_cancelled(cancellable)) ||
        examined >= AWG_RUNTIME_MAX_DIRECTORY_ENTRIES) {
      complete = FALSE;
      break;
    }
    errno = 0;
    entry = readdir(directory);
    if (entry == NULL) {
      if (errno != 0)
        complete = FALSE;
      break;
    }
    examined++;
    if (!network_sidebar_amneziawg_name_is_valid(entry->d_name))
      continue;
    if (g_hash_table_size(names) >= AWG_RUNTIME_MAX_NAMES &&
        !g_hash_table_contains(names, entry->d_name)) {
      complete = FALSE;
      break;
    }
    g_hash_table_add(names, g_strdup(entry->d_name));
  }
  if (closedir(directory) != 0)
    complete = FALSE;
  return complete;
}

static RuntimeMarkerState
read_runtime_marker(int directory_fd,
                    RuntimeDirectoryState directory_state,
                    const char *name,
                    NetworkSidebarAmneziaWGRuntimeMarker *marker)
{
  guint8 buffer[NETWORK_SIDEBAR_AMNEZIAWG_MAX_RUNTIME_MARKER_SIZE];
  struct stat before;
  struct stat after;
  gsize used = 0;
  int fd;

  if (directory_state == RUNTIME_DIRECTORY_MISSING)
    return RUNTIME_MARKER_ABSENT;
  if (directory_state != RUNTIME_DIRECTORY_TRUSTED)
    return RUNTIME_MARKER_UNKNOWN;
  if (fstatat(directory_fd, name, &before, AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT ? RUNTIME_MARKER_ABSENT : RUNTIME_MARKER_UNKNOWN;
  if (!S_ISREG(before.st_mode) || before.st_uid != 0 || before.st_gid != 0 ||
      before.st_nlink != 1 || (before.st_mode & 0777) != 0644)
    return RUNTIME_MARKER_INVALID;

  fd = openat(directory_fd,
              name,
              O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return RUNTIME_MARKER_UNKNOWN;
  if (fstat(fd, &after) != 0 || before.st_dev != after.st_dev ||
      before.st_ino != after.st_ino || !S_ISREG(after.st_mode) ||
      after.st_uid != 0 || after.st_gid != 0 || after.st_nlink != 1 ||
      (after.st_mode & 0777) != 0644) {
    close(fd);
    return RUNTIME_MARKER_UNKNOWN;
  }
  while (used < sizeof(buffer)) {
    ssize_t count = read(fd, buffer + used, sizeof(buffer) - used);

    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0) {
      close(fd);
      return RUNTIME_MARKER_UNKNOWN;
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
      return count < 0 ? RUNTIME_MARKER_UNKNOWN : RUNTIME_MARKER_INVALID;
    }
  }
  close(fd);
  return network_sidebar_amneziawg_runtime_marker_parse(buffer, used, marker) ?
    RUNTIME_MARKER_VALID : RUNTIME_MARKER_INVALID;
}

static RuntimeScanResult *
scan_runtime(const RuntimeScanRequest *request, GCancellable *cancellable)
{
  g_autoptr(GHashTable) names = g_hash_table_new_full(g_str_hash,
                                                       g_str_equal,
                                                       g_free,
                                                       NULL);
  g_autoptr(NetworkSidebarAmneziaWGLinkSnapshot) links = NULL;
  RuntimeScanResult *result = g_new0(RuntimeScanResult, 1);
  RuntimeDirectoryState directory_state;
  GHashTableIter iter;
  gpointer key;
  int directory_fd = -1;

  result->generation = request->generation;
  result->complete = TRUE;
  result->records = g_ptr_array_new_with_free_func(
    (GDestroyNotify) network_sidebar_awg_runtime_record_free);
  result->link_filter = network_sidebar_amneziawg_link_event_filter_new();

  /* The previous session set is cache-bounded. Inspect both its markers and
   * links before retiring names, even after a complete directory enumeration. */
  for (guint i = 0; i < request->previous_names->len; i++)
    g_hash_table_add(names, g_strdup(g_ptr_array_index(request->previous_names, i)));
  for (guint i = 0; i < request->names->len; i++) {
    const char *name = g_ptr_array_index(request->names, i);

    if (g_hash_table_contains(names, name) ||
        g_hash_table_size(names) < AWG_RUNTIME_MAX_NAMES)
      g_hash_table_add(names, g_strdup(name));
    else
      result->complete = FALSE;
  }

  directory_state = open_runtime_directory(&directory_fd);
  if (directory_state == RUNTIME_DIRECTORY_TRUSTED &&
      !add_runtime_names(directory_fd, names, cancellable)) {
    result->complete = FALSE;
  } else if (directory_state == RUNTIME_DIRECTORY_UNKNOWN) {
    result->complete = FALSE;
  }

  if (cancellable != NULL && g_cancellable_is_cancelled(cancellable))
    goto cancelled;
  links = network_sidebar_amneziawg_link_snapshot_read(cancellable);
  if (links == NULL)
    result->complete = FALSE;

  g_hash_table_iter_init(&iter, names);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    const char *name = key;
    NetworkSidebarAmneziaWGRuntimeMarker marker = { 0 };
    NetworkSidebarAmneziaWGLinkObservation observation = { 0 };
    RuntimeMarkerState marker_state;
    NetworkSidebarAmneziaWGRuntimeState state =
      NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_UNKNOWN;
    gboolean have_observation = FALSE;

    if (cancellable != NULL && g_cancellable_is_cancelled(cancellable))
      goto cancelled;
    marker_state = read_runtime_marker(directory_fd,
                                       directory_state,
                                       name,
                                       &marker);
    NetworkSidebarAmneziaWGLinkIdentity identity = {
      .name = name,
      .link_token = marker_state == RUNTIME_MARKER_VALID ? marker.link_token : NULL,
      .ifindex = marker_state == RUNTIME_MARKER_VALID ? marker.ifindex : 0,
    };
    network_sidebar_amneziawg_link_event_filter_add(result->link_filter,
                                                    links, &identity);
    if (links != NULL) {
      have_observation = network_sidebar_amneziawg_link_snapshot_observe(
        links,
        name,
        marker_state == RUNTIME_MARKER_VALID ? marker.link_token : NULL,
        &observation);
    }
    if (marker_state == RUNTIME_MARKER_UNKNOWN || !have_observation) {
      result->complete = FALSE;
    } else {
      state = network_sidebar_amneziawg_runtime_state_classify(
        marker_state != RUNTIME_MARKER_ABSENT,
        marker_state == RUNTIME_MARKER_VALID,
        &marker,
        &observation);
    }
    g_ptr_array_add(
      result->records,
      network_sidebar_awg_runtime_record_new(name,
                                              state,
                                              have_observation &&
                                                observation.named_exists));
  }
  if (directory_fd >= 0)
    close(directory_fd);
  /* Only a complete observation can prove that an event is unrelated. */
  if (!result->complete)
    g_clear_pointer(&result->link_filter,
                    network_sidebar_amneziawg_link_event_filter_free);
  return result;

cancelled:
  if (directory_fd >= 0)
    close(directory_fd);
  g_clear_pointer(&result->records, g_ptr_array_unref);
  g_clear_pointer(&result->link_filter,
                  network_sidebar_amneziawg_link_event_filter_free);
  g_free(result);
  return NULL;
}

static void
runtime_scan_request_free(RuntimeScanRequest *request)
{
  if (request == NULL)
    return;
  g_clear_pointer(&request->names, g_ptr_array_unref);
  g_clear_pointer(&request->previous_names, g_ptr_array_unref);
  g_free(request);
}

static void
runtime_scan_result_free(RuntimeScanResult *result)
{
  if (result == NULL)
    return;
  g_clear_pointer(&result->records, g_ptr_array_unref);
  g_clear_pointer(&result->link_filter,
                  network_sidebar_amneziawg_link_event_filter_free);
  g_free(result);
}

static void
runtime_scan_completion_free(RuntimeScanCompletion *completion)
{
  if (completion == NULL)
    return;
  runtime_scan_result_free(completion->result);
  network_sidebar_awg_runtime_source_unref(completion->source);
  g_free(completion);
}

static void
scan_worker(GTask *task,
            gpointer source_object,
            gpointer task_data,
            GCancellable *cancellable)
{
  RuntimeScanRequest *request = task_data;
  RuntimeScanResult *result;
  (void) source_object;

  result = scan_runtime(request, cancellable);
  if (result == NULL) {
    g_task_return_new_error(task,
                            G_IO_ERROR,
                            G_IO_ERROR_CANCELLED,
                            "AmneziaWG runtime scan was cancelled");
    return;
  }
  g_task_return_pointer(task,
                        result,
                        (GDestroyNotify) runtime_scan_result_free);
}

static void
notify_changed(NetworkSidebarAwgRuntimeSource *source)
{
  NetworkSidebarAwgRuntimeChangedCallback callback;
  gpointer user_data;

  if (!runtime_source_is_running(source))
    return;
  callback = source->changed;
  user_data = source->user_data;
  if (callback == NULL)
    return;
  network_sidebar_awg_runtime_source_ref(source);
  callback(user_data);
  network_sidebar_awg_runtime_source_unref(source);
}

static void
queue_scan(NetworkSidebarAwgRuntimeSource *source,
           NetworkSidebarAwgRefreshPurpose purpose)
{
  source->reconciliation_active = purpose == NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
  if (!source->refresh_pending) {
    source->refresh_generation = source->refresh_generation == G_MAXUINT64 ?
      1 : source->refresh_generation + 1;
  }
  source->refresh_pending = TRUE;
  /* A superseded visible scan still owns presentation until its replacement
   * settles; invalidation must not downgrade it to quiet reconciliation. */
  if (source->scan_running)
    purpose = MAX(purpose, source->scan_purpose);
  source->queued_purpose = MAX(source->queued_purpose, purpose);
}

static gboolean
invalidate_records(NetworkSidebarAwgRuntimeSource *source,
                   NetworkSidebarAwgRefreshPurpose purpose)
{
  gboolean was_fresh = source->records_fresh;

  queue_scan(source, purpose);
  source->records_fresh = FALSE;
  return was_fresh;
}

static gboolean scheduled_refresh_cb(gpointer user_data);

static void
schedule_refresh(NetworkSidebarAwgRuntimeSource *source, guint delay_msec)
{
  if (!runtime_source_is_running(source) || !source->monitor_enabled ||
      source->monitor_debounce_source != 0)
    return;
  source->monitor_debounce_source = g_timeout_add_full(
    G_PRIORITY_DEFAULT,
    delay_msec,
    scheduled_refresh_cb,
    network_sidebar_awg_runtime_source_ref(source),
    (GDestroyNotify) network_sidebar_awg_runtime_source_unref);
}

static gboolean
retry_cb(gpointer user_data)
{
  NetworkSidebarAwgRuntimeSource *source = user_data;
  NetworkSidebarAwgRefreshPurpose purpose = source->retry_purpose;

  source->retry_source = 0;
  source->retry_purpose = NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
  network_sidebar_awg_runtime_source_request_refresh(source, purpose);
  return G_SOURCE_REMOVE;
}

static void
schedule_retry(NetworkSidebarAwgRuntimeSource *source,
               NetworkSidebarAwgRefreshPurpose purpose)
{
  static const guint delays[] = { 2, 5, 15, 30, 60 };
  guint delay;

  if (!runtime_source_is_running(source) || !source->monitor_enabled)
    return;
  source->retry_purpose = MAX(source->retry_purpose, purpose);
  if (source->retry_source != 0)
    return;
  delay = delays[MIN(source->retry_attempt, G_N_ELEMENTS(delays) - 1)];
  if (source->retry_attempt < G_N_ELEMENTS(delays) - 1)
    source->retry_attempt++;
  source->retry_source = g_timeout_add_seconds_full(
    G_PRIORITY_DEFAULT,
    delay,
    retry_cb,
    network_sidebar_awg_runtime_source_ref(source),
    (GDestroyNotify) network_sidebar_awg_runtime_source_unref);
}

static gboolean
record_identifies_session(const NetworkSidebarAwgRuntimeRecord *record)
{
  return record->state != NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_INACTIVE;
}

static GPtrArray *
reconcile_scan_records(const GPtrArray *previous_records,
                       RuntimeScanResult *result)
{
  g_autoptr(GHashTable) observations = NULL;
  GPtrArray *records;

  if (result->complete)
    return g_steal_pointer(&result->records);

  observations = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < result->records->len; i++) {
    NetworkSidebarAwgRuntimeRecord *record =
      g_ptr_array_index(result->records, i);

    g_hash_table_insert(observations, record->name, record);
  }
  records = g_ptr_array_new_with_free_func(
    (GDestroyNotify) network_sidebar_awg_runtime_record_free);

  /* An incomplete scan cannot establish absence by omission. Reserve space
   * for known sessions before admitting new names, including at the limit. */
  for (guint i = 0; i < previous_records->len &&
                    records->len < AWG_RUNTIME_MAX_NAMES; i++) {
    const NetworkSidebarAwgRuntimeRecord *previous =
      g_ptr_array_index((GPtrArray *) previous_records, i);
    const NetworkSidebarAwgRuntimeRecord *observed;

    if (!record_identifies_session(previous))
      continue;
    observed = g_hash_table_lookup(observations, previous->name);
    g_ptr_array_add(
      records,
      network_sidebar_awg_runtime_record_new(
        previous->name,
        observed != NULL ? observed->state :
          NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_UNKNOWN,
        observed != NULL && observed->interface_exists));
    g_hash_table_remove(observations, previous->name);
  }
  for (guint i = 0; i < result->records->len &&
                    records->len < AWG_RUNTIME_MAX_NAMES; i++) {
    const NetworkSidebarAwgRuntimeRecord *record =
      g_ptr_array_index(result->records, i);

    if (g_hash_table_remove(observations, record->name))
      g_ptr_array_add(records,
                      network_sidebar_awg_runtime_record_new(
                        record->name, record->state, record->interface_exists));
  }
  return records;
}

static gboolean
publish_scan_cb(gpointer user_data)
{
  RuntimeScanCompletion *completion = user_data;
  NetworkSidebarAwgRuntimeSource *source = completion->source;
  RuntimeScanResult *result = completion->result;
  gboolean accepted = FALSE;
  gboolean invalidated = FALSE;
  gboolean monitors_ready = FALSE;
  gboolean restart;
  gboolean was_checking = network_sidebar_awg_runtime_source_is_checking(source);
  gboolean was_loading = network_sidebar_awg_runtime_source_is_loading(source);
  NetworkSidebarAwgRefreshPurpose purpose = source->scan_purpose;

  source->scan_publish_source = 0;
  source->scan_running = FALSE;
  if (runtime_source_is_running(source) && source->monitor_enabled) {
    gboolean monitors_changed = ensure_monitors(source, &monitors_ready);

    if (monitors_changed || !monitors_ready) {
      invalidated = invalidate_records(source, purpose);
      if (monitors_changed)
        source->scan_queued = TRUE;
    } else if (result == NULL && !source->refresh_pending) {
      invalidated = invalidate_records(source, purpose);
      schedule_retry(source, purpose);
    }
  }
  if (runtime_source_is_running(source) && source->monitor_enabled &&
      monitors_ready && result != NULL &&
      result->generation == source->refresh_generation &&
      !source->refresh_pending) {
    GPtrArray *records = reconcile_scan_records(source->records, result);

    g_clear_pointer(&source->records, g_ptr_array_unref);
    source->records = records;
    g_clear_pointer(&source->link_filter,
                    network_sidebar_amneziawg_link_event_filter_free);
    source->link_filter = g_steal_pointer(&result->link_filter);
    source->records_fresh = TRUE;
    accepted = TRUE;
    if (result->complete) {
      clear_source_id(&source->retry_source);
      source->retry_purpose = NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
      source->retry_attempt = 0;
    } else {
      schedule_retry(source, purpose);
    }
  }
  if (runtime_source_is_running(source) && source->monitor_enabled &&
      !monitors_ready)
    schedule_retry(source, purpose);

  restart = runtime_source_is_running(source) && source->monitor_enabled &&
            source->refresh_pending && source->scan_queued;
  source->scan_queued = FALSE;
  source->scan_purpose = NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
  if (restart)
    start_scan(source);
  if (!network_sidebar_awg_runtime_source_is_checking(source))
    source->reconciliation_active = FALSE;
  if (accepted || invalidated ||
      was_checking != network_sidebar_awg_runtime_source_is_checking(source) ||
      was_loading != network_sidebar_awg_runtime_source_is_loading(source))
    notify_changed(source);
  return G_SOURCE_REMOVE;
}

static void
scan_complete_cb(GObject *object, GAsyncResult *async_result, gpointer user_data)
{
  NetworkSidebarAwgRuntimeSource *source = user_data;
  g_autoptr(GError) error = NULL;
  RuntimeScanCompletion *completion;
  RuntimeScanResult *result = g_task_propagate_pointer(G_TASK(async_result),
                                                        &error);
  (void) object;

  if (!runtime_source_is_running(source) || !source->monitor_enabled) {
    source->scan_running = FALSE;
    source->scan_purpose = NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
    runtime_scan_result_free(result);
    network_sidebar_awg_runtime_source_unref(source);
    return;
  }
  completion = g_new0(RuntimeScanCompletion, 1);
  completion->source = source;
  completion->result = result;
  source->scan_publish_source = g_idle_add_full(
    G_PRIORITY_DEFAULT_IDLE,
    publish_scan_cb,
    completion,
    (GDestroyNotify) runtime_scan_completion_free);
}

static void
start_scan(NetworkSidebarAwgRuntimeSource *source)
{
  RuntimeScanRequest *request;
  GTask *task;
  gboolean monitors_ready = FALSE;
  gboolean monitors_changed;

  if (!runtime_source_is_running(source) || !source->monitor_enabled ||
      source->scan_running || !source->refresh_pending)
    return;
  monitors_changed = ensure_monitors(source, &monitors_ready);
  if ((monitors_changed || !monitors_ready) &&
      invalidate_records(source, source->queued_purpose))
    notify_changed(source);
  if (!runtime_source_is_running(source) || !source->monitor_enabled ||
      source->scan_running || !source->refresh_pending)
    return;
  request = g_new0(RuntimeScanRequest, 1);
  request->names = copy_names(source->names);
  request->previous_names = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < source->records->len; i++) {
    const NetworkSidebarAwgRuntimeRecord *record =
      g_ptr_array_index(source->records, i);

    if (record_identifies_session(record))
      g_ptr_array_add(request->previous_names, g_strdup(record->name));
  }
  request->generation = source->refresh_generation;
  source->refresh_pending = FALSE;
  source->scan_queued = FALSE;
  source->scan_running = TRUE;
  source->scan_purpose = source->queued_purpose;
  source->queued_purpose = NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
  task = g_task_new(NULL,
                    source->lifecycle_cancellable,
                    scan_complete_cb,
                    network_sidebar_awg_runtime_source_ref(source));
  g_task_set_return_on_cancel(task, FALSE);
  g_task_set_task_data(task,
                       request,
                       (GDestroyNotify) runtime_scan_request_free);
  g_task_run_in_thread(task, scan_worker);
  g_object_unref(task);
}

void
network_sidebar_awg_runtime_source_poll(NetworkSidebarAwgRuntimeSource *source,
                                        NetworkSidebarAwgRefreshPurpose purpose)
{
  gboolean was_loading;

  if (!runtime_source_is_running(source) || !source->monitor_enabled)
    return;
  was_loading = network_sidebar_awg_runtime_source_is_loading(source);
  if (source->scan_running || source->refresh_pending) {
    if (purpose == NETWORK_SIDEBAR_AWG_REFRESH_VISIBLE)
      source->reconciliation_active = FALSE;
    if (source->scan_running)
      source->scan_purpose = MAX(source->scan_purpose, purpose);
    if (source->refresh_pending)
      source->queued_purpose = MAX(source->queued_purpose, purpose);
    if (was_loading != network_sidebar_awg_runtime_source_is_loading(source))
      notify_changed(source);
    return;
  }
  /* Polling alone is not evidence that a monitored observation is obsolete.
   * Real change notifications still invalidate it and retire this generation. */
  clear_source_id(&source->retry_source);
  source->retry_purpose = NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
  queue_scan(source, purpose);
  schedule_refresh(source, 0);
  notify_changed(source);
}

void
network_sidebar_awg_runtime_source_request_refresh(
  NetworkSidebarAwgRuntimeSource *source,
  NetworkSidebarAwgRefreshPurpose purpose)
{
  gboolean was_fresh;
  gboolean was_checking;
  gboolean was_loading;

  if (!runtime_source_is_running(source) || !source->monitor_enabled)
    return;
  was_checking = network_sidebar_awg_runtime_source_is_checking(source);
  was_loading = network_sidebar_awg_runtime_source_is_loading(source);
  clear_source_id(&source->retry_source);
  source->retry_purpose = NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
  was_fresh = invalidate_records(source, purpose);
  schedule_refresh(source, 0);
  if (was_fresh || !was_checking ||
      was_loading != network_sidebar_awg_runtime_source_is_loading(source))
    notify_changed(source);
}

void
network_sidebar_awg_runtime_source_record_delete(
  NetworkSidebarAwgRuntimeSource *source,
  const char *name)
{
  if (source == NULL || source->stopping ||
      !network_sidebar_amneziawg_name_is_valid(name))
    return;
  for (guint i = 0; i < source->records->len; i++) {
    const NetworkSidebarAwgRuntimeRecord *record =
      g_ptr_array_index(source->records, i);

    if (g_strcmp0(record->name, name) == 0) {
      g_ptr_array_remove_index(source->records, i);
      break;
    }
  }
  network_sidebar_awg_runtime_source_request_refresh(
    source, NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE);
}

void
network_sidebar_awg_runtime_source_set_names(
  NetworkSidebarAwgRuntimeSource *source,
  const GPtrArray *names,
  NetworkSidebarAwgRefreshPurpose purpose)
{
  g_autoptr(GPtrArray) normalized = NULL;
  gboolean was_checking;
  gboolean was_loading;
  gboolean was_fresh;

  if (source == NULL || source->stopping)
    return;
  normalized = normalize_names(names);
  if (names_equal(source->names, normalized))
    return;
  was_checking = network_sidebar_awg_runtime_source_is_checking(source);
  was_loading = network_sidebar_awg_runtime_source_is_loading(source);
  g_clear_pointer(&source->names, g_ptr_array_unref);
  source->names = g_steal_pointer(&normalized);
  was_fresh = invalidate_records(source, purpose);
  if (runtime_source_is_running(source) && source->monitor_enabled) {
    clear_source_id(&source->retry_source);
    source->retry_purpose = NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
    schedule_refresh(source, 0);
  }
  if (was_fresh ||
      was_checking != network_sidebar_awg_runtime_source_is_checking(source) ||
      was_loading != network_sidebar_awg_runtime_source_is_loading(source))
    notify_changed(source);
}

GPtrArray *
network_sidebar_awg_runtime_source_dup_cached_records(
  NetworkSidebarAwgRuntimeSource *source)
{
  GPtrArray *records = g_ptr_array_new_with_free_func(
    (GDestroyNotify) network_sidebar_awg_runtime_record_free);

  if (source == NULL)
    return records;
  for (guint i = 0; i < source->records->len; i++) {
    const NetworkSidebarAwgRuntimeRecord *record =
      g_ptr_array_index(source->records, i);

    /* Saved-profile membership is supplied separately by the model. Do not
     * turn a formerly inactive profile into an unknown runtime-only row. */
    if (!source->records_fresh && !record_identifies_session(record))
      continue;
    g_ptr_array_add(records,
                    network_sidebar_awg_runtime_record_new(
                      record->name,
                      source->records_fresh ? record->state :
                        NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_UNKNOWN,
                      source->records_fresh && record->interface_exists));
  }
  return records;
}

gboolean
network_sidebar_awg_runtime_source_get_cached(
  NetworkSidebarAwgRuntimeSource *source,
  const char *name,
  NetworkSidebarAmneziaWGRuntimeState *state,
  gboolean *interface_exists)
{
  if (state != NULL)
    *state = NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_UNKNOWN;
  if (interface_exists != NULL)
    *interface_exists = FALSE;
  if (source == NULL || !source->records_fresh ||
      !network_sidebar_amneziawg_name_is_valid(name))
    return FALSE;

  for (guint i = 0; i < source->records->len; i++) {
    const NetworkSidebarAwgRuntimeRecord *record =
      g_ptr_array_index(source->records, i);

    if (strcmp(record->name, name) != 0)
      continue;
    if (state != NULL)
      *state = record->state;
    if (interface_exists != NULL)
      *interface_exists = record->interface_exists;
    return TRUE;
  }
  return FALSE;
}

static gboolean
scheduled_refresh_cb(gpointer user_data)
{
  NetworkSidebarAwgRuntimeSource *source = user_data;

  /* Retain scheduled progress until start_scan has taken ownership, including
   * any monitor-change callbacks emitted before the worker is launched. */
  if (source->scan_running)
    source->scan_queued = TRUE;
  else
    start_scan(source);
  source->monitor_debounce_source = 0;
  return G_SOURCE_REMOVE;
}

static void
schedule_monitor_refresh(NetworkSidebarAwgRuntimeSource *source)
{
  gboolean was_fresh;
  gboolean was_checking;

  if (!runtime_source_is_running(source) || !source->monitor_enabled)
    return;
  was_checking = network_sidebar_awg_runtime_source_is_checking(source);
  clear_source_id(&source->retry_source);
  source->retry_purpose = NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
  was_fresh = invalidate_records(source, automatic_refresh_purpose(source));
  schedule_refresh(source, AWG_RUNTIME_MONITOR_DEBOUNCE_MSEC);
  if (was_fresh || !was_checking)
    notify_changed(source);
}

static void
monitor_changed_cb(GFileMonitor *monitor,
                   GFile *file,
                   GFile *other_file,
                   GFileMonitorEvent event_type,
                   gpointer user_data)
{
  g_autoptr(NetworkSidebarAwgRuntimeSource) source =
    network_sidebar_awg_runtime_source_ref(user_data);
  g_autofree char *path = file != NULL ? g_file_get_path(file) : NULL;
  g_autofree char *other_path = other_file != NULL ?
    g_file_get_path(other_file) : NULL;
  gboolean fallback_event;
  gboolean monitor_removed;
  (void) monitor;

  if (!runtime_source_is_running(source) || !source->monitor_enabled)
    return;
  monitor_removed = event_type == G_FILE_MONITOR_EVENT_UNMOUNTED ||
                    ((event_type == G_FILE_MONITOR_EVENT_DELETED ||
                      event_type == G_FILE_MONITOR_EVENT_MOVED_OUT ||
                      event_type == G_FILE_MONITOR_EVENT_RENAMED) &&
                     (g_strcmp0(path, source->monitor_path) == 0 ||
                      g_strcmp0(other_path, source->monitor_path) == 0));
  fallback_event = source->monitor_expected_child != NULL &&
                   (g_strcmp0(path, source->monitor_expected_child) == 0 ||
                    g_strcmp0(other_path,
                              source->monitor_expected_child) == 0);
  if (source->monitor_expected_child != NULL && !fallback_event &&
      !monitor_removed)
    return;
  if (fallback_event || monitor_removed)
    clear_monitor(source);
  schedule_monitor_refresh(source);
}

static gboolean
link_monitor_cb(gint fd, GIOCondition condition, gpointer user_data)
{
  g_autoptr(NetworkSidebarAwgRuntimeSource) source =
    network_sidebar_awg_runtime_source_ref(user_data);
  _Alignas(struct nlmsghdr) guint8 buffer[AWG_LINK_EVENT_BUFFER_SIZE];
  gboolean failed = (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) != 0;
  gboolean changed = FALSE;
  gboolean drained = FALSE;

  for (guint i = 0; !failed && i < AWG_LINK_EVENT_DRAIN_LIMIT; i++) {
    struct sockaddr_nl sender = { 0 };
    struct iovec vector = { buffer, sizeof(buffer) };
    struct msghdr message = {
      .msg_name = &sender,
      .msg_namelen = sizeof(sender),
      .msg_iov = &vector,
      .msg_iovlen = 1,
    };
    ssize_t count = recvmsg(fd, &message, MSG_DONTWAIT | MSG_TRUNC);

    if (count > 0) {
      if (count > (ssize_t) sizeof(buffer) ||
          (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
          message.msg_namelen != sizeof(sender) ||
          sender.nl_family != AF_NETLINK || sender.nl_pid != 0) {
        failed = TRUE;
        break;
      }
      if (!source->records_fresh ||
          network_sidebar_amneziawg_link_event_filter_is_relevant(
            source->link_filter, buffer, (gsize) count))
        changed = TRUE;
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      drained = TRUE;
      break;
    }
    failed = TRUE;
  }
  if (!failed) {
    /* A bounded drain must not leave a possibly relevant event hidden behind
     * unrelated traffic while advertising a fresh cache. */
    if (changed || !drained)
      schedule_monitor_refresh(source);
    return G_SOURCE_CONTINUE;
  }

  source->link_monitor_source = 0;
  if (source->link_monitor_fd >= 0)
    close(source->link_monitor_fd);
  source->link_monitor_fd = -1;
  schedule_monitor_refresh(source);
  return G_SOURCE_REMOVE;
}

static GFileMonitor *
create_file_monitor(NetworkSidebarAwgRuntimeSource *source, const char *path)
{
  g_autoptr(GFile) directory = NULL;
  g_autoptr(GError) error = NULL;
  GFileMonitor *monitor;

  directory = g_file_new_for_path(path);
  monitor = g_file_monitor_directory(directory,
                                     G_FILE_MONITOR_NONE,
                                     source->lifecycle_cancellable,
                                     &error);
  return monitor;
}

static void
install_file_monitor(NetworkSidebarAwgRuntimeSource *source,
                     GFileMonitor *monitor,
                     const char *path,
                     const char *expected_child)
{
  clear_monitor(source);
  source->monitor = monitor;
  source->monitor_path = g_strdup(path);
  source->monitor_expected_child = g_strdup(expected_child);
  g_signal_connect(source->monitor,
                   "changed",
                   G_CALLBACK(monitor_changed_cb),
                   source);
}

static gboolean
ensure_file_monitor(NetworkSidebarAwgRuntimeSource *source, gboolean *ready)
{
  g_autofree char *parent = g_path_get_dirname(
    NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_DIR);
  g_autofree char *ancestor = g_path_get_dirname(parent);
  const char *paths[] = {
    NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_DIR,
    parent,
    ancestor,
  };
  const char *expected[] = {
    NULL,
    NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_DIR,
    parent,
  };

  if (source->monitor != NULL &&
      g_file_monitor_is_cancelled(source->monitor))
    clear_monitor(source);
  for (guint i = 0; i < G_N_ELEMENTS(paths); i++) {
    GFileMonitor *monitor;

    if (source->monitor != NULL &&
        g_strcmp0(source->monitor_path, paths[i]) == 0) {
      if (ready != NULL)
        *ready = TRUE;
      return FALSE;
    }
    monitor = create_file_monitor(source, paths[i]);
    if (monitor == NULL)
      continue;
    install_file_monitor(source, monitor, paths[i], expected[i]);
    if (ready != NULL)
      *ready = TRUE;
    return TRUE;
  }
  if (ready != NULL)
    *ready = FALSE;
  return FALSE;
}

static gboolean
ensure_link_monitor(NetworkSidebarAwgRuntimeSource *source, gboolean *ready)
{
  struct sockaddr_nl address = { 0 };
  int fd;

  if (source->link_monitor_source != 0) {
    if (ready != NULL)
      *ready = TRUE;
    return FALSE;
  }
  if (source->link_monitor_fd >= 0)
    close(source->link_monitor_fd);
  source->link_monitor_fd = -1;
  fd = socket(AF_NETLINK,
              SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
              NETLINK_ROUTE);
  if (fd < 0)
    goto unavailable;
  address.nl_family = AF_NETLINK;
  address.nl_groups = RTMGRP_LINK;
  if (bind(fd, (const struct sockaddr *) &address, sizeof(address)) != 0) {
    close(fd);
    goto unavailable;
  }
  source->link_monitor_fd = fd;
  source->link_monitor_source = g_unix_fd_add_full(
    G_PRIORITY_DEFAULT,
    fd,
    G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
    link_monitor_cb,
    source,
    NULL);
  if (source->link_monitor_source == 0) {
    close(fd);
    source->link_monitor_fd = -1;
    goto unavailable;
  }
  if (ready != NULL)
    *ready = TRUE;
  return TRUE;

unavailable:
  if (ready != NULL)
    *ready = FALSE;
  return FALSE;
}

static gboolean
ensure_monitors(NetworkSidebarAwgRuntimeSource *source, gboolean *ready)
{
  gboolean file_ready = FALSE;
  gboolean link_ready = FALSE;
  gboolean changed;

  changed = ensure_file_monitor(source, &file_ready);
  changed = ensure_link_monitor(source, &link_ready) || changed;
  if (ready != NULL)
    *ready = file_ready && link_ready;
  return changed;
}

void
network_sidebar_awg_runtime_source_ensure_monitor(
  NetworkSidebarAwgRuntimeSource *source)
{
  gboolean ready;
  gboolean was_fresh = FALSE;
  gboolean was_checking;

  if (!runtime_source_is_running(source) || !source->monitor_enabled)
    return;
  was_checking = network_sidebar_awg_runtime_source_is_checking(source);
  gboolean changed = ensure_monitors(source, &ready);
  if (changed || !ready) {
    was_fresh = invalidate_records(source, automatic_refresh_purpose(source));
    if (changed)
      schedule_refresh(source, 0);
  }
  if (!ready)
    schedule_retry(source, automatic_refresh_purpose(source));
  if (was_fresh ||
      was_checking != network_sidebar_awg_runtime_source_is_checking(source))
    notify_changed(source);
}

void
network_sidebar_awg_runtime_source_start(NetworkSidebarAwgRuntimeSource *source)
{
  if (source == NULL || source->started || source->stopping)
    return;
  source->started = TRUE;
  source->lifecycle_cancellable = g_cancellable_new();
  if (source->monitor_enabled) {
    network_sidebar_awg_runtime_source_ensure_monitor(source);
    network_sidebar_awg_runtime_source_request_refresh(
      source, NETWORK_SIDEBAR_AWG_REFRESH_VISIBLE);
  }
}

void
network_sidebar_awg_runtime_source_stop(NetworkSidebarAwgRuntimeSource *source)
{
  if (source != NULL)
    runtime_source_stop_internal(source);
}

void
network_sidebar_awg_runtime_source_set_monitor_enabled(
  NetworkSidebarAwgRuntimeSource *source,
  gboolean enabled)
{
  if (source == NULL || source->monitor_enabled == enabled)
    return;
  gboolean was_checking = network_sidebar_awg_runtime_source_is_checking(source);
  source->monitor_enabled = enabled;
  if (!enabled) {
    clear_source_id(&source->monitor_debounce_source);
    clear_source_id(&source->retry_source);
    clear_monitor(source);
    clear_link_monitor(source);
    source->refresh_generation = source->refresh_generation == G_MAXUINT64 ?
      1 : source->refresh_generation + 1;
    source->records_fresh = FALSE;
    source->refresh_pending = FALSE;
    source->scan_queued = FALSE;
    source->retry_attempt = 0;
    source->queued_purpose = NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
    source->scan_purpose = NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
    source->retry_purpose = NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
    source->mutation_active = FALSE;
    source->reconciliation_active = FALSE;
    if (was_checking)
      notify_changed(source);
    return;
  }
  network_sidebar_awg_runtime_source_ensure_monitor(source);
  network_sidebar_awg_runtime_source_request_refresh(
    source, NETWORK_SIDEBAR_AWG_REFRESH_VISIBLE);
}
