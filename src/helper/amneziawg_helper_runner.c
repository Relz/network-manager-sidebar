#include "helper/amneziawg_helper_runner.h"

#include "amneziawg/deadline.h"
#include "core/config.h"
#include "helper/amneziawg_supervision_protocol.h"
#include "helper/amneziawg_supervisor.h"

#include <gio/gio.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define HELPER_TERMINATION_GRACE_MSEC 2000
#define HELPER_CLEANUP_REAP_GRACE_MSEC 200
#define GROUP_CLEANUP_TIMEOUT_MSEC 2000
#define HELPER_POLL_INTERVAL_MSEC 25
#define RELEASED_GROUP_REAP_INTERVAL_SECONDS 1

typedef enum {
  RUN_STOP_NONE,
  RUN_STOP_TIMEOUT,
  RUN_STOP_SHUTDOWN,
  RUN_STOP_IO_FAILURE,
  RUN_STOP_PREEMPTED,
} RunStopReason;

typedef struct {
  NetworkSidebarAwgHelperRunner *runner;
  char *operation;
  char *name;
  gboolean profile_report;
  GBytes *config;
  pid_t helper_pid;
  int helper_stdin_fd;
  int helper_stdout_fd;
  gsize helper_input_offset;
  GByteArray *helper_output;
  NetworkSidebarAwgSupervisor *supervisor;
  gboolean helper_output_done;
  gboolean helper_input_done;
  gboolean helper_reaped;
  gboolean group_cleanup_done;
  gboolean helper_cleanup_kill_requested;
  gboolean helper_force_kill_sent;
  gboolean helper_io_failed;
  NetworkSidebarAmneziaWGHelperExit helper_status;
  guint helper_source;
  NetworkSidebarAmneziaWGDeadlines deadlines;
  gint64 force_kill_deadline;
  gint64 group_cleanup_deadline;
  gint64 supervisor_close_observed_at;
  gint64 helper_exit_observed_at;
  gboolean forward_timeout_sent;
  gboolean result_sent;
  RunStopReason stop_reason;
  guint capabilities;
} AwgHelperRun;

struct _NetworkSidebarAwgHelperRunner {
  NetworkSidebarAwgHelperRunnerCallbacks callbacks;
  gpointer user_data;
  AwgHelperRun *run;
  GHashTable *released_groups;
  GHashTable *pending_child_reaps;
  guint reap_source;
  gboolean failed;
};

typedef struct {
  guint8 *data;
  gsize length;
} SecretBuffer;

static void begin_stop(AwgHelperRun *run, RunStopReason reason);

static gint64
monotonic_msec(void)
{
  return g_get_monotonic_time() / 1000;
}

