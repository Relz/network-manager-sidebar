#ifndef NETWORK_SIDEBAR_AMNEZIAWG_PROCESS_H
#define NETWORK_SIDEBAR_AMNEZIAWG_PROCESS_H

#include "helper/amneziawg_supervision_protocol.h"

#include <glib.h>

#include <poll.h>
#include <signal.h>
#include <sys/types.h>

/* Initialize once, before creating workers. Signal descriptors live until exit. */
gboolean network_sidebar_amneziawg_process_install_handlers(void);
/* The absolute cleanup deadline also bounds escalation acknowledgements. */
gboolean network_sidebar_amneziawg_process_init(int supervision_fd,
                                               gint64 cleanup_deadline);
/* Pure, thread-safe queries; they never signal or inspect process groups. */
gboolean network_sidebar_amneziawg_process_termination_requested(void);
gboolean network_sidebar_amneziawg_process_is_committed(void);
/* All remaining process operations belong to the helper's operation thread.
 * Poll dispatches cancellation and returns -1/EINTR for a signal wakeup. */
void network_sidebar_amneziawg_process_dispatch_termination(void);
int network_sidebar_amneziawg_process_poll(struct pollfd *fds,
                                          nfds_t count,
                                          int timeout_msec);
void network_sidebar_amneziawg_process_set_cleanup_mode(gboolean enabled);
gboolean network_sidebar_amneziawg_process_block_termination(sigset_t *previous_mask);
gboolean network_sidebar_amneziawg_process_open_start_gate(int gate[2]);
gboolean network_sidebar_amneziawg_process_prepare_child(
  const sigset_t *previous_mask,
  pid_t expected_parent,
  int gate_read_fd,
  int gate_write_fd);
gboolean network_sidebar_amneziawg_process_register_group(
  pid_t child,
  const sigset_t *previous_mask,
  int gate_read_fd,
  int gate_write_fd,
  gint64 deadline);
void network_sidebar_amneziawg_process_restore_mask(
  const sigset_t *previous_mask);
gboolean network_sidebar_amneziawg_process_finish_group(
  pid_t child,
  gboolean terminate_descendants,
  gint64 deadline,
  int *status);
/* Commit activation, withdrawing an acknowledged release within the cleanup
 * budget if local commit fails. Failure preserves the retained backend for
 * rollback; escalation and supervisor hard cleanup remain authoritative. */
gboolean network_sidebar_amneziawg_process_release_retained_group(
  gint64 deadline,
  gint64 cleanup_deadline);
/* The transaction calls this after rollback, never as part of a commit attempt. */
gboolean network_sidebar_amneziawg_process_terminate_retained_group(
  gint64 deadline);
gboolean network_sidebar_amneziawg_process_terminate_group(
  pid_t child,
  guint grace_msec,
  gint64 deadline,
  int *status);
gint64 network_sidebar_amneziawg_process_monotonic_msec(void);

#endif
