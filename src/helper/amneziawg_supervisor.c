#define _GNU_SOURCE

#include "helper/amneziawg_supervisor.h"

#include "helper/amneziawg_supervision_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_SUPERVISED_GROUPS 8u
#define SENTINEL_FD_FALLBACK_LIMIT (1u << 20)
#define SENTINEL_READY_TIMEOUT_MSEC 2000
#define SENTINEL_SETUP_REAP_TIMEOUT_MSEC 100

typedef struct {
  pid_t process_group;
  pid_t sentinel;
  gboolean pending_release;
  gboolean helper_terminating;
  gboolean terminal_signal_sent;
} SupervisedGroup;

typedef enum {
  GROUP_SCAN_FAILED = -1,
  GROUP_SCAN_EMPTY,
  GROUP_SCAN_LIVE,
} GroupScanResult;

struct _NetworkSidebarAwgSupervisor {
  GHashTable *owned_groups;
  GArray *retired_sentinels;
  int fd;
  gboolean allow_release;
  gboolean channel_closed;
  gboolean failed;
  gboolean cleanup_started;
  gboolean group_released;
  gboolean release_cancelled;
};

static volatile sig_atomic_t sentinel_expected_parent = -1;

static void
close_fd(int *fd)
{
  if (*fd >= 0)
    close(*fd);
  *fd = -1;
}

static void
kill_and_reap_setup_child(pid_t child)
{
  gint64 deadline = g_get_monotonic_time() +
    SENTINEL_SETUP_REAP_TIMEOUT_MSEC * 1000;

  kill(child, SIGKILL);
  while (g_get_monotonic_time() < deadline) {
    pid_t waited = waitpid(child, NULL, WNOHANG);

    if (waited == child || (waited < 0 && errno == ECHILD))
      return;
    if (waited < 0 && errno != EINTR)
      return;
    g_usleep(1000);
  }
}