static void
close_fd(int *fd)
{
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

static void
secret_buffer_free(gpointer user_data)
{
  SecretBuffer *secret = user_data;

  network_sidebar_amneziawg_secret_free(secret->data, secret->length);
  g_free(secret);
}

static GBytes *
secret_bytes_copy(const guint8 *data, gsize length)
{
  SecretBuffer *secret = g_new0(SecretBuffer, 1);

  secret->length = length;
  secret->data = g_memdup2(data, length);
  return g_bytes_new_with_free_func(secret->data, secret->length,
                                    secret_buffer_free, secret);
}

static gboolean
executable_is_trusted(const char *path)
{
  struct stat status;
  int fd;
  gboolean trusted;

  if (path == NULL || path[0] != '/')
    return FALSE;
  fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return FALSE;
  trusted = fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
            status.st_uid == 0 && status.st_gid == 0 && status.st_nlink == 1 &&
            (status.st_mode & 0111) != 0 && (status.st_mode & 0022) == 0;
  close(fd);
  return trusted;
}

static guint
helper_capabilities(void)
{
  static const char *const awg_quick_paths[] = {
    "/usr/bin/awg-quick", "/usr/sbin/awg-quick", "/bin/awg-quick", "/sbin/awg-quick",
  };
  guint capabilities = NETWORK_SIDEBAR_AMNEZIAWG_CAPABILITY_NONE;

  if (executable_is_trusted(NETWORK_SIDEBAR_INSTALLED_AWG_HELPER_PATH))
    capabilities |= NETWORK_SIDEBAR_AMNEZIAWG_CAPABILITY_HELPER;
  for (gsize i = 0; i < G_N_ELEMENTS(awg_quick_paths); i++) {
    if (executable_is_trusted(awg_quick_paths[i])) {
      capabilities |= NETWORK_SIDEBAR_AMNEZIAWG_CAPABILITY_AWG_QUICK;
      break;
    }
  }
  return capabilities;
}

static void
notify_changed(NetworkSidebarAwgHelperRunner *runner)
{
  if (runner->callbacks.changed != NULL)
    runner->callbacks.changed(runner->user_data);
}

static void
mark_failed(NetworkSidebarAwgHelperRunner *runner)
{
  if (runner->failed)
    return;
  runner->failed = TRUE;
  notify_changed(runner);
}

static void
reap_pending_children(NetworkSidebarAwgHelperRunner *runner)
{
  GHashTableIter iter;
  gpointer key;

  g_hash_table_iter_init(&iter, runner->pending_child_reaps);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    pid_t child_pid = GPOINTER_TO_INT(key);
    pid_t waited;

    do
      waited = waitpid(child_pid, NULL, WNOHANG);
    while (waited < 0 && errno == EINTR);
    if (waited == child_pid || (waited < 0 && errno == ECHILD))
      g_hash_table_iter_remove(&iter);
  }
}

static void
run_free(AwgHelperRun *run)
{
  if (run->helper_source != 0)
    g_source_remove(run->helper_source);
  close_fd(&run->helper_stdin_fd);
  close_fd(&run->helper_stdout_fd);
  if (run->supervisor != NULL) {
    g_autoptr(GArray) child_pids = g_array_new(FALSE, FALSE, sizeof(pid_t));

    network_sidebar_awg_supervisor_detach_retired_sentinels(run->supervisor, child_pids);
    for (guint i = 0; i < child_pids->len; i++)
      g_hash_table_add(run->runner->pending_child_reaps,
                       GINT_TO_POINTER(g_array_index(child_pids, pid_t, i)));
  }
  g_clear_pointer(&run->supervisor, network_sidebar_awg_supervisor_free);
  g_clear_pointer(&run->helper_output, g_byte_array_unref);
  g_clear_pointer(&run->config, g_bytes_unref);
  g_free(run->operation);
  g_free(run->name);
  g_free(run);
}

static NetworkSidebarAmneziaWGHelperExit
effective_status(AwgHelperRun *run)
{
  NetworkSidebarAmneziaWGHelperExit status = run->helper_status;

  if (run->helper_io_failed && run->stop_reason == RUN_STOP_NONE)
    status = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED;
  if (run->stop_reason == RUN_STOP_PREEMPTED)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SIGNALED;
  if (status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ROLLBACK_FAILED ||
      status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_ROLLBACK_FAILED)
    return status;
  if (run->stop_reason == RUN_STOP_TIMEOUT || run->forward_timeout_sent)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_TIMED_OUT;
  if (run->stop_reason == RUN_STOP_SHUTDOWN)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SIGNALED;
  if (run->stop_reason == RUN_STOP_IO_FAILURE)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED;
  return status;
}

static void
publish_result(AwgHelperRun *run)
{
  g_autoptr(NetworkSidebarAwgProfileReport) report = NULL;
  NetworkSidebarAwgHelperResult result = {
    .status = effective_status(run),
    .capabilities = run->capabilities,
  };

  if (run->result_sent)
    return;
  if (run->profile_report &&
      (result.status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS ||
       result.status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVENTORY_PARTIAL)) {
    if (run->helper_output != NULL)
      report = network_sidebar_awg_profile_report_decode(
        run->helper_output->data, run->helper_output->len, result.status);
    if (report == NULL)
      result.status = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED;
  }
  result.profiles = report;
  run->result_sent = TRUE;
  if (run->runner->callbacks.result != NULL)
    run->runner->callbacks.result(&result, run->runner->user_data);
}

