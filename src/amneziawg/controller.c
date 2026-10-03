#include "amneziawg/controller.h"

#include "amneziawg/admission.h"
#include "amneziawg/backend.h"
#include "amneziawg/controller_internal.h"
#include "amneziawg/inventory.h"
#include "amneziawg/runtime_source.h"

#define AWG_INVENTORY_RECHECK_DELAY_MSEC 2000u

typedef struct {
  NetworkSidebarAwgController *controller;
  guint64 activity_id;
  char *name;
  GBytes *config;
  NetworkSidebarAwgBackendOperation operation;
  gboolean runtime_cleanup_only;
} AwgServiceAction;

struct _NetworkSidebarAwgController {
  gint ref_count;
  NetworkSidebarAwgChangedCallback changed;
  gpointer changed_data;
  NetworkSidebarAwgUiEventCallback ui_event;
  gpointer ui_event_data;

  NetworkSidebarAwgModel *model;
  NetworkSidebarAwgBackend *backend;
  NetworkSidebarAwgInventory *inventory;
  NetworkSidebarAwgRuntimeSource *runtime_source;
  gboolean authorized;

  NetworkSidebarAwgActivity activity;
  char *activity_name;
  guint64 activity_id;
  guint64 generation;
  AwgServiceAction *pending_confirmation;
  gboolean runtime_verification_pending;

  gboolean started;
  gboolean stopping;
};

static void dispatch_service_action(AwgServiceAction *action);
static void service_action_free(AwgServiceAction *action);
static gboolean operation_is_allowed(
  NetworkSidebarAwgController *controller,
  NetworkSidebarAwgBackendOperation operation,
  const char *name,
  gboolean continuing_activity,
  NetworkSidebarAwgAdmissionTarget *target_out);

static gboolean
controller_is_running(NetworkSidebarAwgController *controller)
{
  return controller != NULL && controller->started && !controller->stopping;
}

static void
sync_runtime_profile_names(NetworkSidebarAwgController *controller,
                           NetworkSidebarAwgRefreshPurpose purpose)
{
  g_autoptr(GPtrArray) names =
    network_sidebar_awg_model_dup_profile_names(controller->model);

  network_sidebar_awg_runtime_source_set_names(controller->runtime_source,
                                                 names, purpose);
}

static void
rebuild_entries(NetworkSidebarAwgController *controller)
{
  g_autoptr(GPtrArray) runtime_records = NULL;

  if (controller->authorized)
    runtime_records = network_sidebar_awg_runtime_source_dup_cached_records(
      controller->runtime_source);
  network_sidebar_awg_model_rebuild_entries(
    controller->model,
    controller->authorized,
    runtime_records,
    network_sidebar_awg_runtime_source_is_checking(controller->runtime_source),
    controller->activity,
    controller->activity_name);
}

static void
emit_changed(NetworkSidebarAwgController *controller, guint delay_ms)
{
  NetworkSidebarAwgChangedCallback callback;
  gpointer user_data;

  if (!controller_is_running(controller))
    return;
  if (controller->generation != G_MAXUINT64)
    controller->generation++;
  callback = controller->changed;
  user_data = controller->changed_data;
  if (callback == NULL)
    return;

  network_sidebar_awg_controller_ref(controller);
  callback(delay_ms, user_data);
  network_sidebar_awg_controller_unref(controller);
}

static gboolean
emit_ui_event(NetworkSidebarAwgController *controller,
              const NetworkSidebarAwgUiEvent *event)
{
  NetworkSidebarAwgUiEventCallback callback;
  gpointer user_data;
  gboolean handled;

  if (!controller_is_running(controller))
    return FALSE;
  callback = controller->ui_event;
  user_data = controller->ui_event_data;
  if (callback == NULL)
    return FALSE;

  network_sidebar_awg_controller_ref(controller);
  handled = callback(event, user_data);
  network_sidebar_awg_controller_unref(controller);
  return handled;
}

static gboolean
emit_ui_request(NetworkSidebarAwgController *controller,
                NetworkSidebarAwgUiEventType type,
                guint64 activity_id,
                const char *name,
                gboolean availability_probe)
{
  NetworkSidebarAwgUiEvent event = {
    .type = type,
    .activity_id = activity_id,
    .name = name,
    .availability_probe = availability_probe,
  };

  return emit_ui_event(controller, &event);
}

static void
emit_notice(NetworkSidebarAwgController *controller,
            NetworkSidebarAwgNotice notice,
            NetworkSidebarAwgBackendOperation operation,
            guint helper_status,
            NetworkSidebarAwgImportError import_error,
            const char *name)
{
  NetworkSidebarAwgUiEvent event = {
    .type = NETWORK_SIDEBAR_AWG_UI_NOTICE,
    .notice = notice,
    .operation = operation,
    .import_error = import_error,
    .helper_status = helper_status,
    .name = name,
  };

  emit_ui_event(controller, &event);
}

static gboolean
activity_matches(NetworkSidebarAwgController *controller, guint64 activity_id)
{
  return controller_is_running(controller) &&
         controller->activity != NETWORK_SIDEBAR_AWG_ACTIVITY_IDLE &&
         controller->activity_id == activity_id;
}

static gboolean
confirmation_matches(NetworkSidebarAwgController *controller,
                     NetworkSidebarAwgUiEventType confirmation,
                     guint64 activity_id)
{
  if (!activity_matches(controller, activity_id) ||
      controller->pending_confirmation == NULL ||
      controller->pending_confirmation->activity_id != activity_id)
    return FALSE;

  switch (confirmation) {
  case NETWORK_SIDEBAR_AWG_UI_CONFIRM_REPLACE:
    return controller->activity ==
             NETWORK_SIDEBAR_AWG_ACTIVITY_REPLACE_CONFIRM &&
           controller->pending_confirmation->operation ==
             NETWORK_SIDEBAR_AWG_BACKEND_IMPORT;
  case NETWORK_SIDEBAR_AWG_UI_CONFIRM_DELETE:
    return controller->activity ==
             NETWORK_SIDEBAR_AWG_ACTIVITY_DELETE_CONFIRM &&
           controller->pending_confirmation->operation ==
             NETWORK_SIDEBAR_AWG_BACKEND_DELETE;
  default:
    return FALSE;
  }
}