static gboolean
set_nonblocking(int fd)
{
  int flags = fcntl(fd, F_GETFL);

  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static gboolean
close_sentinel_fds(int ready_write, rlim_t fd_limit)
{
  if (ready_write != STDERR_FILENO + 1) {
    if (dup2(ready_write, STDERR_FILENO + 1) < 0)
      return FALSE;
    close(ready_write);
  }
#ifdef SYS_close_range
  if (syscall(SYS_close_range, STDERR_FILENO + 2, UINT_MAX, 0) == 0)
    return TRUE;
#endif
  if (fd_limit == RLIM_INFINITY || fd_limit > SENTINEL_FD_FALLBACK_LIMIT)
    return FALSE;
  for (int fd = STDERR_FILENO + 2; fd < (int) fd_limit; fd++)
    close(fd);
  return TRUE;
}

static gboolean
get_sentinel_fd_limit(rlim_t *fd_limit)
{
  g_autoptr(GDir) descriptors = NULL;
  struct rlimit limits;
  const char *entry;

  if (fd_limit == NULL || getrlimit(RLIMIT_NOFILE, &limits) != 0)
    return FALSE;
  descriptors = g_dir_open("/proc/self/fd", 0, NULL);
  if (descriptors == NULL)
    return FALSE;
  *fd_limit = limits.rlim_max;
  for (;;) {
    guint64 descriptor;

    errno = 0;
    entry = g_dir_read_name(descriptors);
    if (entry == NULL)
      return errno == 0;
    if (entry[0] == '\0')
      continue;
    for (const char *character = entry; *character != '\0'; character++) {
      if (!g_ascii_isdigit(*character))
        goto next_descriptor;
    }
    descriptor = g_ascii_strtoull(entry, NULL, 10);
    if (*fd_limit != RLIM_INFINITY && descriptor >= *fd_limit)
      *fd_limit = descriptor < G_MAXUINT64 ? descriptor + 1 : RLIM_INFINITY;

next_descriptor:
    continue;
  }
}

static void
sentinel_parent_death_handler(int signal_number)
{
  (void) signal_number;
  if (getppid() == (pid_t) sentinel_expected_parent)
    return;
  kill(0, SIGKILL);
  _exit(127);
}

static void
sentinel_child(pid_t expected_parent,
               pid_t process_group,
               int ready_read,
               int ready_write,
               rlim_t fd_limit)
{
  struct sigaction ignore_action = { 0 };
  struct sigaction death_action = { 0 };
  sigset_t unblocked;
  const guint8 ready = 1;

  close(ready_read);
  if (setpgid(0, process_group) != 0)
    _exit(127);
  ignore_action.sa_handler = SIG_IGN;
  sigemptyset(&ignore_action.sa_mask);
  death_action.sa_handler = sentinel_parent_death_handler;
  sigemptyset(&death_action.sa_mask);
  sigaddset(&death_action.sa_mask, SIGCONT);
  sentinel_expected_parent = (sig_atomic_t) expected_parent;
  sigemptyset(&unblocked);
  if (sigaction(SIGTERM, &ignore_action, NULL) != 0 ||
      sigaction(SIGINT, &ignore_action, NULL) != 0 ||
      sigaction(SIGHUP, &ignore_action, NULL) != 0 ||
      sigaction(SIGCONT, &death_action, NULL) != 0 ||
      sigprocmask(SIG_SETMASK, &unblocked, NULL) != 0 ||
      prctl(PR_SET_PDEATHSIG, SIGCONT, 0, 0, 0) != 0 ||
      getppid() != expected_parent ||
      !close_sentinel_fds(ready_write, fd_limit)) {
    kill(0, SIGKILL);
    _exit(127);
  }
  ready_write = STDERR_FILENO + 1;
  if (write(ready_write, &ready, sizeof(ready)) != (ssize_t) sizeof(ready)) {
    kill(0, SIGKILL);
    _exit(127);
  }
  close(ready_write);
  for (;;)
    pause();
}

static pid_t
spawn_sentinel(pid_t process_group)
{
  int ready[2] = { -1, -1 };
  const pid_t expected_parent = getpid();
  rlim_t fd_limit;
  guint8 response = 0;
  struct pollfd readiness;
  ssize_t count;
  pid_t sentinel;

  if (pipe2(ready, O_CLOEXEC) != 0)
    return -1;
  if (!get_sentinel_fd_limit(&fd_limit)) {
    close_fd(&ready[0]);
    close_fd(&ready[1]);
    return -1;
  }
  sentinel = fork();
  if (sentinel < 0) {
    close_fd(&ready[0]);
    close_fd(&ready[1]);
    return -1;
  }
  if (sentinel == 0)
    sentinel_child(expected_parent,
                   process_group,
                   ready[0],
                   ready[1],
                   fd_limit);

  close_fd(&ready[1]);
  readiness = (struct pollfd) { ready[0], POLLIN | POLLHUP, 0 };
  do
    count = poll(&readiness, 1, SENTINEL_READY_TIMEOUT_MSEC);
  while (count < 0 && errno == EINTR);
  if (count > 0) {
    do
      count = read(ready[0], &response, sizeof(response));
    while (count < 0 && errno == EINTR);
  }
  close_fd(&ready[0]);
  if (count != (ssize_t) sizeof(response) || response != 1 ||
      getpgid(sentinel) != process_group) {
    kill_and_reap_setup_child(sentinel);
    return -1;
  }
  return sentinel;
}

static gboolean
reap_sentinel(SupervisedGroup *group)
{
  pid_t waited;

  if (group->sentinel <= 0)
    return TRUE;
  kill(group->sentinel, SIGKILL);
  do
    waited = waitpid(group->sentinel, NULL, WNOHANG);
  while (waited < 0 && errno == EINTR);
  if (waited == group->sentinel || (waited < 0 && errno == ECHILD))
    group->sentinel = -1;
  return group->sentinel <= 0;
}

static gboolean
send_response(NetworkSidebarAwgSupervisor *supervisor,
              NetworkSidebarAmneziaWGSupervisionMessage *message,
              int status)
{
  ssize_t written;

  message->status = status;
  do
    written = send(supervisor->fd,
                   message,
                   sizeof(*message),
                   MSG_DONTWAIT | MSG_NOSIGNAL);
  while (written < 0 && errno == EINTR);
  return written == (ssize_t) sizeof(*message);
}

static GroupScanResult
scan_process_group(const SupervisedGroup *group)
{
  g_autoptr(GDir) proc = g_dir_open("/proc", 0, NULL);

  if (proc == NULL)
    return GROUP_SCAN_FAILED;
  for (;;) {
    const char *entry;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    char *closing_parenthesis;
    char state;
    long parent;
    long process_group;
    gint64 process;

    errno = 0;
    entry = g_dir_read_name(proc);
    if (entry == NULL)
      return errno == 0 ? GROUP_SCAN_EMPTY : GROUP_SCAN_FAILED;
    if (entry[0] == '\0')
      continue;
    for (const char *character = entry; *character != '\0'; character++) {
      if (!g_ascii_isdigit(*character))
        goto next_entry;
    }
    process = g_ascii_strtoll(entry, NULL, 10);
    if (process <= 0 || process > G_MAXINT || process == group->sentinel)
      continue;
    path = g_build_filename("/proc", entry, "stat", NULL);
    if (!g_file_get_contents(path, &contents, NULL, &error)) {
      if (kill((pid_t) process, 0) != 0 && errno == ESRCH)
        continue;
      return GROUP_SCAN_FAILED;
    }
    closing_parenthesis = strrchr(contents, ')');
    if (closing_parenthesis == NULL ||
        sscanf(closing_parenthesis + 2,
               "%c %ld %ld",
               &state,
               &parent,
               &process_group) != 3) {
      if (kill((pid_t) process, 0) != 0 && errno == ESRCH)
        continue;
      return GROUP_SCAN_FAILED;
    }
    if (process_group == group->process_group && state != 'Z' && state != 'X')
      return GROUP_SCAN_LIVE;

next_entry:
    continue;
  }
}

static void
reap_retired_sentinels(NetworkSidebarAwgSupervisor *supervisor)
{
  for (guint i = 0; i < supervisor->retired_sentinels->len;) {
    pid_t sentinel = g_array_index(supervisor->retired_sentinels, pid_t, i);
    pid_t waited;

    do
      waited = waitpid(sentinel, NULL, WNOHANG);
    while (waited < 0 && errno == EINTR);
    if (waited == sentinel || (waited < 0 && errno == ECHILD))
      g_array_remove_index_fast(supervisor->retired_sentinels, i);
    else
      i++;
  }
}

static gboolean
unregister_group(NetworkSidebarAwgSupervisor *supervisor,
                  gpointer group_key,
                  SupervisedGroup *group)
{
  if (group->sentinel > 0 && kill(group->sentinel, SIGKILL) != 0 &&
      errno != ESRCH)
    return FALSE;

  /* Ownership ends before the ACK; sentinel exit is independent bookkeeping. */
  g_hash_table_steal(supervisor->owned_groups, group_key);
  if (group->pending_release)
    supervisor->group_released = FALSE;
  if (group->sentinel > 0)
    g_array_append_val(supervisor->retired_sentinels, group->sentinel);
  g_free(group);
  reap_retired_sentinels(supervisor);
  return TRUE;
}

static gboolean
sentinels_are_safe(NetworkSidebarAwgSupervisor *supervisor)
{
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init(&iter, supervisor->owned_groups);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    SupervisedGroup *group = value;
    siginfo_t child_info = { 0 };
    int wait_result;

    if (group->sentinel <= 0) {
      if (!group->terminal_signal_sent)
        return FALSE;
      continue;
    }
    do
      wait_result = waitid(P_PID,
                           (id_t) group->sentinel,
                           &child_info,
                           WEXITED | WNOHANG | WNOWAIT);
    while (wait_result != 0 && errno == EINTR);
    if (wait_result != 0)
      return FALSE;
    if (child_info.si_pid != group->sentinel)
      continue;
    if (group->terminal_signal_sent || group->helper_terminating)
      continue;
    if (group->pending_release || scan_process_group(group) != GROUP_SCAN_EMPTY)
      return FALSE;
  }
  return TRUE;
}

