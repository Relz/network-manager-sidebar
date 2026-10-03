#ifndef NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNNER_H
#define NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNNER_H

#include "amneziawg/profile_report.h"

G_BEGIN_DECLS

typedef struct _NetworkSidebarAwgHelperRunner NetworkSidebarAwgHelperRunner;

typedef enum {
  NETWORK_SIDEBAR_AWG_HELPER_STOP_SHUTDOWN,
  NETWORK_SIDEBAR_AWG_HELPER_STOP_PREEMPTED,
} NetworkSidebarAwgHelperStopReason;

typedef struct {
  NetworkSidebarAmneziaWGHelperExit status;
  guint capabilities;
  /* Borrowed for the result callback; NULL on failure or a non-inventory run. */
  const NetworkSidebarAwgProfileReport *profiles;
} NetworkSidebarAwgHelperResult;

typedef struct {
  /* Exactly once per accepted start. Inventory may report cancellation/timeout
   * before cleanup finishes. Receiving a result does not release ownership. */
  void (*result)(const NetworkSidebarAwgHelperResult *result, gpointer user_data);
  /* Execution finished, pending reaps progressed, or a fatal runner error.
   * Query is_running/is_busy/failed before admitting more work. */
  void (*changed)(gpointer user_data);
} NetworkSidebarAwgHelperRunnerCallbacks;

/* One main-context-owned runner per service process. Installs the subreaper and
 * child signal disposition required to supervise and reap helper descendants.
 * It has no D-Bus or authorization responsibilities. */
NetworkSidebarAwgHelperRunner *network_sidebar_awg_helper_runner_new(
  const NetworkSidebarAwgHelperRunnerCallbacks *callbacks,
  gpointer user_data);
/* Stop and drain busy work before freeing. Released active backends survive. */
void network_sidebar_awg_helper_runner_free(NetworkSidebarAwgHelperRunner *runner);

/* The caller must authorize/admit the operation first. Inputs are copied.
 * FALSE means busy/failed, with no callbacks. An accepted start can complete
 * synchronously on setup failure; callbacks may start the next admitted run. */
gboolean network_sidebar_awg_helper_runner_start(
  NetworkSidebarAwgHelperRunner *runner,
  const char *operation,
  const char *name,
  const guint8 *config,
  gsize config_length);
void network_sidebar_awg_helper_runner_stop(
  NetworkSidebarAwgHelperRunner *runner,
  NetworkSidebarAwgHelperStopReason reason);

gboolean network_sidebar_awg_helper_runner_is_running(
  const NetworkSidebarAwgHelperRunner *runner);
/* Also polls nonblocking exact-PID reaps. Retired children block admission;
 * successfully released backend groups do not. */
gboolean network_sidebar_awg_helper_runner_is_busy(NetworkSidebarAwgHelperRunner *runner);
gboolean network_sidebar_awg_helper_runner_failed(const NetworkSidebarAwgHelperRunner *runner);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarAwgHelperRunner,
                              network_sidebar_awg_helper_runner_free)

G_END_DECLS

#endif