static gboolean
begin_activity(NetworkSidebarAwgController *controller,
               NetworkSidebarAwgBackendOperation operation,
               NetworkSidebarAwgActivity activity,
               const char *name)
{
  guint64 activity_id;

  if (!controller_is_running(controller) ||
      activity == NETWORK_SIDEBAR_AWG_ACTIVITY_IDLE ||
      controller->activity != NETWORK_SIDEBAR_AWG_ACTIVITY_IDLE ||
      controller->activity_id == G_MAXUINT64)
    return FALSE;
  /* UI availability probes may have re-entered the controller since the
   * initial request. Re-evaluate current facts before reserving an activity. */
  if (!operation_is_allowed(controller, operation, name, FALSE, NULL))
    return FALSE;

  activity_id = ++controller->activity_id;
  controller->activity = activity;
  g_free(controller->activity_name);
  controller->activity_name = g_strdup(name);
  if (activity == NETWORK_SIDEBAR_AWG_ACTIVITY_UP)
    network_sidebar_awg_model_begin_switch(controller->model);
  /* Selection/confirmation may take time or be cancelled. Let the current
   * inventory finish until an actual command is ready to supersede it. */
  network_sidebar_awg_inventory_set_suspended(controller->inventory, TRUE);
  rebuild_entries(controller);
  emit_changed(controller, 1);
  return activity_matches(controller, activity_id) &&
         controller->activity == activity;
}

static gboolean
transition_activity(NetworkSidebarAwgController *controller,
                    guint64 activity_id,
                    NetworkSidebarAwgActivity activity,
                    const char *name)
{
  if (!activity_matches(controller, activity_id) ||
      activity == NETWORK_SIDEBAR_AWG_ACTIVITY_IDLE)
    return FALSE;

  controller->activity = activity;
  g_free(controller->activity_name);
  controller->activity_name = g_strdup(name);
  rebuild_entries(controller);
  emit_changed(controller, 1);
  return activity_matches(controller, activity_id) &&
         controller->activity == activity;
}

static gboolean
finish_activity(NetworkSidebarAwgController *controller,
                guint64 activity_id,
                guint changed_delay_ms)
{
  if (!activity_matches(controller, activity_id))
    return FALSE;

  controller->activity = NETWORK_SIDEBAR_AWG_ACTIVITY_IDLE;
  controller->runtime_verification_pending = FALSE;
  g_clear_pointer(&controller->activity_name, g_free);
  rebuild_entries(controller);
  emit_changed(controller, changed_delay_ms);
  if (!controller_is_running(controller))
    return TRUE;
  network_sidebar_awg_runtime_source_ensure_monitor(
    controller->runtime_source);
  if (controller->activity == NETWORK_SIDEBAR_AWG_ACTIVITY_IDLE)
    network_sidebar_awg_inventory_set_suspended(controller->inventory, FALSE);
  return TRUE;
}

static void
set_authorized(NetworkSidebarAwgController *controller, gboolean authorized)
{
  gboolean changed = controller->authorized != authorized;
  gboolean authorization_lost = changed && !authorized;
  gboolean cancelled_interaction = FALSE;
  NetworkSidebarAwgBackendOperation operation =
    controller->activity == NETWORK_SIDEBAR_AWG_ACTIVITY_DELETE_CONFIRM ?
      NETWORK_SIDEBAR_AWG_BACKEND_DELETE : NETWORK_SIDEBAR_AWG_BACKEND_IMPORT;

  controller->authorized = authorized;
  if (!authorized) {
    switch (controller->activity) {
    case NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_SELECT:
    case NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_LOAD:
    case NETWORK_SIDEBAR_AWG_ACTIVITY_REPLACE_CONFIRM:
    case NETWORK_SIDEBAR_AWG_ACTIVITY_DELETE_CONFIRM:
      /* Invalidate ownership before cancellation or UI callbacks can respond. */
      controller->activity = NETWORK_SIDEBAR_AWG_ACTIVITY_IDLE;
      g_clear_pointer(&controller->activity_name, g_free);
      g_clear_pointer(&controller->pending_confirmation, service_action_free);
      cancelled_interaction = TRUE;
      changed = TRUE;
      break;
    default:
      break;
    }
    network_sidebar_awg_runtime_source_set_monitor_enabled(
      controller->runtime_source, FALSE);
    if (controller->runtime_verification_pending)
      finish_activity(controller, controller->activity_id, 1);
    network_sidebar_awg_inventory_set_authorized(controller->inventory, FALSE);
    network_sidebar_awg_inventory_set_suspended(
      controller->inventory,
      controller->activity != NETWORK_SIDEBAR_AWG_ACTIVITY_IDLE);
    if (authorization_lost)
      emit_ui_request(controller,
                      NETWORK_SIDEBAR_AWG_UI_DISMISS_ALL,
                      0,
                      NULL,
                      FALSE);
    if (cancelled_interaction && !controller->authorized)
      emit_notice(controller,
                  NETWORK_SIDEBAR_AWG_NOTICE_AUTHORIZATION_UNAVAILABLE,
                  operation,
                  0,
                  NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
                  NULL);
  }

  if (authorized)
    network_sidebar_awg_inventory_set_authorized(controller->inventory, TRUE);
  if (!changed)
    return;
  rebuild_entries(controller);
  emit_changed(controller, 1);
  if (!controller_is_running(controller) || !authorized ||
      !controller->authorized)
    return;
  network_sidebar_awg_runtime_source_set_monitor_enabled(
    controller->runtime_source, TRUE);
  network_sidebar_awg_controller_request_inventory(controller, TRUE);
}

static void
backend_authorization_changed(gboolean authorized, gpointer user_data)
{
  NetworkSidebarAwgController *controller = user_data;

  network_sidebar_awg_controller_ref(controller);
  if (controller_is_running(controller))
    set_authorized(controller, authorized);
  network_sidebar_awg_controller_unref(controller);
}

static void
runtime_source_changed(gpointer user_data)
{
  NetworkSidebarAwgController *controller = user_data;

  network_sidebar_awg_controller_ref(controller);
  if (controller_is_running(controller) &&
      controller->runtime_verification_pending &&
      (!controller->authorized ||
       !network_sidebar_awg_runtime_source_is_checking(controller->runtime_source))) {
    /* End progress on the first completed attempt, including an observation
     * failure. Retry backoff must not conceal the resulting runtime state. */
    finish_activity(controller, controller->activity_id, 1);
    network_sidebar_awg_controller_unref(controller);
    return;
  }
  if (controller_is_running(controller) && controller->authorized) {
    rebuild_entries(controller);
    emit_changed(controller, 1);
  }
  network_sidebar_awg_controller_unref(controller);
}

static void
inventory_changed(NetworkSidebarAwgInventoryChange change,
                  NetworkSidebarAwgRefreshPurpose purpose, gpointer user_data)
{
  g_autoptr(NetworkSidebarAwgController) controller =
    network_sidebar_awg_controller_ref(user_data);

  if (!controller_is_running(controller))
    return;
  if (change == NETWORK_SIDEBAR_AWG_INVENTORY_PROFILES_CHANGED)
    sync_runtime_profile_names(controller, purpose);
  else if (change == NETWORK_SIDEBAR_AWG_INVENTORY_REFRESH_STARTED)
    network_sidebar_awg_runtime_source_poll(controller->runtime_source, purpose);
  rebuild_entries(controller);
  emit_changed(controller, 1);
}

