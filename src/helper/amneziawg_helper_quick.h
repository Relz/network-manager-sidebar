#ifndef NETWORK_SIDEBAR_AMNEZIAWG_HELPER_QUICK_H
#define NETWORK_SIDEBAR_AMNEZIAWG_HELPER_QUICK_H

#include "amneziawg/amneziawg.h"

typedef enum {
  /* The invocation failed before a child could execute the tool. */
  AWG_QUICK_NOT_STARTED,
  /* Conservative after fork, even for setup errors or an exit status of 127. */
  AWG_QUICK_MAY_HAVE_RUN,
} AwgQuickExecution;

typedef struct {
  NetworkSidebarAmneziaWGHelperExit status;
  AwgQuickExecution execution;
  /* Complete, recognized failed-up trace ended before DNS/routes and included
   * link teardown. This does not itself establish absence of any artifacts. */
  gboolean failed_before_routes;
} AwgQuickResult;

AwgQuickResult awg_helper_quick_run(
  const char *command,
  const char *config_path,
  guint8 **standard_output,
  gsize *standard_output_length,
  gint64 outer_deadline);

#endif