static gboolean
handle_message(NetworkSidebarAwgSupervisor *supervisor,
               NetworkSidebarAmneziaWGSupervisionMessage *message)
{
  pid_t process_group = (pid_t) message->process_group;
  gpointer group_key = GINT_TO_POINTER(message->process_group);
  SupervisedGroup *group = g_hash_table_lookup(supervisor->owned_groups,
                                                group_key);
  GroupScanResult group_state;
  int status = 0;
  gboolean recoverable_rejection = FALSE;

  if (message->magic != NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_MAGIC ||
      message->version != NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_VERSION ||
      message->sequence == 0 || message->status != 0 || process_group <= 0)
    return FALSE;

  switch ((NetworkSidebarAmneziaWGSupervisionType) message->type) {
  case NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_REGISTER:
    /* Retired sentinels consume child slots, but never block RELEASE. */
    reap_retired_sentinels(supervisor);
    if (supervisor->cleanup_started || supervisor->group_released) {
      status = ESHUTDOWN;
    } else if (group != NULL) {
      status = EEXIST;
    } else if (g_hash_table_size(supervisor->owned_groups) +
                 supervisor->retired_sentinels->len >=
               MAX_SUPERVISED_GROUPS) {
      status = ENOSPC;
    } else {
      group = g_new0(SupervisedGroup, 1);
      group->process_group = process_group;
      group->sentinel = spawn_sentinel(process_group);
      if (group->sentinel <= 0) {
        g_free(group);
        status = EIO;
      } else {
        g_hash_table_insert(supervisor->owned_groups, group_key, group);
      }
    }
    break;
  case NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_UNREGISTER:
    if (group == NULL) {
      status = ENOENT;
    } else {
      group_state = scan_process_group(group);
      if (group_state == GROUP_SCAN_FAILED)
        status = EIO;
      else if (group_state == GROUP_SCAN_LIVE)
        status = EBUSY;
      else if (!unregister_group(supervisor, group_key, group))
        status = EIO;
    }
    break;
  case NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_TERMINATING:
    if (group == NULL) {
      status = ENOENT;
    } else if (!sentinels_are_safe(supervisor)) {
      /* A notification cannot retroactively excuse an unexpected death. */
      status = EIO;
    } else {
      /* Keep the sentinel waitable and the group owned until UNREGISTER or
       * supervisor cleanup. This intent is not evidence that SIGKILL was sent. */
      group->helper_terminating = TRUE;
      if (group->pending_release) {
        group->pending_release = FALSE;
        supervisor->group_released = FALSE;
      }
    }
    break;
  case NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_RELEASE:
    if (supervisor->cleanup_started || supervisor->group_released) {
      status = EPERM;
    } else if (supervisor->release_cancelled) {
      status = ECANCELED;
      recoverable_rejection = TRUE;
    } else if (!supervisor->allow_release) {
      status = EPERM;
    } else if (group == NULL) {
      status = ENOENT;
    } else if (group->helper_terminating || group->terminal_signal_sent) {
      status = EPERM;
    } else if (g_hash_table_size(supervisor->owned_groups) != 1) {
      status = EBUSY;
    } else if ((group_state = scan_process_group(group)) == GROUP_SCAN_FAILED) {
      status = EIO;
    } else if (group_state != GROUP_SCAN_LIVE) {
      status = ENOTSUP;
    } else {
      group->pending_release = TRUE;
      supervisor->group_released = TRUE;
    }
    break;
  case NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_WITHDRAW_RELEASE:
    if (supervisor->cleanup_started || group == NULL ||
        !group->pending_release || !supervisor->group_released ||
        group->terminal_signal_sent) {
      status = EPERM;
    } else {
      /* Cancellation may disable future release while this ACK is in flight.
       * Reopen registration for rollback without releasing any ownership or
       * undoing cancellation/hard-cleanup decisions. */
      group->pending_release = FALSE;
      supervisor->group_released = FALSE;
    }
    break;
  default:
    return FALSE;
  }

  if (!send_response(supervisor, message, status))
    return FALSE;
  return status == 0 || recoverable_rejection;
}

