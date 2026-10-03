#include "amneziawg/admission.h"

static gboolean
runtime_can_disconnect(NetworkSidebarAmneziaWGRuntimeState state)
{
  return state == NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_ACTIVE ||
         network_sidebar_amneziawg_runtime_state_is_recoverable(state);
}

NetworkSidebarAwgAction
network_sidebar_awg_primary_action(NetworkSidebarAmneziaWGRuntimeState state)
{
  return runtime_can_disconnect(state) ||
         state == NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_MANUAL_INTERVENTION ?
    NETWORK_SIDEBAR_AWG_ACTION_DOWN : NETWORK_SIDEBAR_AWG_ACTION_UP;
}

NetworkSidebarAwgDependency
network_sidebar_awg_action_missing_dependencies(
  NetworkSidebarAwgAction action,
  const NetworkSidebarAwgAdmissionContext *context,
  const NetworkSidebarAwgAdmissionTarget *target,
  gboolean runtime_cleanup_only)
{
  NetworkSidebarAwgDependency missing = context->missing_dependencies;

  if (action == NETWORK_SIDEBAR_AWG_ACTION_DOWN) {
    /* Runtime-only cleanup must not wait for the first capability inventory. */
    if (!context->capabilities_known)
      return missing & NETWORK_SIDEBAR_AWG_DEPENDENCY_SERVICE;
    if (runtime_cleanup_only ||
        (target != NULL &&
         target->runtime_state != NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_UNKNOWN &&
         !target->interface_exists))
      missing &= ~NETWORK_SIDEBAR_AWG_DEPENDENCY_AWG_QUICK;
  } else if (action == NETWORK_SIDEBAR_AWG_ACTION_DELETE &&
             target != NULL &&
             target->runtime_state != NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_UNKNOWN &&
             !target->interface_exists) {
    /* Removal uses Down first. Only an absent link exempts it from awg-quick;
     * dispatch must recheck this rather than retain a stale cleanup intent. */
    missing &= ~NETWORK_SIDEBAR_AWG_DEPENDENCY_AWG_QUICK;
  }
  return missing;
}

static NetworkSidebarAwgActionBlockedReason
dependency_blocked_reason(const NetworkSidebarAwgAdmissionContext *context,
                          NetworkSidebarAwgDependency missing)
{
  if (missing == NETWORK_SIDEBAR_AWG_DEPENDENCY_NONE)
    return NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_NONE;
  if (!context->capabilities_known &&
      (missing & NETWORK_SIDEBAR_AWG_DEPENDENCY_SERVICE) == 0)
    return NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_CAPABILITIES;
  return NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_DEPENDENCIES;
}

static NetworkSidebarAwgActionBlockedReason
action_blocked_reason(NetworkSidebarAwgAction action,
                      const NetworkSidebarAwgAdmissionContext *context,
                      const NetworkSidebarAwgAdmissionTarget *target,
                      NetworkSidebarAwgDependency missing)
{
  if (!context->authorized)
    return NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_AUTHORIZATION;
  if (context->busy)
    return NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_BUSY;
  if (action == NETWORK_SIDEBAR_AWG_ACTION_IMPORT ||
      action == NETWORK_SIDEBAR_AWG_ACTION_REPLACE)
    return dependency_blocked_reason(context, missing);

  if (action == NETWORK_SIDEBAR_AWG_ACTION_UP) {
    if (target == NULL || !target->has_profile)
      return NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_PROFILE_MISSING;
    if (target->profile_status != NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_AVAILABLE)
      return NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_PROFILE_ACTIVATION;
  } else if (action == NETWORK_SIDEBAR_AWG_ACTION_DELETE) {
    if (target == NULL || !target->has_profile ||
        !network_sidebar_amneziawg_profile_status_allows_removal(target->profile_status))
      return NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_PROFILE_REMOVAL;
  }
  if (target == NULL ||
      target->runtime_state == NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_UNKNOWN)
    return NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_RUNTIME_UNKNOWN;
  if (target->runtime_state == NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_CONFLICT)
    return NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_CONFLICT;
  if (target->runtime_state == NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_MANUAL_INTERVENTION)
    return NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_MANUAL_CLEANUP;
  if (action == NETWORK_SIDEBAR_AWG_ACTION_DELETE)
    return dependency_blocked_reason(context, missing);
  if (runtime_can_disconnect(target->runtime_state) ==
      (action == NETWORK_SIDEBAR_AWG_ACTION_UP))
    return NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_ALREADY_IN_STATE;
  return dependency_blocked_reason(context, missing);
}

NetworkSidebarAwgActionAvailability
network_sidebar_awg_action_evaluate(
  NetworkSidebarAwgAction action,
  const NetworkSidebarAwgAdmissionContext *context,
  const NetworkSidebarAwgAdmissionTarget *target)
{
  NetworkSidebarAwgActionAvailability availability = { .action = action };

  switch (action) {
  case NETWORK_SIDEBAR_AWG_ACTION_IMPORT:
  case NETWORK_SIDEBAR_AWG_ACTION_REPLACE:
    availability.offered = TRUE;
    break;
  case NETWORK_SIDEBAR_AWG_ACTION_UP:
    availability.offered = target != NULL && target->has_profile &&
      target->profile_status == NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_AVAILABLE;
    break;
  case NETWORK_SIDEBAR_AWG_ACTION_DOWN:
    availability.offered = target != NULL && runtime_can_disconnect(target->runtime_state);
    break;
  case NETWORK_SIDEBAR_AWG_ACTION_DELETE:
    availability.offered = target != NULL && target->has_profile &&
      network_sidebar_amneziawg_profile_status_allows_removal(target->profile_status);
    break;
  }
  availability.missing_dependencies = network_sidebar_awg_action_missing_dependencies(
    action, context, target, FALSE);
  availability.blocked_reason = action_blocked_reason(
    action, context, target, availability.missing_dependencies);
  availability.allowed = availability.blocked_reason == NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_NONE;
  return availability;
}
