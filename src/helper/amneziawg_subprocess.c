#define _GNU_SOURCE

#include "helper/amneziawg_subprocess.h"

#include "amneziawg/deadline.h"
#include "helper/amneziawg_helper_util.h"
#include "helper/amneziawg_process.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#define IO_CHUNK_SIZE 16384u
#define DIAGNOSTIC_DRAIN_READS 16u

static gboolean
move_above_stdio(int *fd)
{
  int moved;

  if (*fd > STDERR_FILENO)
    return TRUE;
  moved = fcntl(*fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  if (moved < 0)
    return FALSE;
  close(*fd);
  *fd = moved;
  return TRUE;
}

static gboolean
open_pipe(int pipe_fds[2], guint nonblocking_end)
{
  int flags;

  if (pipe2(pipe_fds, O_CLOEXEC) != 0 ||
      !move_above_stdio(&pipe_fds[0]) || !move_above_stdio(&pipe_fds[1]))
    return FALSE;
  flags = fcntl(pipe_fds[nonblocking_end], F_GETFL);
  return flags >= 0 && fcntl(pipe_fds[nonblocking_end], F_SETFL,
                             flags | O_NONBLOCK) == 0;
}

static void
exec_child(const AwgSubprocessRequest *request,
           int executable,
           const sigset_t *previous_mask,
           pid_t parent,
           int gate[2],
           int input[2],
           int output[2],
           int diagnostics[2])
{
  static char *const environment[] = {
    "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/root", "LANG=C", "LC_ALL=C", NULL,
  };
  struct sigaction action = { .sa_handler = SIG_DFL };
  int null_fd;

  awg_helper_close_fd(&input[1]);
  awg_helper_close_fd(&output[0]);
  awg_helper_close_fd(&diagnostics[0]);
  if (!network_sidebar_amneziawg_process_prepare_child(
        previous_mask, parent, gate[0], gate[1]))
    _exit(127);
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGPIPE, &action, NULL) != 0)
    _exit(127);
  null_fd = open("/dev/null", O_RDWR | O_NOFOLLOW | O_CLOEXEC);
  if (null_fd < 0 || !move_above_stdio(&null_fd) ||
      dup2(input[0] >= 0 ? input[0] : null_fd, STDIN_FILENO) < 0 ||
      dup2(output[1] >= 0 ? output[1] : null_fd, STDOUT_FILENO) < 0 ||
      dup2(diagnostics[1] >= 0 ? diagnostics[1] : null_fd, STDERR_FILENO) < 0)
    _exit(127);
  close(null_fd);
  awg_helper_close_fd(&input[0]);
  awg_helper_close_fd(&output[1]);
  awg_helper_close_fd(&diagnostics[1]);
  /* A trusted script interpreter must be able to reopen /dev/fd/<executable>. */
  if (fcntl(executable, F_SETFD, 0) != 0)
    _exit(127);
  fexecve(executable, (char *const *) request->argv, environment);
  _exit(127);
}

static gboolean
write_input(int *fd, const guint8 *data, gsize length, gsize *offset)
{
  ssize_t count;

  if (*fd < 0)
    return TRUE;
  count = write(*fd, data + *offset, MIN(length - *offset, IO_CHUNK_SIZE));
  if (count > 0) {
    *offset += (gsize) count;
    if (*offset == length)
      awg_helper_close_fd(fd);
    return TRUE;
  }
  return count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK);
}

static AwgSubprocessStatus
read_output(int *fd, guint8 *data, gsize limit, gsize *length)
{
  ssize_t count;

  if (*fd < 0)
    return AWG_SUBPROCESS_EXITED;
  count = read(*fd, data + *length, MIN(limit + 1 - *length, IO_CHUNK_SIZE));
  if (count > 0) {
    *length += (gsize) count;
    return *length > limit ? AWG_SUBPROCESS_OUTPUT_TOO_LARGE : AWG_SUBPROCESS_EXITED;
  }
  if (count == 0)
    awg_helper_close_fd(fd);
  else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
    return AWG_SUBPROCESS_IO_FAILED;
  return AWG_SUBPROCESS_EXITED;
}