static void
finish_run(AwgHelperRun *run)
{
  NetworkSidebarAwgHelperRunner *runner = run->runner;

  publish_result(run);
  run_free(run);
  runner->run = NULL;
  /* A result can precede this point by seconds. Retired exact-PID obligations
   * are transferred before observers can release their admission slot. */
  reap_pending_children(runner);
  notify_changed(runner);
}

static void
signal_helper(AwgHelperRun *run, int signal_number)
{
  if (run->helper_pid <= 0 || run->helper_reaped)
    return;
  if (kill(-run->helper_pid, signal_number) != 0 && errno == ESRCH)
    kill(run->helper_pid, signal_number);
}

static void
begin_supervisor_cleanup(AwgHelperRun *run, int signal_number)
{
  if (signal_number == SIGKILL && run->group_cleanup_deadline == 0)
    run->group_cleanup_deadline = network_sidebar_amneziawg_deadline_cap(
      monotonic_msec(), run->deadlines.hard_at, GROUP_CLEANUP_TIMEOUT_MSEC);
  if (run->supervisor == NULL) {
    run->group_cleanup_done = TRUE;
    return;
  }
  network_sidebar_awg_supervisor_begin_cleanup(run->supervisor);
  network_sidebar_awg_supervisor_signal_groups(run->supervisor, signal_number);
  run->group_cleanup_done = network_sidebar_awg_supervisor_groups_empty(run->supervisor);
}

static void
refresh_supervisor_cleanup(AwgHelperRun *run)
{
  if (run->supervisor == NULL) {
    run->group_cleanup_done = TRUE;
    return;
  }
  network_sidebar_awg_supervisor_poll_cleanup(run->supervisor);
  run->group_cleanup_done = network_sidebar_awg_supervisor_groups_empty(run->supervisor);
}

static void
track_released_group(NetworkSidebarAwgHelperRunner *runner, pid_t process_group)
{
  pid_t waited;

  do
    waited = waitpid(-process_group, NULL, WNOHANG);
  while (waited < 0 && errno == EINTR);
  while (waited > 0) {
    do
      waited = waitpid(-process_group, NULL, WNOHANG);
    while (waited < 0 && errno == EINTR);
  }
  if (waited == 0)
    g_hash_table_add(runner->released_groups, GINT_TO_POINTER(process_group));
  else if (waited < 0 && errno != ECHILD)
    mark_failed(runner);
}

static void
detach_stalled_group_cleanup(AwgHelperRun *run)
{
  g_autoptr(GArray) process_groups = g_array_new(FALSE, FALSE, sizeof(pid_t));

  network_sidebar_awg_supervisor_detach_terminated_groups(run->supervisor, process_groups);
  for (guint i = 0; i < process_groups->len; i++)
    track_released_group(run->runner, g_array_index(process_groups, pid_t, i));
  run->group_cleanup_done = network_sidebar_awg_supervisor_groups_empty(run->supervisor);
}

