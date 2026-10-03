#ifndef NETWORK_SIDEBAR_AMNEZIAWG_ADMISSION_H
#define NETWORK_SIDEBAR_AMNEZIAWG_ADMISSION_H

#include "amneziawg/runtime_journal.h"

G_BEGIN_DECLS

typedef enum {
  NETWORK_SIDEBAR_AWG_DEPENDENCY_NONE = 0,
  NETWORK_SIDEBAR_AWG_DEPENDENCY_HELPER = 1 << 0,
  NETWORK_SIDEBAR_AWG_DEPENDENCY_SERVICE = 1 << 1,
  NETWORK_SIDEBAR_AWG_DEPENDENCY_AWG_QUICK = 1 << 2,
} NetworkSidebarAwgDependency;

typedef enum {
  NETWORK_SIDEBAR_AWG_ACTION_IMPORT,
  NETWORK_SIDEBAR_AWG_ACTION_REPLACE,
  NETWORK_SIDEBAR_AWG_ACTION_UP,
  NETWORK_SIDEBAR_AWG_ACTION_DOWN,
  NETWORK_SIDEBAR_AWG_ACTION_DELETE,
} NetworkSidebarAwgAction;

typedef enum {
  NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_NONE,
  NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_AUTHORIZATION,
  NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_BUSY,
  NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_CAPABILITIES,
  NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_DEPENDENCIES,
  NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_PROFILE_MISSING,
  NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_PROFILE_ACTIVATION,
  NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_PROFILE_REMOVAL,
  NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_RUNTIME_UNKNOWN,
  NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_CONFLICT,
  NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_MANUAL_CLEANUP,
  NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_ALREADY_IN_STATE,
} NetworkSidebarAwgActionBlockedReason;

typedef struct {
  gboolean authorized;
  gboolean busy;
  gboolean capabilities_known;
  NetworkSidebarAwgDependency missing_dependencies;
} NetworkSidebarAwgAdmissionContext;

typedef struct {
  gboolean has_profile;
  NetworkSidebarAmneziaWGProfileStatus profile_status;
  NetworkSidebarAmneziaWGRuntimeState runtime_state;
  gboolean interface_exists;
} NetworkSidebarAwgAdmissionTarget;

typedef struct {
  NetworkSidebarAwgAction action;
  /* Whether to offer this action even while temporarily blocked (e.g. a
   * removable saved profile while runtime state is being refreshed). */
  gboolean offered;
  gboolean allowed;
  NetworkSidebarAwgActionBlockedReason blocked_reason;
  NetworkSidebarAwgDependency missing_dependencies;
} NetworkSidebarAwgActionAvailability;

/* Pure policy: no profile reads, runtime refreshes, UI, or service calls. */
NetworkSidebarAwgAction network_sidebar_awg_primary_action(
  NetworkSidebarAmneziaWGRuntimeState runtime_state);
NetworkSidebarAwgActionAvailability network_sidebar_awg_action_evaluate(
  NetworkSidebarAwgAction action,
  const NetworkSidebarAwgAdmissionContext *context,
  const NetworkSidebarAwgAdmissionTarget *target);
/* Dispatch rechecks current capabilities. An already admitted firewall-only
 * Down retains its tool exemption if its cached runtime observation expires;
 * the privileged helper still validates the durable phase and link identity. */
NetworkSidebarAwgDependency network_sidebar_awg_action_missing_dependencies(
  NetworkSidebarAwgAction action,
  const NetworkSidebarAwgAdmissionContext *context,
  const NetworkSidebarAwgAdmissionTarget *target,
  gboolean runtime_cleanup_only);

G_END_DECLS

#endif