static gboolean
inventory_can_dispatch(gpointer user_data)
{
  g_autoptr(NetworkSidebarAwgController) controller =
    network_sidebar_awg_controller_ref(user_data);

  return controller_is_running(controller) && controller->authorized &&
         !emit_ui_request(controller,
                          NETWORK_SIDEBAR_AWG_UI_EXTERNAL_INTERACTION_ACTIVE,
                          0, NULL, FALSE);
}

static NetworkSidebarAwgAction
operation_action(NetworkSidebarAwgBackendOperation operation)
{
  switch (operation) {
  case NETWORK_SIDEBAR_AWG_BACKEND_IMPORT:
    return NETWORK_SIDEBAR_AWG_ACTION_IMPORT;
  case NETWORK_SIDEBAR_AWG_BACKEND_REPLACE:
    return NETWORK_SIDEBAR_AWG_ACTION_REPLACE;
  case NETWORK_SIDEBAR_AWG_BACKEND_UP:
    return NETWORK_SIDEBAR_AWG_ACTION_UP;
  case NETWORK_SIDEBAR_AWG_BACKEND_DOWN:
    return NETWORK_SIDEBAR_AWG_ACTION_DOWN;
  case NETWORK_SIDEBAR_AWG_BACKEND_DELETE:
    return NETWORK_SIDEBAR_AWG_ACTION_DELETE;
  case NETWORK_SIDEBAR_AWG_BACKEND_LIST_PROFILES:
  default:
    g_assert_not_reached();
  }
}

static NetworkSidebarAwgAdmissionContext
admission_context(NetworkSidebarAwgController *controller,
                  gboolean continuing_activity)
{
  return (NetworkSidebarAwgAdmissionContext) {
    .authorized = controller_is_running(controller) && controller->authorized,
    /* Only a matching activity/confirmation owner may continue its own work. */
    .busy = !continuing_activity && controller->activity != NETWORK_SIDEBAR_AWG_ACTIVITY_IDLE,
    .capabilities_known = network_sidebar_awg_inventory_capabilities_known(controller->inventory),
    .missing_dependencies = network_sidebar_awg_inventory_missing_dependencies(controller->inventory),
  };
}

static NetworkSidebarAwgAdmissionTarget
admission_target(NetworkSidebarAwgController *controller, const char *name)
{
  NetworkSidebarAwgAdmissionTarget target = {
    .profile_status = NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_CHANGED_OR_UNREADABLE,
    .runtime_state = NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_UNKNOWN,
  };

  if (name != NULL) {
    target.has_profile = network_sidebar_awg_model_get_profile_status(
      controller->model, name, &target.profile_status);
    network_sidebar_awg_runtime_source_get_cached(
      controller->runtime_source, name, &target.runtime_state, &target.interface_exists);
  }
  return target;
}

static NetworkSidebarAwgDependency
operation_missing_dependencies(NetworkSidebarAwgController *controller,
                               NetworkSidebarAwgBackendOperation operation,
                               const char *name,
                               gboolean runtime_cleanup_only)
{
  NetworkSidebarAwgAdmissionContext context = admission_context(controller, TRUE);
  NetworkSidebarAwgAdmissionTarget target = admission_target(controller, name);

  return network_sidebar_awg_action_missing_dependencies(
    operation_action(operation), &context, &target, runtime_cleanup_only);
}

static gboolean
operation_is_allowed(NetworkSidebarAwgController *controller,
                     NetworkSidebarAwgBackendOperation operation,
                     const char *name,
                     gboolean continuing_activity,
                     NetworkSidebarAwgAdmissionTarget *target_out)
{
  NetworkSidebarAwgAdmissionContext context = admission_context(controller, continuing_activity);
  NetworkSidebarAwgAdmissionTarget target = admission_target(controller, name);
  NetworkSidebarAwgActionAvailability availability = network_sidebar_awg_action_evaluate(
    operation_action(operation), &context, &target);
  NetworkSidebarAwgNotice notice = NETWORK_SIDEBAR_AWG_NOTICE_NONE;
  gboolean refresh_profiles = FALSE;
  gboolean refresh_runtime = FALSE;
  gboolean refresh_entries = FALSE;

  if (target_out != NULL)
    *target_out = target;
  if (availability.allowed)
    return TRUE;

  /* The policy only describes why admission failed. The controller owns the
   * resulting notices and refresh scheduling. */
  switch (availability.blocked_reason) {
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_AUTHORIZATION:
    notice = NETWORK_SIDEBAR_AWG_NOTICE_AUTHORIZATION_UNAVAILABLE;
    break;
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_CAPABILITIES:
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_DEPENDENCIES:
    notice = NETWORK_SIDEBAR_AWG_NOTICE_REQUIRED_COMPONENTS_UNAVAILABLE;
    break;
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_PROFILE_MISSING:
    notice = NETWORK_SIDEBAR_AWG_NOTICE_PROFILE_MISSING;
    refresh_profiles = TRUE;
    break;
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_PROFILE_ACTIVATION:
    notice = NETWORK_SIDEBAR_AWG_NOTICE_PROFILE_ACTIVATION_BLOCKED;
    break;
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_PROFILE_REMOVAL:
    notice = NETWORK_SIDEBAR_AWG_NOTICE_PROFILE_REMOVAL_BLOCKED;
    refresh_profiles = TRUE;
    break;
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_RUNTIME_UNKNOWN:
    notice = NETWORK_SIDEBAR_AWG_NOTICE_WAIT_FOR_RUNTIME_REFRESH;
    refresh_runtime = TRUE;
    break;
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_CONFLICT:
    notice = NETWORK_SIDEBAR_AWG_NOTICE_INTERFACE_EXTERNALLY_MANAGED;
    refresh_entries = TRUE;
    break;
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_MANUAL_CLEANUP:
    notice = NETWORK_SIDEBAR_AWG_NOTICE_CLEANUP_UNCONFIRMED;
    refresh_entries = TRUE;
    break;
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_ALREADY_IN_STATE:
    refresh_entries = TRUE;
    break;
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_NONE:
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_BUSY:
    break;
  }
  if (notice != NETWORK_SIDEBAR_AWG_NOTICE_NONE)
    emit_notice(controller, notice, operation, 0, NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE, name);
  if (refresh_profiles)
    network_sidebar_awg_controller_request_inventory(controller, TRUE);
  else if (refresh_runtime)
    network_sidebar_awg_runtime_source_request_refresh(
      controller->runtime_source, NETWORK_SIDEBAR_AWG_REFRESH_VISIBLE);
  if (refresh_entries) {
    rebuild_entries(controller);
    emit_changed(controller, 1);
  }
  return FALSE;
}