static void
complete_helper_supervision(AwgHelperRun *run)
{
  gboolean clean_exit;
  gint64 now;

  if (!run->helper_reaped || run->group_cleanup_done)
    return;
  if (run->supervisor == NULL) {
    run->group_cleanup_done = TRUE;
    return;
  }
  if (!network_sidebar_awg_supervisor_channel_closed(run->supervisor)) {
    now = monotonic_msec();
    if (run->helper_exit_observed_at == 0) {
      run->helper_exit_observed_at = now;
      return;
    }
    if (now - run->helper_exit_observed_at < 100)
      return;
    begin_stop(run, RUN_STOP_IO_FAILURE);
    run->helper_io_failed = TRUE;
    network_sidebar_awg_supervisor_abort(run->supervisor);
  }
  clean_exit = run->stop_reason == RUN_STOP_NONE && !run->helper_io_failed &&
               run->helper_status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS &&
               !network_sidebar_awg_supervisor_failed(run->supervisor);
  if (clean_exit) {
    pid_t released_group = network_sidebar_awg_supervisor_finalize_release(run->supervisor);

    if (released_group > 0)
      track_released_group(run->runner, released_group);
  }
  if (network_sidebar_awg_supervisor_groups_empty(run->supervisor)) {
    run->group_cleanup_done = TRUE;
    return;
  }
  if (run->stop_reason == RUN_STOP_NONE)
    run->helper_io_failed = TRUE;
  begin_supervisor_cleanup(run, SIGKILL);
  refresh_supervisor_cleanup(run);
  if (!run->profile_report && !run->group_cleanup_done &&
      run->group_cleanup_deadline > 0 && monotonic_msec() >= run->group_cleanup_deadline)
    detach_stalled_group_cleanup(run);
}

static void
begin_stop(AwgHelperRun *run, RunStopReason reason)
{
  if (run->stop_reason == RUN_STOP_NONE)
    run->stop_reason = reason;
  close_fd(&run->helper_stdin_fd);
  close_fd(&run->helper_stdout_fd);
  run->helper_input_done = TRUE;
  run->helper_output_done = TRUE;
  if (run->helper_pid <= 0 || run->helper_reaped ||
      run->group_cleanup_done || run->force_kill_deadline != 0)
    return;
  if (!(reason == RUN_STOP_TIMEOUT && run->forward_timeout_sent &&
        run->deadlines.forward_at == run->deadlines.cleanup_at))
    signal_helper(run, SIGTERM);
  run->force_kill_deadline = network_sidebar_amneziawg_deadline_cap(
    monotonic_msec(), run->deadlines.hard_at, HELPER_TERMINATION_GRACE_MSEC);
}

static gboolean
maybe_finish_helper(AwgHelperRun *run)
{
  if (!run->helper_reaped || !run->group_cleanup_done ||
      !run->helper_input_done || !run->helper_output_done ||
      (run->supervisor != NULL &&
       !network_sidebar_awg_supervisor_channel_closed(run->supervisor)))
    return FALSE;
  run->helper_source = 0;
  finish_run(run);
  return TRUE;
}