NetworkSidebarAwgSupervisor *
network_sidebar_awg_supervisor_new(int fd, gboolean allow_release)
{
  NetworkSidebarAwgSupervisor *supervisor;

  if (fd < 0 || !set_nonblocking(fd))
    return NULL;
  supervisor = g_new0(NetworkSidebarAwgSupervisor, 1);
  supervisor->owned_groups = g_hash_table_new_full(g_direct_hash,
                                                    g_direct_equal,
                                                    NULL,
                                                    g_free);
  supervisor->retired_sentinels = g_array_new(FALSE, FALSE, sizeof(pid_t));
  supervisor->fd = fd;
  supervisor->allow_release = allow_release;
  return supervisor;
}

void
network_sidebar_awg_supervisor_free(NetworkSidebarAwgSupervisor *supervisor)
{
  GHashTableIter iter;
  gpointer value;

  if (supervisor == NULL)
    return;
  if (supervisor->owned_groups != NULL &&
      g_hash_table_size(supervisor->owned_groups) > 0) {
    network_sidebar_awg_supervisor_signal_groups(supervisor, SIGKILL);
    g_hash_table_iter_init(&iter, supervisor->owned_groups);
    while (g_hash_table_iter_next(&iter, NULL, &value))
      reap_sentinel(value);
  }
  reap_retired_sentinels(supervisor);
  g_warn_if_fail(supervisor->retired_sentinels->len == 0);
  close_fd(&supervisor->fd);
  g_clear_pointer(&supervisor->owned_groups, g_hash_table_unref);
  g_clear_pointer(&supervisor->retired_sentinels, g_array_unref);
  g_free(supervisor);
}