static AwgServiceAction *
service_action_new(NetworkSidebarAwgController *controller,
                   guint64 activity_id,
                   const char *name,
                   NetworkSidebarAwgBackendOperation operation,
                   GBytes *config)
{
  AwgServiceAction *action = g_new0(AwgServiceAction, 1);

  action->controller = network_sidebar_awg_controller_ref(controller);
  action->activity_id = activity_id;
  action->name = g_strdup(name);
  action->operation = operation;
  action->config = config != NULL ? g_bytes_ref(config) : NULL;
  return action;
}

static void
service_action_free(AwgServiceAction *action)
{
  NetworkSidebarAwgController *controller;

  if (action == NULL)
    return;
  controller = action->controller;
  g_clear_pointer(&action->config, g_bytes_unref);
  g_free(action->name);
  g_free(action);
  network_sidebar_awg_controller_unref(controller);
}

static gboolean
service_action_matches(AwgServiceAction *action)
{
  return activity_matches(action->controller, action->activity_id);
}

static void
complete_mutation_action_with_purpose(AwgServiceAction *action,
                                      guint inventory_delay_ms,
                                      NetworkSidebarAwgRefreshPurpose purpose)
{
  if (service_action_matches(action)) {
    if (action->operation == NETWORK_SIDEBAR_AWG_BACKEND_IMPORT ||
        action->operation == NETWORK_SIDEBAR_AWG_BACKEND_REPLACE ||
        action->operation == NETWORK_SIDEBAR_AWG_BACKEND_DELETE) {
      network_sidebar_awg_inventory_queue_refresh(action->controller->inventory,
                                                   inventory_delay_ms, purpose);
    }
    /* An in-flight scan may have observed the helper's activating/cleanup
     * journal. Keep mutation progress until a post-operation scan replaces
     * that observation, rather than briefly presenting it as recovery. */
    action->controller->runtime_verification_pending =
      action->controller->activity == NETWORK_SIDEBAR_AWG_ACTIVITY_UP ||
      action->controller->activity == NETWORK_SIDEBAR_AWG_ACTIVITY_DOWN ||
      action->controller->activity == NETWORK_SIDEBAR_AWG_ACTIVITY_DELETE ||
      action->controller->activity == NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT ||
      action->controller->activity == NETWORK_SIDEBAR_AWG_ACTIVITY_REPLACE;
    network_sidebar_awg_runtime_source_request_refresh(
      action->controller->runtime_source, purpose);
    if (!action->controller->runtime_verification_pending ||
        !action->controller->authorized ||
        !network_sidebar_awg_runtime_source_is_checking(action->controller->runtime_source))
      finish_activity(action->controller, action->activity_id, 400);
  }
  service_action_free(action);
}

static void
complete_mutation_action(AwgServiceAction *action, guint inventory_delay_ms)
{
  complete_mutation_action_with_purpose(action, inventory_delay_ms,
                                        NETWORK_SIDEBAR_AWG_REFRESH_VISIBLE);
}

static void
complete_mutation_without_inventory(AwgServiceAction *action)
{
  if (service_action_matches(action)) {
    finish_activity(action->controller, action->activity_id, 1);
  }
  service_action_free(action);
}

static gboolean
request_confirmation(AwgServiceAction *action,
                     NetworkSidebarAwgUiEventType event_type,
                     NetworkSidebarAwgActivity activity)
{
  g_autoptr(NetworkSidebarAwgController) controller_ref =
    network_sidebar_awg_controller_ref(action->controller);
  NetworkSidebarAwgController *controller = controller_ref;
  g_autofree char *name = g_strdup(action->name);
  guint64 activity_id = action->activity_id;
  gboolean available;
  NetworkSidebarAwgNotice unavailable_notice =
    event_type == NETWORK_SIDEBAR_AWG_UI_CONFIRM_REPLACE ?
      NETWORK_SIDEBAR_AWG_NOTICE_REPLACEMENT_CONFIRMATION_UNAVAILABLE :
      NETWORK_SIDEBAR_AWG_NOTICE_REMOVAL_CONFIRMATION_UNAVAILABLE;

  if (!service_action_matches(action)) {
    service_action_free(action);
    return FALSE;
  }
  available = controller->authorized &&
    emit_ui_request(controller, event_type, 0, name, TRUE);
  if (!service_action_matches(action)) {
    service_action_free(action);
    return FALSE;
  }
  if (!controller->authorized) {
    emit_notice(controller,
                NETWORK_SIDEBAR_AWG_NOTICE_AUTHORIZATION_UNAVAILABLE,
                action->operation,
                0,
                NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
                NULL);
    complete_mutation_without_inventory(action);
    return FALSE;
  }
  if (!available)
    goto unavailable;

  /* Changed/UI callbacks may consume the action, including during transition. */
  controller->pending_confirmation = action;
  if (controller->activity != activity &&
      !transition_activity(controller,
                           activity_id,
                           activity,
                           name)) {
    if (controller->pending_confirmation != NULL &&
        controller->pending_confirmation->activity_id == activity_id)
      g_clear_pointer(&controller->pending_confirmation, service_action_free);
    return FALSE;
  }
  if (!network_sidebar_awg_controller_confirmation_is_pending(
        controller, event_type, activity_id))
    return FALSE;

  available = emit_ui_request(controller, event_type, activity_id, name, FALSE);
  if (!confirmation_matches(controller, event_type, activity_id))
    return available;
  if (available)
    return TRUE;
  action = g_steal_pointer(&controller->pending_confirmation);

unavailable:
  emit_notice(controller,
              unavailable_notice,
              action->operation,
              0,
              NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
              name);
  if (event_type == NETWORK_SIDEBAR_AWG_UI_CONFIRM_REPLACE)
    complete_mutation_action(action, 400);
  else {
    finish_activity(controller, activity_id, 1);
    service_action_free(action);
  }
  return FALSE;
}