static gboolean
set_nonblocking(int fd)
{
  int flags = fcntl(fd, F_GETFL);

  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void
fail_helper_io(AwgHelperRun *run)
{
  run->helper_io_failed = TRUE;
  begin_stop(run, RUN_STOP_IO_FAILURE);
}

static void
poll_supervisor(AwgHelperRun *run)
{
  if ((run->supervisor == NULL || !network_sidebar_awg_supervisor_poll(run->supervisor)) &&
      run->stop_reason == RUN_STOP_NONE)
    fail_helper_io(run);
}

static void
write_helper_input(AwgHelperRun *run)
{
  const guint8 *data;
  gsize length;

  if (run->helper_stdin_fd < 0)
    return;
  data = g_bytes_get_data(run->config, &length);
  while (run->helper_input_offset < length) {
    ssize_t written = write(run->helper_stdin_fd, data + run->helper_input_offset,
                            length - run->helper_input_offset);

    if (written > 0) {
      run->helper_input_offset += (gsize) written;
      continue;
    }
    if (written < 0 && errno == EINTR)
      continue;
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return;
    fail_helper_io(run);
    return;
  }
  close_fd(&run->helper_stdin_fd);
  run->helper_input_done = TRUE;
}

static void
read_helper_output(AwgHelperRun *run)
{
  guint8 buffer[4096];

  for (guint reads = 0; run->helper_stdout_fd >= 0 && reads < 16; reads++) {
    ssize_t count = read(run->helper_stdout_fd, buffer, sizeof(buffer));

    if (count > 0) {
      if ((gsize) count > NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_REPORT_MAX_SIZE -
                           run->helper_output->len) {
        fail_helper_io(run);
        return;
      }
      g_byte_array_append(run->helper_output, buffer, (guint) count);
      continue;
    }
    if (count == 0) {
      close_fd(&run->helper_stdout_fd);
      run->helper_output_done = TRUE;
      return;
    }
    if (errno == EINTR) {
      reads--;
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;
    fail_helper_io(run);
    return;
  }
}

static void
record_helper_reap(AwgHelperRun *run, pid_t helper_pid, int status, gboolean status_known)
{
  if (!status_known) {
    run->helper_status = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED;
  } else if (WIFSIGNALED(status)) {
    run->helper_status = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SIGNALED;
  } else if (!WIFEXITED(status) ||
             !network_sidebar_amneziawg_helper_exit_is_valid(WEXITSTATUS(status)) ||
             (!run->profile_report &&
              WEXITSTATUS(status) == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVENTORY_PARTIAL)) {
    run->helper_status = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED;
  } else {
    run->helper_status = WEXITSTATUS(status);
  }
  g_spawn_close_pid(helper_pid);
  run->helper_pid = 0;
  run->helper_reaped = TRUE;
}

static void
detach_stalled_helper(AwgHelperRun *run)
{
  pid_t helper_pid = run->helper_pid;

  if (helper_pid <= 0 || run->helper_reaped)
    return;
  g_hash_table_add(run->runner->pending_child_reaps, GINT_TO_POINTER(helper_pid));
  g_spawn_close_pid(helper_pid);
  run->helper_pid = 0;
  run->helper_reaped = TRUE;
  run->helper_io_failed = TRUE;
  run->helper_status = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED;
  close_fd(&run->helper_stdin_fd);
  close_fd(&run->helper_stdout_fd);
  run->helper_input_done = TRUE;
  run->helper_output_done = TRUE;
  network_sidebar_awg_supervisor_abort(run->supervisor);
}

static gboolean
helper_has_exited(AwgHelperRun *run)
{
  siginfo_t child_info = { 0 };
  int wait_result;

  do
    wait_result = waitid(P_PID, (id_t) run->helper_pid, &child_info,
                         WEXITED | WNOHANG | WNOWAIT);
  while (wait_result != 0 && errno == EINTR);
  if (wait_result == 0)
    return child_info.si_pid == run->helper_pid;

  if (errno != ECHILD) {
    pid_t helper_pid = run->helper_pid;
    int status = 0;
    pid_t waited;

    do
      waited = waitpid(helper_pid, &status, WNOHANG);
    while (waited < 0 && errno == EINTR);
    if (waited == helper_pid) {
      record_helper_reap(run, helper_pid, status, TRUE);
      return FALSE;
    }
    if (waited < 0 && errno == ECHILD)
      goto helper_lost;
    mark_failed(run->runner);
    if (run->stop_reason == RUN_STOP_NONE)
      fail_helper_io(run);
    return FALSE;
  }
helper_lost:
  if (!run->helper_reaped) {
    pid_t helper_pid = run->helper_pid;

    mark_failed(run->runner);
    if (run->stop_reason == RUN_STOP_NONE)
      run->stop_reason = RUN_STOP_IO_FAILURE;
    run->helper_io_failed = TRUE;
    record_helper_reap(run, helper_pid, 0, FALSE);
    begin_supervisor_cleanup(run, SIGKILL);
    close_fd(&run->helper_stdin_fd);
    close_fd(&run->helper_stdout_fd);
    run->helper_input_done = TRUE;
    run->helper_output_done = TRUE;
  }
  return FALSE;
}

static void
reap_helper(AwgHelperRun *run)
{
  pid_t helper_pid = run->helper_pid;
  pid_t waited;
  int status = 0;

  if (helper_pid <= 0 || run->helper_reaped) {
    mark_failed(run->runner);
    run->helper_io_failed = TRUE;
    return;
  }
  do
    waited = waitpid(helper_pid, &status, 0);
  while (waited < 0 && errno == EINTR);
  if (waited < 0 && errno != ECHILD) {
    mark_failed(run->runner);
    run->helper_io_failed = TRUE;
    return;
  }
  if (waited != helper_pid) {
    mark_failed(run->runner);
    run->helper_io_failed = TRUE;
  }
  record_helper_reap(run, helper_pid, status, waited == helper_pid);
}

static gboolean
helper_poll_cb(gpointer user_data)
{
  AwgHelperRun *run = user_data;
  gint64 now;
  gboolean child_exited = FALSE;

  write_helper_input(run);
  read_helper_output(run);
  if (!run->helper_reaped)
    child_exited = helper_has_exited(run);
  now = monotonic_msec();
  if (run->stop_reason == RUN_STOP_NONE && !run->forward_timeout_sent &&
      !run->helper_reaped && !child_exited && now >= run->deadlines.forward_at &&
      !network_sidebar_awg_supervisor_release_pending(run->supervisor)) {
    network_sidebar_awg_supervisor_disable_release(run->supervisor);
    signal_helper(run, SIGTERM);
    run->forward_timeout_sent = TRUE;
  }
  poll_supervisor(run);
  if (run->stop_reason == RUN_STOP_NONE && !run->helper_reaped &&
      !child_exited && now >= run->deadlines.cleanup_at)
    begin_stop(run, RUN_STOP_TIMEOUT);
  if (!child_exited && !run->helper_reaped && run->supervisor != NULL &&
      network_sidebar_awg_supervisor_channel_closed(run->supervisor)) {
    if (run->supervisor_close_observed_at == 0)
      run->supervisor_close_observed_at = now;
    else if (run->stop_reason == RUN_STOP_NONE && now - run->supervisor_close_observed_at >= 100)
      fail_helper_io(run);
  }

  if (run->stop_reason != RUN_STOP_NONE && !run->helper_reaped && now >= run->force_kill_deadline) {
    if (!run->helper_cleanup_kill_requested) {
      signal_helper(run, SIGINT);
      begin_supervisor_cleanup(run, SIGTERM);
      run->helper_cleanup_kill_requested = TRUE;
      run->force_kill_deadline = network_sidebar_amneziawg_deadline_cap(
        now, run->deadlines.hard_at, HELPER_CLEANUP_REAP_GRACE_MSEC);
    } else if (!run->helper_force_kill_sent) {
      signal_helper(run, SIGKILL);
      begin_supervisor_cleanup(run, SIGKILL);
      run->helper_force_kill_sent = TRUE;
    }
  }
  if (child_exited) {
    /* Preserve the exact helper PID until its observed exit is reaped. */
    poll_supervisor(run);
    reap_helper(run);
  } else if (!run->profile_report && run->helper_force_kill_sent &&
             !run->helper_reaped && run->group_cleanup_deadline > 0 &&
             now >= run->group_cleanup_deadline) {
    /* Inventory keeps cleanup ownership even after its result is delivered. */
    detach_stalled_helper(run);
  }
  if (run->helper_reaped)
    complete_helper_supervision(run);
  if (run->helper_reaped && !run->helper_input_done) {
    run->helper_io_failed = TRUE;
    close_fd(&run->helper_stdin_fd);
    run->helper_input_done = TRUE;
  }
  read_helper_output(run);
  if (maybe_finish_helper(run))
    return G_SOURCE_REMOVE;
  if (run->profile_report && now >= run->deadlines.hard_at)
    publish_result(run);
  return G_SOURCE_CONTINUE;
}

static void
helper_child_setup(gpointer user_data)
{
  const pid_t *expected_parent = user_data;

  if (setpgid(0, 0) != 0 || prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0 ||
      getppid() != *expected_parent)
    _exit(127);
}

static gboolean
spawn_helper(AwgHelperRun *run)
{
  char forward_deadline[32];
  char cleanup_deadline[32];
  const char *argv[] = {
    NETWORK_SIDEBAR_INSTALLED_AWG_HELPER_PATH, run->operation, run->name,
    forward_deadline, cleanup_deadline, NULL,
  };
  g_autoptr(GError) error = NULL;
  pid_t expected_parent = getpid();
  GSpawnFlags flags = G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_CLOEXEC_PIPES | G_SPAWN_STDERR_TO_DEV_NULL;
  int control[2] = { -1, -1 };
  int source_fds[1];
  int target_fds[1] = { NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_FD };
  gboolean io_setup_failed = FALSE;
  gint64 started_at = monotonic_msec();

  run->capabilities = helper_capabilities();
  if ((run->capabilities & NETWORK_SIDEBAR_AMNEZIAWG_CAPABILITY_HELPER) == 0 ||
      !network_sidebar_amneziawg_deadlines_init(run->operation, started_at, &run->deadlines))
    return FALSE;
  g_snprintf(forward_deadline, sizeof(forward_deadline), "%" G_GINT64_FORMAT, run->deadlines.forward_at);
  g_snprintf(cleanup_deadline, sizeof(cleanup_deadline), "%" G_GINT64_FORMAT, run->deadlines.cleanup_at);
  run->helper_input_done = run->config == NULL;
  run->helper_output_done = !run->profile_report;
  if (!run->profile_report)
    flags |= G_SPAWN_STDOUT_TO_DEV_NULL;
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, control) != 0)
    return FALSE;
  source_fds[0] = control[1];
  if (!g_spawn_async_with_pipes_and_fds(
        NULL, argv, NULL, flags, helper_child_setup, &expected_parent,
        -1, -1, -1, source_fds, target_fds, G_N_ELEMENTS(source_fds),
        &run->helper_pid, run->config != NULL ? &run->helper_stdin_fd : NULL,
        run->profile_report ? &run->helper_stdout_fd : NULL, NULL, &error)) {
    close_fd(&control[1]);
    close_fd(&control[0]);
    run->helper_pid = 0;
    return FALSE;
  }
  close_fd(&control[1]);
  run->supervisor = network_sidebar_awg_supervisor_new(
    control[0], g_strcmp0(run->operation, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_UP) == 0);
  if (run->supervisor == NULL) {
    close_fd(&control[0]);
    io_setup_failed = TRUE;
  }
  if (setpgid(run->helper_pid, run->helper_pid) != 0 && errno != EACCES && errno != ESRCH)
    io_setup_failed = TRUE;
  if (run->helper_stdin_fd >= 0 && !set_nonblocking(run->helper_stdin_fd))
    io_setup_failed = TRUE;
  if (run->helper_stdout_fd >= 0 && !set_nonblocking(run->helper_stdout_fd))
    io_setup_failed = TRUE;
  if (run->profile_report)
    run->helper_output = g_byte_array_sized_new(NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_REPORT_MAX_SIZE);
  run->helper_source = g_timeout_add(HELPER_POLL_INTERVAL_MSEC, helper_poll_cb, run);
  if (io_setup_failed)
    fail_helper_io(run);
  return TRUE;
}

