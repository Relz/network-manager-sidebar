#ifndef NETWORK_SIDEBAR_AMNEZIAWG_QUICK_DIAGNOSTICS_H
#define NETWORK_SIDEBAR_AMNEZIAWG_QUICK_DIAGNOSTICS_H

#include "amneziawg/amneziawg.h"
#include "helper/amneziawg_subprocess.h"

typedef enum {
  AWG_QUICK_STAGE_NONE,
  AWG_QUICK_STAGE_INTERFACE,
  AWG_QUICK_STAGE_USERSPACE,
  AWG_QUICK_STAGE_SETCONF,
  AWG_QUICK_STAGE_ADDRESS,
  AWG_QUICK_STAGE_MTU,
  AWG_QUICK_STAGE_DNS,
  AWG_QUICK_STAGE_ROUTE,
  AWG_QUICK_STAGE_FIREWALL,
} AwgQuickStage;

#define AWG_QUICK_DIAGNOSTIC_LINE_LIMIT (16u * 1024u)

/* Zero-initialized, bounded tool-adapter state; never returns raw diagnostics. */
typedef struct {
  guint8 line[AWG_QUICK_DIAGNOSTIC_LINE_LIMIT];
  gsize length;
  gboolean discarding;
  gboolean userspace_started;
  AwgQuickStage current_stage;
  AwgQuickStage failure_candidate;
  gboolean interface_seen;
  gboolean teardown_seen;
  gboolean recovery_blocked;
} AwgQuickDiagnostics;

void awg_quick_diagnostics_consume(const guint8 *data, gsize length,
                                  gpointer user_data);
void awg_quick_diagnostics_finish(AwgQuickDiagnostics *diagnostics);
NetworkSidebarAmneziaWGHelperExit awg_quick_diagnostics_failure_status(
  const AwgQuickDiagnostics *diagnostics);
/* Only evidence about this failed invocation's phase. The transaction must
 * independently verify link/firewall absence and that DNS was never applied. */
gboolean awg_quick_diagnostics_failed_before_routes(
  const AwgQuickDiagnostics *diagnostics,
  const AwgSubprocessResult *child);

#endif
