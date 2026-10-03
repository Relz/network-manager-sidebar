#define _GNU_SOURCE

#include "helper/amneziawg_process.h"

#include "amneziawg/deadline.h"
#include "helper/amneziawg_supervision_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SUPERVISION_REPLY_TIMEOUT_MSEC 5000u
#define GROUP_REAP_GRACE_MSEC 100u

enum {
  TERMINATION_LEVEL_MASK = 3u,
  TERMINATION_REQUESTED = 1u,
  TERMINATION_ESCALATED = 2u,
  TERMINATION_CLEANUP = 4u,
  TERMINATION_COMMITTED = 8u,
};

/* A locking atomic fallback is not safe inside an asynchronous handler. */
G_STATIC_ASSERT(ATOMIC_INT_LOCK_FREE == 2);
static atomic_uint termination_state = ATOMIC_VAR_INIT(0);
static atomic_int termination_write_fd = ATOMIC_VAR_INIT(-1);
static int termination_read_fd = -1;
static pid_t active_process_group = -1;
static pid_t retained_process_group = -1;
static unsigned int active_signal_level = 0;
static unsigned int retained_signal_level = 0;
static int process_supervision_fd = -1;
static gint64 process_cleanup_deadline = -1;
static guint32 supervision_sequence = 1;
static gboolean supervision_request_in_progress = FALSE;
static gboolean dispatching_termination = FALSE;

static gboolean begin_group_termination(pid_t child, gint64 deadline);

static void
close_fd(int *fd)
{
  if (*fd >= 0)
    close(*fd);
  *fd = -1;
}

static void
termination_signal_handler(int signal_number)
{
  int saved_errno = errno;
  unsigned int state = atomic_load(&termination_state);

  (void) signal_number;
  while ((state & TERMINATION_COMMITTED) == 0 &&
         (state & TERMINATION_LEVEL_MASK) < TERMINATION_ESCALATED) {
    unsigned int next = state + 1;

    if (atomic_compare_exchange_weak(&termination_state, &state, next)) {
      const unsigned char wakeup = 1;
      int fd = atomic_load(&termination_write_fd);
      ssize_t written;

      /* The atomic state survives a full pipe; this is only a wakeup. */
      if (fd >= 0) {
        do
          written = write(fd, &wakeup, sizeof(wakeup));
        while (written < 0 && errno == EINTR);
      }
      break;
    }
  }
  errno = saved_errno;
}

static gboolean
set_nonblocking(int fd)
{
  int flags = fcntl(fd, F_GETFL);

  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

gboolean
network_sidebar_amneziawg_process_init(int supervision_fd,
                                       gint64 cleanup_deadline)
{
  struct ucred credentials;
  socklen_t credentials_length = sizeof(credentials);
  int fd_flags;
  int socket_type;
  socklen_t socket_type_length = sizeof(socket_type);

  if (process_supervision_fd >= 0 || supervision_fd < 0 || cleanup_deadline <= 0 ||
      getsockopt(supervision_fd,
                 SOL_SOCKET,
                 SO_TYPE,
                 &socket_type,
                 &socket_type_length) != 0 ||
      socket_type != SOCK_SEQPACKET ||
      getsockopt(supervision_fd,
                 SOL_SOCKET,
                 SO_PEERCRED,
                 &credentials,
                 &credentials_length) != 0 ||
      credentials.pid != getppid() || credentials.uid != geteuid() ||
      credentials.gid != getegid() || !set_nonblocking(supervision_fd))
    return FALSE;
  fd_flags = fcntl(supervision_fd, F_GETFD);
  if (fd_flags < 0 ||
      fcntl(supervision_fd, F_SETFD, fd_flags | FD_CLOEXEC) != 0)
    return FALSE;
  process_supervision_fd = supervision_fd;
  process_cleanup_deadline = cleanup_deadline;
  return TRUE;
}

gboolean
network_sidebar_amneziawg_process_install_handlers(void)
{
  struct sigaction action = { 0 };
  int wakeup[2];

  if (termination_read_fd >= 0 ||
      pipe2(wakeup, O_CLOEXEC | O_NONBLOCK) != 0)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS(wakeup); i++) {
    if (wakeup[i] <= STDERR_FILENO) {
      int moved = fcntl(wakeup[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);

      if (moved < 0) {
        close(wakeup[0]);
        close(wakeup[1]);
        return FALSE;
      }
      close(wakeup[i]);
      wakeup[i] = moved;
    }
  }
  termination_read_fd = wakeup[0];
  /* Never close or replace the published descriptors while handlers can run. */
  atomic_store(&termination_write_fd, wakeup[1]);

  action.sa_handler = termination_signal_handler;
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGTERM);
  sigaddset(&action.sa_mask, SIGINT);
  return sigaction(SIGTERM, &action, NULL) == 0 &&
         sigaction(SIGINT, &action, NULL) == 0;
}