static gboolean
reap_children_cb(gpointer user_data)
{
  NetworkSidebarAwgHelperRunner *runner = user_data;
  GHashTableIter iter;
  gpointer key;

  g_hash_table_iter_init(&iter, runner->released_groups);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    pid_t process_group = GPOINTER_TO_INT(key);
    pid_t waited;

    do
      waited = waitpid(-process_group, NULL, WNOHANG);
    while (waited < 0 && errno == EINTR);
    while (waited > 0) {
      do
        waited = waitpid(-process_group, NULL, WNOHANG);
      while (waited < 0 && errno == EINTR);
    }
    if (waited < 0 && errno == ECHILD)
      g_hash_table_iter_remove(&iter);
  }
  reap_pending_children(runner);
  notify_changed(runner);
  return G_SOURCE_CONTINUE;
}

NetworkSidebarAwgHelperRunner *
network_sidebar_awg_helper_runner_new(const NetworkSidebarAwgHelperRunnerCallbacks *callbacks,
                                     gpointer user_data)
{
  NetworkSidebarAwgHelperRunner *runner;
  struct sigaction action = { 0 };

  if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0)
    return NULL;
  sigemptyset(&action.sa_mask);
  action.sa_handler = SIG_IGN;
  if (sigaction(SIGPIPE, &action, NULL) != 0)
    return NULL;
  action.sa_handler = SIG_DFL;
  if (sigaction(SIGCHLD, &action, NULL) != 0)
    return NULL;
  runner = g_new0(NetworkSidebarAwgHelperRunner, 1);
  if (callbacks != NULL)
    runner->callbacks = *callbacks;
  runner->user_data = user_data;
  runner->released_groups = g_hash_table_new(g_direct_hash, g_direct_equal);
  runner->pending_child_reaps = g_hash_table_new(g_direct_hash, g_direct_equal);
  runner->reap_source = g_timeout_add_seconds(RELEASED_GROUP_REAP_INTERVAL_SECONDS,
                                              reap_children_cb, runner);
  return runner;
}