static void
backend_service_result(const NetworkSidebarAwgBackendResult *result,
                       gpointer user_data)
{
  AwgServiceAction *action = user_data;
  NetworkSidebarAwgController *controller = action->controller;

  if (!service_action_matches(action)) {
    service_action_free(action);
    return;
  }

  network_sidebar_awg_runtime_source_set_mutation_active(
    controller->runtime_source, FALSE);
  if (result->kind == NETWORK_SIDEBAR_AWG_BACKEND_RESULT_CANCELLED) {
    complete_mutation_without_inventory(action);
    return;
  }
  if (result->kind == NETWORK_SIDEBAR_AWG_BACKEND_RESULT_TIMED_OUT) {
    emit_notice(controller,
                NETWORK_SIDEBAR_AWG_NOTICE_OPERATION_TIMED_OUT,
                action->operation,
                0,
                NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
                action->name);
    complete_mutation_action(action, AWG_INVENTORY_RECHECK_DELAY_MSEC);
    return;
  }
  if (result->kind == NETWORK_SIDEBAR_AWG_BACKEND_RESULT_BUS_UNAVAILABLE) {
    network_sidebar_awg_inventory_set_service_reachable(controller->inventory,
                                                        FALSE);
    network_sidebar_awg_inventory_schedule_retry(
      controller->inventory, NETWORK_SIDEBAR_AWG_REFRESH_VISIBLE);
    emit_notice(controller,
                NETWORK_SIDEBAR_AWG_NOTICE_SERVICE_CONNECT_FAILED,
                action->operation,
                0,
                NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
                action->name);
    complete_mutation_action(action, 400);
    return;
  }
  if (result->kind != NETWORK_SIDEBAR_AWG_BACKEND_RESULT_REPLY) {
    network_sidebar_awg_inventory_set_service_reachable(controller->inventory,
                                                        result->service_replied);
    if (result->kind == NETWORK_SIDEBAR_AWG_BACKEND_RESULT_NOT_AUTHORIZED) {
      network_sidebar_awg_backend_invalidate_authorization(controller->backend);
      emit_notice(controller,
                  NETWORK_SIDEBAR_AWG_NOTICE_AUTHORIZATION_UNAVAILABLE,
                  action->operation,
                  0,
                  NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
                  action->name);
      complete_mutation_without_inventory(action);
    } else if (result->kind == NETWORK_SIDEBAR_AWG_BACKEND_RESULT_BUSY) {
      emit_notice(controller,
                  NETWORK_SIDEBAR_AWG_NOTICE_SERVICE_BUSY,
                  action->operation,
                  0,
                  NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
                  action->name);
      complete_mutation_action(action, AWG_INVENTORY_RECHECK_DELAY_MSEC);
    } else {
      network_sidebar_awg_inventory_schedule_retry(
        controller->inventory, NETWORK_SIDEBAR_AWG_REFRESH_VISIBLE);
      emit_notice(controller,
                  NETWORK_SIDEBAR_AWG_NOTICE_SERVICE_FAILED,
                  action->operation,
                  0,
                  NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
                  action->name);
      complete_mutation_action(action, 400);
    }
    return;
  }

  network_sidebar_awg_inventory_set_service_reachable(controller->inventory,
                                                      TRUE);

  if (result->status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    if (action->operation == NETWORK_SIDEBAR_AWG_BACKEND_DELETE) {
      /* Delete already verified that the saved file, journal, and interface
       * are gone. Retire every row source before runtime callbacks rebuild it;
       * inventory and runtime verification can continue in the background. */
      network_sidebar_awg_model_record_delete(controller->model, action->name);
      g_clear_pointer(&controller->activity_name, g_free);
      network_sidebar_awg_runtime_source_record_delete(
        controller->runtime_source, action->name);
      sync_runtime_profile_names(controller, NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE);
    }
    if (service_action_matches(action) && controller->authorized &&
        (action->operation == NETWORK_SIDEBAR_AWG_BACKEND_IMPORT ||
         action->operation == NETWORK_SIDEBAR_AWG_BACKEND_REPLACE)) {
      /* The helper has validated and saved this profile. Promote the activity
       * row before runtime verification ends, so delayed inventory cannot
       * remove it or leave the new name out of the post-operation scan. */
      network_sidebar_awg_model_record_import(controller->model, action->name);
      sync_runtime_profile_names(controller, NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE);
      rebuild_entries(controller);
    }
    emit_notice(controller,
                NETWORK_SIDEBAR_AWG_NOTICE_OPERATION_SUCCEEDED,
                action->operation,
                0,
                NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
                action->name);
    if (action->operation == NETWORK_SIDEBAR_AWG_BACKEND_DELETE) {
      network_sidebar_awg_inventory_queue_refresh(
        controller->inventory, 400, NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE);
      complete_mutation_without_inventory(action);
      return;
    }
    complete_mutation_action_with_purpose(
      action, 400,
      action->operation == NETWORK_SIDEBAR_AWG_BACKEND_IMPORT ||
      action->operation == NETWORK_SIDEBAR_AWG_BACKEND_REPLACE ?
        NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE : NETWORK_SIDEBAR_AWG_REFRESH_VISIBLE);
    return;
  }
  if (action->operation == NETWORK_SIDEBAR_AWG_BACKEND_IMPORT &&
      result->status ==
        NETWORK_SIDEBAR_AMNEZIAWG_HELPER_REPLACE_CONFIRMATION_REQUIRED) {
    request_confirmation(action,
                         NETWORK_SIDEBAR_AWG_UI_CONFIRM_REPLACE,
                         NETWORK_SIDEBAR_AWG_ACTIVITY_REPLACE_CONFIRM);
    return;
  }

  emit_notice(controller,
              NETWORK_SIDEBAR_AWG_NOTICE_HELPER_FAILED,
              action->operation,
              result->status,
              NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
              action->name);
  if (result->status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_NOT_FOUND ||
      result->status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_CONFIG ||
      result->status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_AWG_QUICK_UNAVAILABLE ||
      result->status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED)
    network_sidebar_awg_inventory_queue_refresh(controller->inventory,
                                                 AWG_INVENTORY_RECHECK_DELAY_MSEC,
                                                 NETWORK_SIDEBAR_AWG_REFRESH_VISIBLE);
  complete_mutation_action(
    action,
    result->status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_TIMED_OUT ?
      AWG_INVENTORY_RECHECK_DELAY_MSEC : 400);
}