gboolean
network_sidebar_amneziawg_process_termination_requested(void)
{
  unsigned int state = atomic_load(&termination_state);
  unsigned int level = state & TERMINATION_LEVEL_MASK;

  return (state & TERMINATION_COMMITTED) == 0 &&
         (level == TERMINATION_ESCALATED ||
          (level == TERMINATION_REQUESTED &&
           (state & TERMINATION_CLEANUP) == 0));
}

gboolean
network_sidebar_amneziawg_process_is_committed(void)
{
  return (atomic_load(&termination_state) & TERMINATION_COMMITTED) != 0;
}

void
network_sidebar_amneziawg_process_dispatch_termination(void)
{
  unsigned int state = atomic_load(&termination_state);
  unsigned int level = state & TERMINATION_LEVEL_MASK;
  int child_signal;
  gint64 deadline;

  if (dispatching_termination || (state & TERMINATION_COMMITTED) != 0 || level == 0 ||
      (level == TERMINATION_REQUESTED && (state & TERMINATION_CLEANUP) != 0))
    return;
  /* Polling an ACK can observe another cancellation. Finish that exchange
   * before requesting termination; never consume another request's reply or
   * destroy its sentinel before the supervisor has acknowledged our intent. */
  if (level == TERMINATION_ESCALATED && supervision_request_in_progress)
    return;
  dispatching_termination = TRUE;
  deadline = network_sidebar_amneziawg_deadline_cap(
    network_sidebar_amneziawg_process_monotonic_msec(), process_cleanup_deadline,
    SUPERVISION_REPLY_TIMEOUT_MSEC);
  child_signal = level == TERMINATION_ESCALATED ? SIGKILL : SIGTERM;
  if (active_process_group > 0 && level > active_signal_level) {
    active_signal_level = level;
    if (level == TERMINATION_ESCALATED)
      begin_group_termination(active_process_group, deadline);
    kill(-active_process_group, child_signal);
    kill(active_process_group, child_signal);
  }
  /* First-stage cancellation stops forward work, but rollback still needs the
   * userspace backend's link. Only escalation may destroy it here. */
  if (level == TERMINATION_ESCALATED && retained_process_group > 0 &&
      retained_process_group != active_process_group &&
      level > retained_signal_level) {
    retained_signal_level = level;
    begin_group_termination(retained_process_group, deadline);
    kill(-retained_process_group, child_signal);
  }
  dispatching_termination = FALSE;
}

static gboolean
drain_termination_wakeup(void)
{
  unsigned char buffer[128];
  gboolean received = FALSE;
  ssize_t count;

  if (termination_read_fd < 0)
    return FALSE;
  do {
    count = read(termination_read_fd, buffer, sizeof(buffer));
    if (count > 0)
      received = TRUE;
  } while (count > 0 || (count < 0 && errno == EINTR));
  return received;
}