void
network_sidebar_awg_helper_runner_free(NetworkSidebarAwgHelperRunner *runner)
{
  if (runner == NULL)
    return;
  g_return_if_fail(!network_sidebar_awg_helper_runner_is_busy(runner));
  g_source_remove(runner->reap_source);
  g_hash_table_unref(runner->released_groups);
  g_hash_table_unref(runner->pending_child_reaps);
  g_free(runner);
}

gboolean
network_sidebar_awg_helper_runner_start(NetworkSidebarAwgHelperRunner *runner,
                                        const char *operation,
                                        const char *name,
                                        const guint8 *config,
                                        gsize config_length)
{
  AwgHelperRun *run;

  g_return_val_if_fail(runner != NULL, FALSE);
  g_return_val_if_fail(config != NULL || config_length == 0, FALSE);
  if (runner->failed || network_sidebar_awg_helper_runner_is_busy(runner))
    return FALSE;
  run = g_new0(AwgHelperRun, 1);
  run->runner = runner;
  run->operation = g_strdup(operation);
  run->name = g_strdup(name);
  run->profile_report = g_strcmp0(operation, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_LIST_PROFILES) == 0;
  run->helper_stdin_fd = -1;
  run->helper_stdout_fd = -1;
  if (config_length != 0)
    run->config = secret_bytes_copy(config, config_length);
  runner->run = run;
  if (!spawn_helper(run)) {
    run->helper_status = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED;
    finish_run(run);
  }
  return TRUE;
}