gboolean
network_sidebar_awg_supervisor_poll(NetworkSidebarAwgSupervisor *supervisor)
{
  if (supervisor == NULL)
    return FALSE;
  reap_retired_sentinels(supervisor);
  if (supervisor->failed)
    return FALSE;
  if (supervisor->channel_closed)
    return TRUE;

  for (;;) {
    NetworkSidebarAmneziaWGSupervisionMessage message;
    ssize_t count;

    do
      count = recv(supervisor->fd,
                   &message,
                   sizeof(message),
                   MSG_DONTWAIT | MSG_TRUNC);
    while (count < 0 && errno == EINTR);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      reap_retired_sentinels(supervisor);
      if (sentinels_are_safe(supervisor))
        return TRUE;
      supervisor->cleanup_started = TRUE;
      network_sidebar_awg_supervisor_signal_groups(supervisor, SIGKILL);
      supervisor->failed = TRUE;
      close_fd(&supervisor->fd);
      supervisor->channel_closed = TRUE;
      return FALSE;
    }
    if (count == 0) {
      if (!sentinels_are_safe(supervisor)) {
        supervisor->cleanup_started = TRUE;
        network_sidebar_awg_supervisor_signal_groups(supervisor, SIGKILL);
        supervisor->failed = TRUE;
      }
      close_fd(&supervisor->fd);
      supervisor->channel_closed = TRUE;
      return !supervisor->failed;
    }
    if (count != (ssize_t) sizeof(message) ||
        !handle_message(supervisor, &message)) {
      supervisor->failed = TRUE;
      close_fd(&supervisor->fd);
      supervisor->channel_closed = TRUE;
      return FALSE;
    }
  }
}

void
network_sidebar_awg_supervisor_disable_release(
  NetworkSidebarAwgSupervisor *supervisor)
{
  if (supervisor != NULL) {
    supervisor->allow_release = FALSE;
    supervisor->release_cancelled = TRUE;
  }
}

gboolean
network_sidebar_awg_supervisor_release_pending(
  const NetworkSidebarAwgSupervisor *supervisor)
{
  return supervisor != NULL && supervisor->group_released;
}

void
network_sidebar_awg_supervisor_begin_cleanup(
  NetworkSidebarAwgSupervisor *supervisor)
{
  if (supervisor != NULL)
    supervisor->cleanup_started = TRUE;
}

void
network_sidebar_awg_supervisor_signal_groups(
  NetworkSidebarAwgSupervisor *supervisor,
  int signal_number)
{
  GHashTableIter iter;
  gpointer value;

  if (supervisor == NULL)
    return;
  g_hash_table_iter_init(&iter, supervisor->owned_groups);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    SupervisedGroup *group = value;

    if (group->terminal_signal_sent)
      continue;
    if (kill(-group->process_group, signal_number) == 0 &&
        signal_number == SIGKILL)
      group->terminal_signal_sent = TRUE;
  }
}