int
network_sidebar_amneziawg_process_poll(struct pollfd *fds,
                                      nfds_t count,
                                      int timeout_msec)
{
  g_autofree struct pollfd *waiting = NULL;
  gboolean wakeup;
  int result;
  int saved_errno;
  int ready = 0;

  if ((count != 0 && fds == NULL) ||
      count > G_MAXSIZE / sizeof(*waiting) - 1) {
    errno = EINVAL;
    return -1;
  }
  for (nfds_t i = 0; i < count; i++)
    fds[i].revents = 0;
  wakeup = drain_termination_wakeup();
  network_sidebar_amneziawg_process_dispatch_termination();
  if (wakeup) {
    errno = EINTR;
    return -1;
  }
  waiting = g_new(struct pollfd, count + 1);
  for (nfds_t i = 0; i < count; i++)
    waiting[i] = fds[i];
  waiting[count] = (struct pollfd) { termination_read_fd, POLLIN, 0 };
  result = poll(waiting, count + 1, timeout_msec);
  saved_errno = errno;
  wakeup = drain_termination_wakeup();
  network_sidebar_amneziawg_process_dispatch_termination();
  if (wakeup) {
    errno = EINTR;
    return -1;
  }
  if (result < 0) {
    errno = saved_errno;
    return -1;
  }
  if ((waiting[count].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
    errno = EIO;
    return -1;
  }
  for (nfds_t i = 0; i < count; i++) {
    fds[i].revents = waiting[i].revents;
    if (fds[i].revents != 0)
      ready++;
  }
  return ready;
}

void
network_sidebar_amneziawg_process_set_cleanup_mode(gboolean enabled)
{
  network_sidebar_amneziawg_process_dispatch_termination();
  if (enabled)
    atomic_fetch_or(&termination_state, TERMINATION_CLEANUP);
  else
    atomic_fetch_and(&termination_state, ~TERMINATION_CLEANUP);
  network_sidebar_amneziawg_process_dispatch_termination();
}

gboolean
network_sidebar_amneziawg_process_block_termination(sigset_t *previous_mask)
{
  sigset_t blocked;
  int error;

  if (previous_mask == NULL)
    return FALSE;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGTERM);
  sigaddset(&blocked, SIGINT);
  error = pthread_sigmask(SIG_BLOCK, &blocked, previous_mask);
  if (error != 0)
    errno = error;
  return error == 0;
}

void
network_sidebar_amneziawg_process_restore_mask(const sigset_t *previous_mask)
{
  if (previous_mask != NULL)
    pthread_sigmask(SIG_SETMASK, previous_mask, NULL);
}

static gboolean
termination_is_pending(void)
{
  sigset_t pending_mask;

  return (atomic_load(&termination_state) & TERMINATION_LEVEL_MASK) != 0 ||
         sigpending(&pending_mask) != 0 ||
         sigismember(&pending_mask, SIGTERM) == 1 ||
         sigismember(&pending_mask, SIGINT) == 1;
}

static void
collect_pending_termination(void)
{
  const struct timespec no_wait = { 0 };
  sigset_t signals;
  sigset_t previous_mask;
  int received;

  /* Also consume signals when the operation thread is the only recipient and
   * its fork/commit mask prevents the handler from waking an ACK wait. */
  sigemptyset(&signals);
  sigaddset(&signals, SIGTERM);
  sigaddset(&signals, SIGINT);
  if (pthread_sigmask(SIG_BLOCK, &signals, &previous_mask) != 0)
    return;
  while ((atomic_load(&termination_state) & TERMINATION_LEVEL_MASK) <
           TERMINATION_ESCALATED &&
         !network_sidebar_amneziawg_process_is_committed()) {
    received = sigtimedwait(&signals, NULL, &no_wait);
    if (received > 0)
      termination_signal_handler(received);
    else if (errno != EINTR)
      break;
  }
  pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
}

static gboolean
commit_operation(gint64 deadline)
{
  unsigned int expected = 0;
  gint64 now = network_sidebar_amneziawg_process_monotonic_msec();

  collect_pending_termination();
  if (now < 0 || now >= deadline || active_process_group > 0 ||
      termination_is_pending())
    return FALSE;
  /* Cancellation and commit share a single linearization point. */
  return atomic_compare_exchange_strong(&termination_state, &expected,
                                         TERMINATION_COMMITTED);
}