void
network_sidebar_awg_helper_runner_stop(NetworkSidebarAwgHelperRunner *runner,
                                       NetworkSidebarAwgHelperStopReason reason)
{
  AwgHelperRun *run;

  if (runner == NULL || runner->run == NULL)
    return;
  run = runner->run;
  begin_stop(run, reason == NETWORK_SIDEBAR_AWG_HELPER_STOP_PREEMPTED ?
                   RUN_STOP_PREEMPTED : RUN_STOP_SHUTDOWN);
  if (run->profile_report && reason == NETWORK_SIDEBAR_AWG_HELPER_STOP_PREEMPTED)
    publish_result(run);
}

gboolean
network_sidebar_awg_helper_runner_is_running(const NetworkSidebarAwgHelperRunner *runner)
{
  return runner != NULL && runner->run != NULL;
}

gboolean
network_sidebar_awg_helper_runner_is_busy(NetworkSidebarAwgHelperRunner *runner)
{
  if (runner == NULL)
    return FALSE;
  reap_pending_children(runner);
  return runner->run != NULL || g_hash_table_size(runner->pending_child_reaps) != 0;
}

gboolean
network_sidebar_awg_helper_runner_failed(const NetworkSidebarAwgHelperRunner *runner)
{
  return runner != NULL && runner->failed;
}