static void
dispatch_service_action(AwgServiceAction *action)
{
  NetworkSidebarAwgController *controller = action->controller;
  gboolean needs_config =
    action->operation == NETWORK_SIDEBAR_AWG_BACKEND_IMPORT ||
    action->operation == NETWORK_SIDEBAR_AWG_BACKEND_REPLACE;

  if (!service_action_matches(action)) {
    service_action_free(action);
    return;
  }
  if (needs_config && action->config == NULL) {
    emit_notice(controller,
                NETWORK_SIDEBAR_AWG_NOTICE_CONFIGURATION_UNAVAILABLE,
                action->operation,
                0,
                NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
                action->name);
    complete_mutation_without_inventory(action);
    return;
  }
  if (emit_ui_request(controller,
                      NETWORK_SIDEBAR_AWG_UI_EXTERNAL_INTERACTION_ACTIVE,
                      action->activity_id,
                      action->name,
                      FALSE)) {
    emit_notice(controller,
                NETWORK_SIDEBAR_AWG_NOTICE_EXTERNAL_OPERATION_IN_PROGRESS,
                action->operation,
                0,
                NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
                action->name);
    complete_mutation_without_inventory(action);
    return;
  }

  if (!service_action_matches(action)) {
    service_action_free(action);
    return;
  }
  /* Retire the client wait before sending Execute. Only the authorized service
   * command may stop the helper; cancellation alone cannot release its lock. */
  network_sidebar_awg_inventory_preempt(controller->inventory);
  if (!service_action_matches(action)) {
    service_action_free(action);
    return;
  }
  if (!controller->authorized) {
    emit_notice(controller,
                NETWORK_SIDEBAR_AWG_NOTICE_AUTHORIZATION_UNAVAILABLE,
                action->operation,
                0,
                NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
                action->name);
    complete_mutation_without_inventory(action);
    return;
  }
  /* Down retains the admitted cleanup intent if the runtime observation expires.
   * Other commands must still match the current profile/runtime facts. */
  if (action->operation != NETWORK_SIDEBAR_AWG_BACKEND_DOWN &&
      !operation_is_allowed(controller, action->operation, action->name, TRUE, NULL)) {
    complete_mutation_without_inventory(action);
    return;
  }
  NetworkSidebarAwgDependency missing = operation_missing_dependencies(
    controller, action->operation, action->name, action->runtime_cleanup_only);
  if (missing != NETWORK_SIDEBAR_AWG_DEPENDENCY_NONE) {
    emit_notice(controller,
                NETWORK_SIDEBAR_AWG_NOTICE_REQUIRED_COMPONENTS_UNAVAILABLE,
                action->operation,
                0,
                NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
                action->name);
    complete_mutation_without_inventory(action);
    return;
  }
  network_sidebar_awg_runtime_source_set_mutation_active(
    controller->runtime_source,
    action->operation == NETWORK_SIDEBAR_AWG_BACKEND_IMPORT ||
    action->operation == NETWORK_SIDEBAR_AWG_BACKEND_REPLACE ||
    action->operation == NETWORK_SIDEBAR_AWG_BACKEND_DELETE);
  network_sidebar_awg_backend_execute(controller->backend,
                                      action->operation,
                                      action->name,
                                      action->config,
                                      NULL,
                                      backend_service_result,
                                      action);
}

NetworkSidebarAwgController *
network_sidebar_awg_controller_new(NetworkSidebarAwgChangedCallback changed,
                                   gpointer user_data)
{
  static const NetworkSidebarAwgBackendCallbacks backend_callbacks = {
    .authorization_changed = backend_authorization_changed,
  };
  static const NetworkSidebarAwgInventoryCallbacks inventory_callbacks = {
    .changed = inventory_changed,
    .can_dispatch = inventory_can_dispatch,
  };
  NetworkSidebarAwgController *controller = g_new0(
    NetworkSidebarAwgController, 1);

  controller->ref_count = 1;
  controller->changed = changed;
  controller->changed_data = user_data;
  controller->model = network_sidebar_awg_model_new();
  controller->backend = network_sidebar_awg_backend_new(&backend_callbacks,
                                                         controller);
  controller->inventory = network_sidebar_awg_inventory_new(
    controller->backend, controller->model, &inventory_callbacks, controller);
  controller->runtime_source = network_sidebar_awg_runtime_source_new(
    runtime_source_changed, controller);
  controller->activity = NETWORK_SIDEBAR_AWG_ACTIVITY_IDLE;
  controller->generation = 1;
  return controller;
}

NetworkSidebarAwgController *
network_sidebar_awg_controller_ref(NetworkSidebarAwgController *controller)
{
  if (controller == NULL)
    return NULL;
  g_atomic_int_inc(&controller->ref_count);
  return controller;
}

void
network_sidebar_awg_controller_set_ui_event_callback(
  NetworkSidebarAwgController *controller,
  NetworkSidebarAwgUiEventCallback callback,
  gpointer user_data)
{
  if (controller == NULL)
    return;
  controller->ui_event = callback;
  controller->ui_event_data = user_data;
}

static void
dismiss_presented_interactions(NetworkSidebarAwgController *controller)
{
  NetworkSidebarAwgUiEventCallback callback = controller->ui_event;
  gpointer user_data = controller->ui_event_data;
  NetworkSidebarAwgUiEvent event = {
    .type = NETWORK_SIDEBAR_AWG_UI_DISMISS_ALL,
  };

  if (callback != NULL)
    callback(&event, user_data);
}

static void
controller_stop_internal(NetworkSidebarAwgController *controller,
                         gboolean notify_ui)
{
  if (controller->stopping)
    return;
  controller->stopping = TRUE;
  controller->started = FALSE;
  if (controller->activity_id != G_MAXUINT64)
    controller->activity_id++;
  controller->activity = NETWORK_SIDEBAR_AWG_ACTIVITY_IDLE;
  controller->runtime_verification_pending = FALSE;
  g_clear_pointer(&controller->activity_name, g_free);
  controller->authorized = FALSE;
  g_clear_pointer(&controller->pending_confirmation, service_action_free);

  network_sidebar_awg_inventory_stop(controller->inventory);
  network_sidebar_awg_backend_stop(controller->backend);
  network_sidebar_awg_runtime_source_stop(controller->runtime_source);
  if (notify_ui)
    dismiss_presented_interactions(controller);

  controller->changed = NULL;
  controller->changed_data = NULL;
  controller->ui_event = NULL;
  controller->ui_event_data = NULL;
  network_sidebar_awg_model_rebuild_entries(
    controller->model,
    FALSE,
    NULL,
    FALSE,
    controller->activity,
    NULL);
  if (controller->generation != G_MAXUINT64)
    controller->generation++;
}

void
network_sidebar_awg_controller_unref(NetworkSidebarAwgController *controller)
{
  if (controller == NULL)
    return;
  if (!g_atomic_int_dec_and_test(&controller->ref_count))
    return;

  controller_stop_internal(controller, FALSE);
  network_sidebar_awg_inventory_unref(controller->inventory);
  network_sidebar_awg_backend_unref(controller->backend);
  network_sidebar_awg_runtime_source_unref(controller->runtime_source);
  network_sidebar_awg_model_free(controller->model);
  g_free(controller->activity_name);
  g_free(controller);
}

void
network_sidebar_awg_controller_start(NetworkSidebarAwgController *controller)
{
  if (controller == NULL || controller->started || controller->stopping)
    return;
  controller->started = TRUE;
  network_sidebar_awg_inventory_start(controller->inventory);
  network_sidebar_awg_runtime_source_start(controller->runtime_source);
  network_sidebar_awg_backend_start(controller->backend);
}