gint64
network_sidebar_amneziawg_process_monotonic_msec(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return -1;
  return (gint64) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static gboolean
wait_for_supervision_event(short events, gint64 deadline)
{
  for (;;) {
    struct pollfd control = { process_supervision_fd, events | POLLHUP, 0 };
    gint64 now;
    int timeout;
    int result;

    collect_pending_termination();
    now = network_sidebar_amneziawg_process_monotonic_msec();
    if (process_supervision_fd < 0 || now < 0 || now >= deadline)
      return FALSE;
    timeout = (int) MIN(deadline - now, 25);
    result = network_sidebar_amneziawg_process_poll(&control, 1, timeout);
    if (result < 0 && errno == EINTR) {
      /* Finish the bounded exchange even when cancellation starts cleanup. */
      continue;
    }
    if (result < 0 || (control.revents & (POLLERR | POLLNVAL | POLLHUP)) != 0)
      return FALSE;
    if ((control.revents & events) != 0)
      return TRUE;
  }
}

static gboolean
fail_supervision(void)
{
  close_fd(&process_supervision_fd);
  return FALSE;
}

static gboolean
supervision_exchange(NetworkSidebarAmneziaWGSupervisionType type,
                     pid_t process_group,
                     gint64 outer_deadline)
{
  NetworkSidebarAmneziaWGSupervisionMessage request = {
    NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_MAGIC,
    NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_VERSION,
    (guint16) type,
    (gint32) process_group,
    supervision_sequence,
    0,
  };
  NetworkSidebarAmneziaWGSupervisionMessage response;
  gint64 now = network_sidebar_amneziawg_process_monotonic_msec();
  gint64 deadline;
  ssize_t count;

  if (process_supervision_fd < 0 || process_group <= 0 || now < 0 ||
      now >= outer_deadline)
    return fail_supervision();
  deadline = network_sidebar_amneziawg_deadline_cap(
    now, outer_deadline, SUPERVISION_REPLY_TIMEOUT_MSEC);
  if (++supervision_sequence == 0)
    supervision_sequence = 1;

  for (;;) {
    do
      count = send(process_supervision_fd,
                   &request,
                   sizeof(request),
                   MSG_DONTWAIT | MSG_NOSIGNAL);
    while (count < 0 && errno == EINTR);
    if (count == (ssize_t) sizeof(request))
      break;
    if (count >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK) ||
        !wait_for_supervision_event(POLLOUT, deadline))
      return fail_supervision();
  }

  for (;;) {
    do
      count = recv(process_supervision_fd,
                   &response,
                   sizeof(response),
                   MSG_DONTWAIT | MSG_TRUNC);
    while (count < 0 && errno == EINTR);
    if (count == (ssize_t) sizeof(response))
      break;
    if (count == 0 || count > 0 ||
        (errno != EAGAIN && errno != EWOULDBLOCK) ||
        !wait_for_supervision_event(POLLIN, deadline))
      return fail_supervision();
  }
  if (response.magic != request.magic ||
      response.version != request.version ||
      response.type != request.type ||
      response.process_group != request.process_group ||
      response.sequence != request.sequence)
    return fail_supervision();
  if (response.status != 0) {
    if (type == NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_RELEASE &&
        response.status == ECANCELED)
      return FALSE;
    return fail_supervision();
  }
  now = network_sidebar_amneziawg_process_monotonic_msec();
  if (type != NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_RELEASE &&
      (now < 0 || now >= outer_deadline))
    return fail_supervision();
  return TRUE;
}

static gboolean
supervision_request(NetworkSidebarAmneziaWGSupervisionType type,
                    pid_t process_group,
                    gint64 deadline)
{
  gboolean acknowledged;

  if (supervision_request_in_progress)
    return fail_supervision();
  supervision_request_in_progress = TRUE;
  acknowledged = supervision_exchange(type, process_group, deadline);
  supervision_request_in_progress = FALSE;
  return acknowledged;
}