void
network_sidebar_awg_supervisor_poll_cleanup(
  NetworkSidebarAwgSupervisor *supervisor)
{
  GHashTableIter iter;
  gpointer value;

  if (supervisor == NULL)
    return;
  reap_retired_sentinels(supervisor);
  if (!supervisor->cleanup_started)
    return;
  g_hash_table_iter_init(&iter, supervisor->owned_groups);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    SupervisedGroup *group = value;
    int ignored_status;
    pid_t waited;

    if (!group->terminal_signal_sent)
      continue;
    do
      waited = waitpid(-group->process_group, &ignored_status, WNOHANG);
    while (waited < 0 && errno == EINTR);
    while (waited > 0) {
      if (waited == group->sentinel)
        group->sentinel = -1;
      do
        waited = waitpid(-group->process_group,
                         &ignored_status,
                         WNOHANG);
      while (waited < 0 && errno == EINTR);
    }
    if (waited < 0 && errno == ECHILD)
      g_hash_table_iter_remove(&iter);
  }
}

void
network_sidebar_awg_supervisor_detach_retired_sentinels(
  NetworkSidebarAwgSupervisor *supervisor,
  GArray *child_pids)
{
  if (supervisor == NULL || child_pids == NULL)
    return;
  reap_retired_sentinels(supervisor);
  g_array_append_vals(child_pids,
                       supervisor->retired_sentinels->data,
                       supervisor->retired_sentinels->len);
  g_array_set_size(supervisor->retired_sentinels, 0);
}

void
network_sidebar_awg_supervisor_detach_terminated_groups(
  NetworkSidebarAwgSupervisor *supervisor,
  GArray *process_groups)
{
  GHashTableIter iter;
  gpointer value;

  if (supervisor == NULL || process_groups == NULL ||
      !supervisor->cleanup_started)
    return;
  g_hash_table_iter_init(&iter, supervisor->owned_groups);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    SupervisedGroup *group = value;
    pid_t process_group;

    if (!group->terminal_signal_sent)
      continue;
    process_group = group->process_group;
    g_hash_table_iter_steal(&iter);
    reap_sentinel(group);
    g_free(group);
    g_array_append_val(process_groups, process_group);
  }
}

pid_t
network_sidebar_awg_supervisor_finalize_release(
  NetworkSidebarAwgSupervisor *supervisor)
{
  GHashTableIter iter;
  gpointer group_key;
  gpointer value;
  SupervisedGroup *group;
  pid_t process_group;

  if (supervisor == NULL || supervisor->cleanup_started ||
      g_hash_table_size(supervisor->owned_groups) != 1)
    return -1;
  g_hash_table_iter_init(&iter, supervisor->owned_groups);
  if (!g_hash_table_iter_next(&iter, &group_key, &value))
    return -1;
  group = value;
  if (!group->pending_release || group->helper_terminating ||
      group->terminal_signal_sent)
    return -1;
  process_group = group->process_group;
  g_hash_table_steal(supervisor->owned_groups, group_key);
  reap_sentinel(group);
  g_free(group);
  return process_group;
}

void
network_sidebar_awg_supervisor_abort(NetworkSidebarAwgSupervisor *supervisor)
{
  if (supervisor == NULL)
    return;
  supervisor->failed = TRUE;
  close_fd(&supervisor->fd);
  supervisor->channel_closed = TRUE;
}

gboolean
network_sidebar_awg_supervisor_channel_closed(
  const NetworkSidebarAwgSupervisor *supervisor)
{
  return supervisor != NULL && supervisor->channel_closed;
}

gboolean
network_sidebar_awg_supervisor_failed(
  const NetworkSidebarAwgSupervisor *supervisor)
{
  return supervisor == NULL || supervisor->failed;
}

gboolean
network_sidebar_awg_supervisor_groups_empty(
  const NetworkSidebarAwgSupervisor *supervisor)
{
  return supervisor != NULL &&
         g_hash_table_size(supervisor->owned_groups) == 0;
}