static gboolean
read_diagnostics(int *fd, const AwgSubprocessRequest *request)
{
  guint8 buffer[4096];

  for (guint i = 0; *fd >= 0 && i < DIAGNOSTIC_DRAIN_READS; i++) {
    ssize_t count = read(*fd, buffer, sizeof(buffer));

    if (count > 0) {
      request->diagnostics(buffer, (gsize) count, request->user_data);
      awg_helper_wipe_bytes(buffer, (gsize) count);
    } else if (count == 0) {
      awg_helper_close_fd(fd);
    } else {
      return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
    }
  }
  return TRUE;
}

AwgSubprocessResult
awg_subprocess_run(const AwgSubprocessRequest *request,
                    gint64 outer_deadline,
                    guint8 **output,
                    gsize *output_length)
{
  AwgSubprocessResult result = { .status = AWG_SUBPROCESS_SETUP_FAILED };
  int executable = -1;
  int input_pipe[2] = { -1, -1 };
  int output_pipe[2] = { -1, -1 };
  int diagnostic_pipe[2] = { -1, -1 };
  int gate[2] = { -1, -1 };
  guint8 *buffer = NULL;
  gsize used = 0;
  gsize written = 0;
  gboolean child_done = FALSE;
  gboolean final_drain = FALSE;
  gboolean capture = output != NULL && output_length != NULL;
  gboolean registered;
  sigset_t previous_mask;
  pid_t parent = getpid();
  pid_t child = -1;
  gint64 now = network_sidebar_amneziawg_process_monotonic_msec();
  gint64 deadline;

  if (output != NULL)
    *output = NULL;
  if (output_length != NULL)
    *output_length = 0;
  if (request == NULL || request->argv == NULL || request->argv[0] == NULL ||
      (output == NULL) != (output_length == NULL) ||
      (capture && (request->output_limit == 0 || request->output_limit == G_MAXSIZE)) ||
      (request->input_length > 0 && request->input == NULL))
    return result;
  deadline = network_sidebar_amneziawg_deadline_cap(
    now, outer_deadline > NETWORK_SIDEBAR_AMNEZIAWG_CHILD_CLEANUP_RESERVE_MSEC ?
      outer_deadline - NETWORK_SIDEBAR_AMNEZIAWG_CHILD_CLEANUP_RESERVE_MSEC : -1,
    request->timeout_msec);
  if (now < 0 || now >= deadline) {
    result.status = AWG_SUBPROCESS_TIMED_OUT;
    return result;
  }
  if (network_sidebar_amneziawg_process_termination_requested()) {
    result.status = AWG_SUBPROCESS_CANCELLED;
    return result;
  }
  executable = fcntl(request->executable, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  if (executable < 0 ||
      (request->input_length > 0 && !open_pipe(input_pipe, 1)) ||
      (capture && !open_pipe(output_pipe, 0)) ||
      (request->diagnostics != NULL && !open_pipe(diagnostic_pipe, 0)) ||
      !network_sidebar_amneziawg_process_open_start_gate(gate))
    goto out;
  if (capture)
    buffer = g_malloc(request->output_limit + 1);
  if (!network_sidebar_amneziawg_process_block_termination(&previous_mask))
    goto out;
  child = fork();
  if (child < 0) {
    network_sidebar_amneziawg_process_restore_mask(&previous_mask);
    goto out;
  }
  if (child == 0)
    exec_child(request, executable, &previous_mask, parent, gate,
                input_pipe, output_pipe, diagnostic_pipe);

  result.may_have_run = TRUE;
  registered = network_sidebar_amneziawg_process_register_group(
    child, &previous_mask, gate[0], gate[1], deadline);
  gate[0] = gate[1] = -1;
  awg_helper_close_fd(&executable);
  awg_helper_close_fd(&input_pipe[0]);
  awg_helper_close_fd(&output_pipe[1]);
  awg_helper_close_fd(&diagnostic_pipe[1]);
  if (!registered)
    goto finish_child;
  result.status = AWG_SUBPROCESS_EXITED;

  for (;;) {
    siginfo_t info = { 0 };
    struct pollfd waiting[3];
    nfds_t count = 0;

    now = network_sidebar_amneziawg_process_monotonic_msec();
    if (network_sidebar_amneziawg_process_termination_requested()) {
      result.status = AWG_SUBPROCESS_CANCELLED;
      break;
    }
    if (now < 0 || now >= deadline) {
      result.status = AWG_SUBPROCESS_TIMED_OUT;
      break;
    }
    if (!write_input(&input_pipe[1], request->input, request->input_length, &written) ||
        !read_diagnostics(&diagnostic_pipe[0], request)) {
      result.status = AWG_SUBPROCESS_IO_FAILED;
      break;
    }
    result.status = read_output(&output_pipe[0], buffer, request->output_limit, &used);
    if (result.status != AWG_SUBPROCESS_EXITED)
      break;
    if (!child_done) {
      if (waitid(P_PID, (id_t) child, &info, WEXITED | WNOHANG | WNOWAIT) == 0)
        child_done = info.si_pid == child;
      else if (errno != EINTR) {
        result.status = AWG_SUBPROCESS_IO_FAILED;
        break;
      }
    }
    if (child_done) {
      if (written != request->input_length) {
        result.status = AWG_SUBPROCESS_IO_FAILED;
        break;
      }
      if (output_pipe[0] < 0 && diagnostic_pipe[0] < 0)
        break;
      if (request->completion == AWG_SUBPROCESS_DRAIN_ON_EXIT) {
        if (final_drain)
          break;
        final_drain = TRUE;
        continue;
      }
    }
    if (input_pipe[1] >= 0)
      waiting[count++] = (struct pollfd) { input_pipe[1], POLLOUT | POLLHUP, 0 };
    if (output_pipe[0] >= 0)
      waiting[count++] = (struct pollfd) { output_pipe[0], POLLIN | POLLHUP, 0 };
    if (diagnostic_pipe[0] >= 0)
      waiting[count++] = (struct pollfd) { diagnostic_pipe[0], POLLIN | POLLHUP, 0 };
    int polled = network_sidebar_amneziawg_process_poll(
      waiting, count, (int) MIN(deadline - now, 25));
    if (polled < 0 && errno != EINTR) {
      result.status = AWG_SUBPROCESS_IO_FAILED;
      break;
    }
  }

finish_child:
  awg_helper_close_fd(&input_pipe[1]);
  if (child_done) {
    siginfo_t info = { 0 };
    gboolean retain = result.status == AWG_SUBPROCESS_EXITED &&
      waitid(P_PID, (id_t) child, &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
      info.si_pid == child && info.si_code == CLD_EXITED && info.si_status == 0 &&
      request->retain_descendants != NULL && request->retain_descendants(request->user_data);

    if (!network_sidebar_amneziawg_process_finish_group(
          child, !retain, outer_deadline, &result.wait_status) &&
        result.status == AWG_SUBPROCESS_EXITED)
      result.status = AWG_SUBPROCESS_IO_FAILED;
    /* A failed launcher's process-substitution children can close stderr just
     * after the final exit drain. Collect their buffered diagnostics/EOF after
     * confirmed group cleanup; a retained backend must not be waited on. */
    if (!retain && result.status == AWG_SUBPROCESS_EXITED &&
        !read_diagnostics(&diagnostic_pipe[0], request))
      result.status = AWG_SUBPROCESS_IO_FAILED;
  } else {
    network_sidebar_amneziawg_process_terminate_group(
      child, request->termination_grace_msec, outer_deadline, &result.wait_status);
  }
  if (network_sidebar_amneziawg_process_termination_requested())
    result.status = AWG_SUBPROCESS_CANCELLED;
  now = network_sidebar_amneziawg_process_monotonic_msec();
  if (result.status == AWG_SUBPROCESS_EXITED && (now < 0 || now >= outer_deadline))
    result.status = AWG_SUBPROCESS_TIMED_OUT;
  if (capture && result.status == AWG_SUBPROCESS_EXITED) {
    *output = g_memdup2(buffer, used);
    *output_length = used;
  }

out:
  result.diagnostics_complete = request->diagnostics != NULL &&
                                diagnostic_pipe[0] < 0 && child_done;
  if (buffer != NULL)
    awg_helper_wipe_bytes(buffer, request->output_limit + 1);
  g_free(buffer);
  awg_helper_close_fd(&executable);
  awg_helper_close_fd(&input_pipe[0]);
  awg_helper_close_fd(&input_pipe[1]);
  awg_helper_close_fd(&output_pipe[0]);
  awg_helper_close_fd(&output_pipe[1]);
  awg_helper_close_fd(&diagnostic_pipe[0]);
  awg_helper_close_fd(&diagnostic_pipe[1]);
  awg_helper_close_fd(&gate[0]);
  awg_helper_close_fd(&gate[1]);
  return result;
}