static gboolean
begin_group_termination(pid_t child, gint64 deadline)
{
  if (child != active_process_group && child != retained_process_group)
    return TRUE;
  /* Failure closes the channel: local destruction/reaping still runs, and the
   * service retains independent hard-cleanup authority over every group. */
  return supervision_request(NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_TERMINATING,
                              child, deadline);
}

gboolean
network_sidebar_amneziawg_process_open_start_gate(int gate[2])
{
  if (gate == NULL)
    return FALSE;
  gate[0] = -1;
  gate[1] = -1;
  return pipe2(gate, O_CLOEXEC) == 0;
}

gboolean
network_sidebar_amneziawg_process_prepare_child(
  const sigset_t *previous_mask,
  pid_t expected_parent,
  int gate_read_fd,
  int gate_write_fd)
{
  struct sigaction action = { 0 };
  sigset_t exec_mask;
  guint8 start = 0;
  ssize_t count;

  if (gate_write_fd >= 0)
    close(gate_write_fd);
  if (previous_mask == NULL || expected_parent <= 0 || gate_read_fd < 0 ||
      setpgid(0, 0) != 0)
    return FALSE;
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGTERM, &action, NULL) != 0 ||
      sigaction(SIGINT, &action, NULL) != 0 ||
      prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0 ||
      getppid() != expected_parent) {
    close(gate_read_fd);
    return FALSE;
  }
  if (process_supervision_fd >= 0)
    close(process_supervision_fd);
  if (termination_read_fd >= 0)
    close(termination_read_fd);
  if (atomic_load(&termination_write_fd) >= 0)
    close(atomic_load(&termination_write_fd));
  do
    count = read(gate_read_fd, &start, sizeof(start));
  while (count < 0 && errno == EINTR);
  close(gate_read_fd);
  if (count != (ssize_t) sizeof(start) || start != 1)
    return FALSE;
  exec_mask = *previous_mask;
  sigdelset(&exec_mask, SIGTERM);
  sigdelset(&exec_mask, SIGINT);
  return sigprocmask(SIG_SETMASK, &exec_mask, NULL) == 0;
}

gboolean
network_sidebar_amneziawg_process_register_group(
  pid_t child,
  const sigset_t *previous_mask,
  int gate_read_fd,
  int gate_write_fd,
  gint64 deadline)
{
  guint8 start = 1;
  gboolean registered = FALSE;
  gboolean started = FALSE;
  int group_error = 0;
  ssize_t count;

  if (gate_read_fd >= 0)
    close(gate_read_fd);
  if (child <= 0 || previous_mask == NULL || gate_write_fd < 0)
    goto out;
  if (setpgid(child, child) != 0 && errno != EACCES)
    group_error = errno;
  if (group_error == 0 &&
      supervision_request(NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_REGISTER,
                          child,
                          deadline)) {
    active_process_group = child;
    active_signal_level = 0;
    registered = TRUE;
    collect_pending_termination();
    if (!network_sidebar_amneziawg_process_termination_requested()) {
      do
        count = write(gate_write_fd, &start, sizeof(start));
      while (count < 0 && errno == EINTR);
      started = count == (ssize_t) sizeof(start);
    }
  }

out:
  close_fd(&gate_write_fd);
  if (previous_mask == NULL ||
      pthread_sigmask(SIG_SETMASK, previous_mask, NULL) != 0)
    started = FALSE;
  network_sidebar_amneziawg_process_dispatch_termination();
  if (group_error != 0)
    errno = group_error;
  return registered && started;
}

static gboolean
unregister_group(pid_t child, gint64 deadline)
{
  gboolean registered = active_process_group == child ||
                        retained_process_group == child;
  gboolean removed = FALSE;

  if (registered) {
    /* The service owns cleanup even if the ACK is lost. Once sent, this
     * request may release the sentinel that prevents numeric PGID reuse. */
    if (active_process_group == child) {
      active_process_group = -1;
      active_signal_level = 0;
    }
    if (retained_process_group == child) {
      retained_process_group = -1;
      retained_signal_level = 0;
    }
    removed = supervision_request(
      NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_UNREGISTER, child, deadline);
  }
  return !registered || removed;
}

