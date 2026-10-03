#ifndef NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISOR_H
#define NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISOR_H

#include <glib.h>

#include <signal.h>
#include <sys/types.h>

typedef struct _NetworkSidebarAwgSupervisor NetworkSidebarAwgSupervisor;

NetworkSidebarAwgSupervisor *network_sidebar_awg_supervisor_new(
  int fd,
  gboolean allow_release);
/* Retired sentinels must be reaped or detached before freeing the supervisor. */
void network_sidebar_awg_supervisor_free(
  NetworkSidebarAwgSupervisor *supervisor);

gboolean network_sidebar_awg_supervisor_poll(
  NetworkSidebarAwgSupervisor *supervisor);
void network_sidebar_awg_supervisor_disable_release(
  NetworkSidebarAwgSupervisor *supervisor);
gboolean network_sidebar_awg_supervisor_release_pending(
  const NetworkSidebarAwgSupervisor *supervisor);
void network_sidebar_awg_supervisor_begin_cleanup(
  NetworkSidebarAwgSupervisor *supervisor);
void network_sidebar_awg_supervisor_signal_groups(
  NetworkSidebarAwgSupervisor *supervisor,
  int signal_number);
void network_sidebar_awg_supervisor_poll_cleanup(
  NetworkSidebarAwgSupervisor *supervisor);
/* Transfers exact-PID reaping obligations before teardown. These children
 * have already been sent SIGKILL; never signal their former process groups. */
void network_sidebar_awg_supervisor_detach_retired_sentinels(
  NetworkSidebarAwgSupervisor *supervisor,
  GArray *child_pids);
void network_sidebar_awg_supervisor_detach_terminated_groups(
  NetworkSidebarAwgSupervisor *supervisor,
  GArray *process_groups);
pid_t network_sidebar_awg_supervisor_finalize_release(
  NetworkSidebarAwgSupervisor *supervisor);
void network_sidebar_awg_supervisor_abort(
  NetworkSidebarAwgSupervisor *supervisor);

gboolean network_sidebar_awg_supervisor_channel_closed(
  const NetworkSidebarAwgSupervisor *supervisor);
gboolean network_sidebar_awg_supervisor_failed(
  const NetworkSidebarAwgSupervisor *supervisor);
/* Reports logical group ownership, excluding retired sentinel children. */
gboolean network_sidebar_awg_supervisor_groups_empty(
  const NetworkSidebarAwgSupervisor *supervisor);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarAwgSupervisor,
                              network_sidebar_awg_supervisor_free)

#endif