void
network_sidebar_awg_controller_stop(NetworkSidebarAwgController *controller)
{
  if (controller == NULL || controller->stopping)
    return;
  network_sidebar_awg_controller_ref(controller);
  controller_stop_internal(controller, TRUE);
  network_sidebar_awg_controller_unref(controller);
}

void
network_sidebar_awg_controller_set_inventory_enabled(
  NetworkSidebarAwgController *controller,
  gboolean enabled)
{
  if (controller == NULL || controller->stopping)
    return;
  network_sidebar_awg_inventory_set_enabled(controller->inventory, enabled);
}

NetworkSidebarAwgSnapshot *
network_sidebar_awg_controller_dup_snapshot(
  NetworkSidebarAwgController *controller)
{
  if (controller == NULL)
    return NULL;
  return network_sidebar_awg_model_dup_snapshot(
    controller->model,
    controller->generation,
    controller->authorized,
    network_sidebar_awg_inventory_capabilities_known(controller->inventory),
    network_sidebar_awg_inventory_missing_dependencies(controller->inventory),
    controller->activity,
    controller->activity_name,
    network_sidebar_awg_inventory_is_pending(controller->inventory),
    network_sidebar_awg_runtime_source_is_checking(controller->runtime_source),
    network_sidebar_awg_inventory_is_loading(controller->inventory),
    network_sidebar_awg_runtime_source_is_loading(controller->runtime_source));
}

void
network_sidebar_awg_controller_free_snapshot(NetworkSidebarAwgSnapshot *snapshot)
{
  network_sidebar_awg_snapshot_free(snapshot);
}

void
network_sidebar_awg_controller_request_inventory(
  NetworkSidebarAwgController *controller,
  gboolean force)
{
  g_autoptr(NetworkSidebarAwgController) controller_ref =
    network_sidebar_awg_controller_ref(controller);

  controller = controller_ref;
  if (!controller_is_running(controller) || !controller->authorized)
    return;
  network_sidebar_awg_runtime_source_poll(
    controller->runtime_source, NETWORK_SIDEBAR_AWG_REFRESH_VISIBLE);
  network_sidebar_awg_inventory_request_refresh(
    controller->inventory, force, NETWORK_SIDEBAR_AWG_REFRESH_VISIBLE);
}

void
network_sidebar_awg_controller_import(NetworkSidebarAwgController *controller)
{
  g_autoptr(NetworkSidebarAwgController) controller_ref =
    network_sidebar_awg_controller_ref(controller);
  guint64 activity_id;

  controller = controller_ref;
  if (!controller_is_running(controller))
    return;
  if (!operation_is_allowed(controller, NETWORK_SIDEBAR_AWG_BACKEND_IMPORT,
                              NULL, FALSE, NULL))
    return;
  if (!emit_ui_request(controller,
                       NETWORK_SIDEBAR_AWG_UI_SELECT_IMPORT,
                       0,
                       NULL,
                       TRUE)) {
    emit_notice(controller,
                NETWORK_SIDEBAR_AWG_NOTICE_IMPORT_CHOOSER_UNAVAILABLE,
                NETWORK_SIDEBAR_AWG_BACKEND_IMPORT,
                0,
                NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
                NULL);
    return;
  }
  if (!begin_activity(controller,
                      NETWORK_SIDEBAR_AWG_BACKEND_IMPORT,
                      NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_SELECT,
                      NULL))
    return;
  activity_id = controller->activity_id;
  if (!emit_ui_request(controller,
                       NETWORK_SIDEBAR_AWG_UI_SELECT_IMPORT,
                       activity_id,
                       NULL,
                       FALSE) &&
      activity_matches(controller, activity_id)) {
    emit_notice(controller,
                NETWORK_SIDEBAR_AWG_NOTICE_IMPORT_CHOOSER_UNAVAILABLE,
                NETWORK_SIDEBAR_AWG_BACKEND_IMPORT,
                0,
                NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
                NULL);
    finish_activity(controller, activity_id, 1);
  }
}

gboolean
network_sidebar_awg_controller_import_file_selected(
  NetworkSidebarAwgController *controller,
  guint64 activity_id,
  const char *basename)
{
  g_autofree char *name = NULL;

  if (!activity_matches(controller, activity_id) ||
      controller->activity != NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_SELECT)
    return FALSE;
  name = network_sidebar_amneziawg_interface_from_filename(basename);
  if (name == NULL || !network_sidebar_amneziawg_name_is_valid(name)) {
    emit_notice(controller,
                NETWORK_SIDEBAR_AWG_NOTICE_INVALID_IMPORT_FILENAME,
                NETWORK_SIDEBAR_AWG_BACKEND_IMPORT,
                0,
                NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
                NULL);
    finish_activity(controller, activity_id, 1);
    return FALSE;
  }
  return transition_activity(controller,
                             activity_id,
                             NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_LOAD,
                             name);
}

void
network_sidebar_awg_controller_import_selection_cancelled(
  NetworkSidebarAwgController *controller,
  guint64 activity_id)
{
  if (activity_matches(controller, activity_id) &&
      controller->activity == NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_SELECT)
    finish_activity(controller, activity_id, 1);
}

void
network_sidebar_awg_controller_import_loaded(
  NetworkSidebarAwgController *controller,
  guint64 activity_id,
  GBytes *config)
{
  AwgServiceAction *action;

  if (!activity_matches(controller, activity_id) ||
      controller->activity != NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_LOAD)
    return;
  action = service_action_new(controller,
                              activity_id,
                              controller->activity_name,
                              NETWORK_SIDEBAR_AWG_BACKEND_IMPORT,
                              config);
  if (!transition_activity(controller,
                           activity_id,
                           NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT,
                           action->name)) {
    service_action_free(action);
    return;
  }
  dispatch_service_action(action);
}

void
network_sidebar_awg_controller_import_failed(
  NetworkSidebarAwgController *controller,
  guint64 activity_id,
  NetworkSidebarAwgImportError error)
{
  if (!activity_matches(controller, activity_id) ||
      controller->activity != NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_LOAD)
    return;
  switch (error) {
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_INSPECT_FAILED:
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NOT_LOCAL:
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NOT_REGULAR:
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_TOO_LARGE:
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_OPEN_FAILED:
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_READ_FAILED:
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_EMPTY:
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_TIMED_OUT:
    break;
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE:
  default:
    return;
  }
  emit_notice(controller,
              NETWORK_SIDEBAR_AWG_NOTICE_IMPORT_LOAD_FAILED,
              NETWORK_SIDEBAR_AWG_BACKEND_IMPORT,
              0,
              error,
              controller->activity_name);
  finish_activity(controller, activity_id, 1);
}