static void
retain_group(pid_t child)
{
  retained_process_group = child;
  retained_signal_level = active_signal_level;
  if (active_process_group == child) {
    active_process_group = -1;
    active_signal_level = 0;
  }
}

static gboolean
reap_descendants(pid_t process_group,
                 guint grace_msec,
                 gint64 outer_deadline)
{
  gint64 now = network_sidebar_amneziawg_process_monotonic_msec();
  gint64 deadline;
  pid_t waited;
  int ignored_status;

  if (now < 0)
    return FALSE;
  deadline = network_sidebar_amneziawg_deadline_cap(now,
                                                      outer_deadline,
                                                      grace_msec);
  for (;;) {
    network_sidebar_amneziawg_process_dispatch_termination();
    do
      waited = waitpid(-process_group, &ignored_status, WNOHANG);
    while (waited < 0 && errno == EINTR);
    if (waited > 0)
      continue;
    if (waited < 0 && errno == ECHILD)
      return TRUE;
    if (waited < 0)
      return FALSE;
    if (deadline < 0)
      return FALSE;
    {
      gint64 now = network_sidebar_amneziawg_process_monotonic_msec();

      if (now < 0 || now >= deadline)
        return FALSE;
    }
    network_sidebar_amneziawg_process_poll(NULL, 0, 10);
  }
}

static gboolean
reap_child(pid_t child, gint64 deadline, int *status)
{
  pid_t waited;

  for (;;) {
    network_sidebar_amneziawg_process_dispatch_termination();
    do
      waited = waitpid(child, status, WNOHANG);
    while (waited < 0 && errno == EINTR);
    if (waited == child)
      break;
    if (waited < 0)
      return FALSE;
    {
      gint64 now = network_sidebar_amneziawg_process_monotonic_msec();

      if (now < 0 || now >= deadline)
        return FALSE;
    }
    network_sidebar_amneziawg_process_poll(NULL, 0, 10);
  }
  return TRUE;
}

static gboolean
reap_group(pid_t child, gint64 deadline, int *status)
{
  return reap_child(child, deadline, status) &&
         reap_descendants(child, GROUP_REAP_GRACE_MSEC, deadline);
}

static gboolean
peek_status(pid_t child, gint64 deadline, int *status)
{
  siginfo_t child_info = { 0 };
  int wait_result;

  for (;;) {
    network_sidebar_amneziawg_process_dispatch_termination();
    do
      wait_result = waitid(P_PID,
                           (id_t) child,
                           &child_info,
                           WEXITED | WNOHANG | WNOWAIT);
    while (wait_result != 0 && errno == EINTR);
    if (wait_result != 0)
      return FALSE;
    if (child_info.si_pid == child)
      break;
    {
      gint64 now = network_sidebar_amneziawg_process_monotonic_msec();

      if (now < 0 || now >= deadline)
        return FALSE;
    }
    network_sidebar_amneziawg_process_poll(NULL, 0, 10);
  }
  if (child_info.si_code == CLD_EXITED)
    *status = child_info.si_status << 8;
  else if (child_info.si_code == CLD_KILLED)
    *status = child_info.si_status & 0x7f;
  else if (child_info.si_code == CLD_DUMPED)
    *status = (child_info.si_status & 0x7f) | 0x80;
  else
    return FALSE;
  return TRUE;
}

gboolean
network_sidebar_amneziawg_process_finish_group(
  pid_t child,
  gboolean terminate_descendants,
  gint64 deadline,
  int *status)
{
  gboolean reaped;
  gboolean acknowledged = TRUE;

  if (child <= 0 || status == NULL)
    return FALSE;
  if (terminate_descendants) {
    acknowledged = begin_group_termination(child, deadline);
    kill(-child, SIGTERM);
    kill(child, SIGTERM);
    kill(-child, SIGKILL);
    kill(child, SIGKILL);
  }
  reaped = terminate_descendants ? reap_group(child, deadline, status) :
                                   peek_status(child, deadline, status);
  if (reaped && !terminate_descendants) {
    retain_group(child);
    return retained_process_group == child;
  }
  return reaped && unregister_group(child, deadline) && acknowledged;
}