void
network_sidebar_awg_controller_cancel_import(
  NetworkSidebarAwgController *controller)
{
  g_autoptr(NetworkSidebarAwgController) controller_ref =
    network_sidebar_awg_controller_ref(controller);
  guint64 activity_id;

  controller = controller_ref;
  if (!controller_is_running(controller) ||
      (controller->activity != NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_SELECT &&
       controller->activity != NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_LOAD))
    return;
  activity_id = controller->activity_id;
  emit_notice(controller,
              NETWORK_SIDEBAR_AWG_NOTICE_IMPORT_CANCELLED,
              NETWORK_SIDEBAR_AWG_BACKEND_IMPORT,
              0,
              NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
              controller->activity_name);
  finish_activity(controller, activity_id, 1);
}

void
network_sidebar_awg_controller_set_active(
  NetworkSidebarAwgController *controller,
  const char *name,
  gboolean active)
{
  g_autoptr(NetworkSidebarAwgController) controller_ref =
    network_sidebar_awg_controller_ref(controller);
  g_autofree char *owned_name = g_strdup(name);
  NetworkSidebarAwgAdmissionTarget target;
  NetworkSidebarAwgBackendOperation operation;
  AwgServiceAction *action;

  controller = controller_ref;
  if (!controller_is_running(controller) ||
      !network_sidebar_amneziawg_name_is_valid(owned_name))
    return;
  operation = active ? NETWORK_SIDEBAR_AWG_BACKEND_UP :
                       NETWORK_SIDEBAR_AWG_BACKEND_DOWN;
  if (!operation_is_allowed(controller, operation, owned_name, FALSE, &target))
    return;
  if (!begin_activity(controller,
                      operation,
                      active ? NETWORK_SIDEBAR_AWG_ACTIVITY_UP :
                               NETWORK_SIDEBAR_AWG_ACTIVITY_DOWN,
                      owned_name))
    return;
  /* Up includes all previous disconnects inside the privileged helper. Keep
   * one activity until its final reply; never chain client-side Down/Up calls. */
  action = service_action_new(controller,
                              controller->activity_id,
                              owned_name,
                              operation,
                              NULL);
  action->runtime_cleanup_only = !active &&
    target.runtime_state == NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_FIREWALL_PENDING;
  emit_notice(controller,
              NETWORK_SIDEBAR_AWG_NOTICE_OPERATION_STARTED,
              operation,
              0,
              NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
              owned_name);
  dispatch_service_action(action);
}

void
network_sidebar_awg_controller_confirm_delete(
  NetworkSidebarAwgController *controller,
  const char *name)
{
  g_autoptr(NetworkSidebarAwgController) controller_ref =
    network_sidebar_awg_controller_ref(controller);
  g_autofree char *owned_name = g_strdup(name);
  AwgServiceAction *action;

  controller = controller_ref;
  if (!controller_is_running(controller) ||
      !network_sidebar_amneziawg_name_is_valid(owned_name))
    return;
  if (!operation_is_allowed(controller, NETWORK_SIDEBAR_AWG_BACKEND_DELETE,
                              owned_name, FALSE, NULL))
    return;
  if (!emit_ui_request(controller,
                       NETWORK_SIDEBAR_AWG_UI_CONFIRM_DELETE,
                       0,
                       owned_name,
                       TRUE)) {
    emit_notice(controller,
                NETWORK_SIDEBAR_AWG_NOTICE_REMOVAL_CONFIRMATION_UNAVAILABLE,
                NETWORK_SIDEBAR_AWG_BACKEND_DELETE,
                0,
                NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
                owned_name);
    return;
  }
  if (!begin_activity(controller,
                      NETWORK_SIDEBAR_AWG_BACKEND_DELETE,
                      NETWORK_SIDEBAR_AWG_ACTIVITY_DELETE_CONFIRM,
                      owned_name))
    return;
  action = service_action_new(controller,
                              controller->activity_id,
                              owned_name,
                              NETWORK_SIDEBAR_AWG_BACKEND_DELETE,
                              NULL);
  request_confirmation(action,
                       NETWORK_SIDEBAR_AWG_UI_CONFIRM_DELETE,
                       NETWORK_SIDEBAR_AWG_ACTIVITY_DELETE_CONFIRM);
}

gboolean
network_sidebar_awg_controller_confirmation_is_pending(
  NetworkSidebarAwgController *controller,
  NetworkSidebarAwgUiEventType confirmation,
  guint64 activity_id)
{
  return confirmation_matches(controller, confirmation, activity_id) &&
         controller->authorized;
}

void
network_sidebar_awg_controller_confirmation_response(
  NetworkSidebarAwgController *controller,
  NetworkSidebarAwgUiEventType confirmation,
  guint64 activity_id,
  gboolean confirmed)
{
  AwgServiceAction *action;

  /* Cancellation must still consume a matching token without authorization. */
  if (!confirmation_matches(controller, confirmation, activity_id))
    return;
  action = g_steal_pointer(&controller->pending_confirmation);

  if (confirmation == NETWORK_SIDEBAR_AWG_UI_CONFIRM_REPLACE) {
    if (!confirmed) {
      complete_mutation_action(action, 400);
      return;
    }
    if (!operation_is_allowed(controller, NETWORK_SIDEBAR_AWG_BACKEND_REPLACE,
                                action->name, TRUE, NULL)) {
      complete_mutation_action(action, 400);
      return;
    }
    action->operation = NETWORK_SIDEBAR_AWG_BACKEND_REPLACE;
    if (!transition_activity(controller,
                             activity_id,
                             NETWORK_SIDEBAR_AWG_ACTIVITY_REPLACE,
                             action->name)) {
      service_action_free(action);
      return;
    }
    dispatch_service_action(action);
    return;
  }

  if (!confirmed) {
    finish_activity(controller, activity_id, 1);
    service_action_free(action);
    return;
  }
  if (!operation_is_allowed(controller, NETWORK_SIDEBAR_AWG_BACKEND_DELETE,
                              action->name, TRUE, NULL)) {
    complete_mutation_without_inventory(action);
    return;
  }
  if (!transition_activity(controller,
                           activity_id,
                           NETWORK_SIDEBAR_AWG_ACTIVITY_DELETE,
                           action->name)) {
    service_action_free(action);
    return;
  }
  emit_notice(controller,
              NETWORK_SIDEBAR_AWG_NOTICE_OPERATION_STARTED,
              NETWORK_SIDEBAR_AWG_BACKEND_DELETE,
              0,
              NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
              action->name);
  dispatch_service_action(action);
}