gboolean
network_sidebar_amneziawg_process_release_retained_group(
  gint64 deadline,
  gint64 cleanup_deadline)
{
  sigset_t previous_mask;
  pid_t child = retained_process_group;
  gboolean committed = FALSE;
  gint64 now;

  if (!network_sidebar_amneziawg_process_block_termination(&previous_mask))
    return FALSE;
  now = network_sidebar_amneziawg_process_monotonic_msec();
  if (termination_is_pending() || now < 0 || now >= deadline ||
      (child > 0 && deadline - now <=
        NETWORK_SIDEBAR_AMNEZIAWG_CHILD_CLEANUP_RESERVE_MSEC))
    goto out;

  /* Keep the exited launcher waitable until commit. A failed attempt must leave
   * both the live backend and the exact child-reaping obligation for rollback. */
  if (child > 0 && !supervision_request(
        NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_RELEASE, child, deadline))
    goto out;
  committed = commit_operation(deadline);
  if (committed) {
    retained_process_group = -1;
    retained_signal_level = 0;
    if (child > 0) {
      pid_t waited;

      /* Reap ready children without turning a committed activation into a
       * rollback. Later exits remain owned by helper/service exit reaping. */
      do
        waited = waitpid(-child, NULL, WNOHANG);
      while (waited > 0 || (waited < 0 && errno == EINTR));
    }
  } else if (child > 0) {
    /* RELEASE only declares intent: the service retains the group and sentinel
     * until helper success. Withdraw that intent before rollback registers any
     * tools, even when the forward deadline has expired. An exchange failure
     * closes supervision and leaves hard cleanup to the service. */
    supervision_request(NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_WITHDRAW_RELEASE,
                        child, cleanup_deadline);
  }

out:
  network_sidebar_amneziawg_process_restore_mask(&previous_mask);
  network_sidebar_amneziawg_process_dispatch_termination();
  return committed;
}

gboolean
network_sidebar_amneziawg_process_terminate_retained_group(gint64 deadline)
{
  pid_t child = retained_process_group;
  int ignored_status;
  gboolean acknowledged;

  if (child <= 0)
    return TRUE;
  acknowledged = begin_group_termination(child, deadline);
  kill(-child, SIGTERM);
  kill(-child, SIGKILL);
  return reap_group(child, deadline, &ignored_status) &&
         unregister_group(child, deadline) && acknowledged;
}

gboolean
network_sidebar_amneziawg_process_terminate_group(
  pid_t child,
  guint grace_msec,
  gint64 outer_deadline,
  int *status)
{
  gint64 now;
  gint64 deadline;
  gboolean reaped;
  gboolean acknowledged;

  if (child <= 0 || status == NULL)
    return FALSE;
  acknowledged = begin_group_termination(child, outer_deadline);
  kill(-child, SIGTERM);
  kill(child, SIGTERM);
  if ((atomic_load(&termination_state) & TERMINATION_LEVEL_MASK) ==
      TERMINATION_ESCALATED)
    grace_msec = 0;
  now = network_sidebar_amneziawg_process_monotonic_msec();
  if (now < 0)
    return FALSE;
  deadline = network_sidebar_amneziawg_deadline_cap(now,
                                                     outer_deadline,
                                                     grace_msec);
  while (deadline >= 0) {
    gint64 now = network_sidebar_amneziawg_process_monotonic_msec();

    if (now < 0 || now >= deadline ||
        (atomic_load(&termination_state) & TERMINATION_LEVEL_MASK) ==
          TERMINATION_ESCALATED)
      break;
    network_sidebar_amneziawg_process_poll(NULL, 0, 25);
  }
  kill(-child, SIGKILL);
  kill(child, SIGKILL);
  reaped = reap_group(child, outer_deadline, status);
  return reaped && unregister_group(child, outer_deadline) && acknowledged;
}
