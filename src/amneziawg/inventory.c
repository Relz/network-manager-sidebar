#include "amneziawg/inventory.h"

#define AWG_INVENTORY_REFRESH_INTERVAL_SECONDS 60
#define AWG_INVENTORY_RECHECK_DELAY_MSEC 2000u
#define AWG_INVENTORY_UNKNOWN_DEPENDENCIES \
  (NETWORK_SIDEBAR_AWG_DEPENDENCY_HELPER | NETWORK_SIDEBAR_AWG_DEPENDENCY_AWG_QUICK)

typedef struct {
  NetworkSidebarAwgInventory *inventory;
  guint64 id;
} InventoryRequest;

struct _NetworkSidebarAwgInventory {
  gint ref_count;
  NetworkSidebarAwgBackend *backend;
  NetworkSidebarAwgModel *model;
  NetworkSidebarAwgInventoryCallbacks callbacks;
  gpointer user_data;

  NetworkSidebarAwgDependency missing_dependencies;
  gboolean capabilities_known;
  guint refresh_source;
  gint64 source_deadline_us;
  gint64 last_attempt_us;
  guint retry_attempt;
  gboolean dirty;
  guint dirty_delay_ms;
  NetworkSidebarAwgRefreshPurpose queued_purpose;
  guint64 request_id;
  gboolean pending;
  NetworkSidebarAwgRefreshPurpose pending_purpose;
  GCancellable *cancellable;

  gboolean authorized;
  gboolean enabled;
  gboolean suspended;
  gboolean started;
  gboolean stopping;
};

static void schedule_queued_refresh(NetworkSidebarAwgInventory *inventory);

static gboolean
inventory_is_running(const NetworkSidebarAwgInventory *inventory)
{
  return inventory != NULL && inventory->started && !inventory->stopping;
}

NetworkSidebarAwgInventory *
network_sidebar_awg_inventory_ref(NetworkSidebarAwgInventory *inventory)
{
  if (inventory != NULL)
    g_atomic_int_inc(&inventory->ref_count);
  return inventory;
}

static void
notify_changed(NetworkSidebarAwgInventory *inventory,
               NetworkSidebarAwgInventoryChange change,
               NetworkSidebarAwgRefreshPurpose purpose)
{
  g_autoptr(NetworkSidebarAwgInventory) guard = NULL;

  if (!inventory_is_running(inventory) || inventory->callbacks.changed == NULL)
    return;
  guard = network_sidebar_awg_inventory_ref(inventory);
  inventory->callbacks.changed(change, purpose, inventory->user_data);
}

static void
cancel_refresh_source(NetworkSidebarAwgInventory *inventory)
{
  g_clear_handle_id(&inventory->refresh_source, g_source_remove);
  inventory->source_deadline_us = 0;
}

static void
cancel_request(NetworkSidebarAwgInventory *inventory)
{
  g_autoptr(GCancellable) cancellable = g_steal_pointer(&inventory->cancellable);

  /* Cancellation may complete synchronously. Retired callbacks must not touch
   * either this snapshot or a newer request, including after reauthorization. */
  inventory->pending = FALSE;
  inventory->pending_purpose = NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
  if (cancellable != NULL)
    g_cancellable_cancel(cancellable);
}

static void
reset_inventory(NetworkSidebarAwgInventory *inventory)
{
  cancel_refresh_source(inventory);
  cancel_request(inventory);
  inventory->dirty = FALSE;
  inventory->dirty_delay_ms = 0;
  inventory->queued_purpose = NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
  inventory->retry_attempt = 0;
  inventory->last_attempt_us = 0;
  inventory->capabilities_known = FALSE;
  inventory->missing_dependencies = AWG_INVENTORY_UNKNOWN_DEPENDENCIES;
  network_sidebar_awg_model_reset_profiles(inventory->model);
}

void
network_sidebar_awg_inventory_stop(NetworkSidebarAwgInventory *inventory)
{
  if (inventory == NULL || inventory->stopping)
    return;
  inventory->stopping = TRUE;
  inventory->started = FALSE;
  inventory->authorized = FALSE;
  inventory->enabled = FALSE;
  inventory->callbacks = (NetworkSidebarAwgInventoryCallbacks) { 0 };
  inventory->user_data = NULL;
  reset_inventory(inventory);
  inventory->model = NULL;
}

void
network_sidebar_awg_inventory_unref(NetworkSidebarAwgInventory *inventory)
{
  if (inventory == NULL || !g_atomic_int_dec_and_test(&inventory->ref_count))
    return;
  network_sidebar_awg_inventory_stop(inventory);
  network_sidebar_awg_backend_unref(inventory->backend);
  g_free(inventory);
}

NetworkSidebarAwgInventory *
network_sidebar_awg_inventory_new(
  NetworkSidebarAwgBackend *backend,
  NetworkSidebarAwgModel *model,
  const NetworkSidebarAwgInventoryCallbacks *callbacks,
  gpointer user_data)
{
  NetworkSidebarAwgInventory *inventory = g_new0(NetworkSidebarAwgInventory, 1);

  inventory->ref_count = 1;
  inventory->backend = network_sidebar_awg_backend_ref(backend);
  inventory->model = model;
  if (callbacks != NULL)
    inventory->callbacks = *callbacks;
  inventory->user_data = user_data;
  inventory->missing_dependencies = AWG_INVENTORY_UNKNOWN_DEPENDENCIES;
  return inventory;
}

void
network_sidebar_awg_inventory_start(NetworkSidebarAwgInventory *inventory)
{
  if (inventory == NULL || inventory->started || inventory->stopping)
    return;
  inventory->started = TRUE;
  schedule_queued_refresh(inventory);
}

void
network_sidebar_awg_inventory_set_authorized(NetworkSidebarAwgInventory *inventory,
                                             gboolean authorized)
{
  g_autoptr(NetworkSidebarAwgInventory) guard =
    network_sidebar_awg_inventory_ref(inventory);
  gboolean changed;

  if (inventory == NULL || inventory->stopping)
    return;
  inventory->authorized = authorized;
  if (authorized) {
    schedule_queued_refresh(inventory);
    return;
  }
  changed = inventory->pending || inventory->capabilities_known ||
    inventory->missing_dependencies != AWG_INVENTORY_UNKNOWN_DEPENDENCIES ||
    network_sidebar_awg_model_has_profile_data(inventory->model);
  reset_inventory(inventory);
  if (changed)
    notify_changed(inventory, NETWORK_SIDEBAR_AWG_INVENTORY_PROFILES_CHANGED,
                   NETWORK_SIDEBAR_AWG_REFRESH_VISIBLE);
}

void
network_sidebar_awg_inventory_set_enabled(NetworkSidebarAwgInventory *inventory,
                                          gboolean enabled)
{
  if (inventory == NULL || inventory->stopping || inventory->enabled == enabled)
    return;
  inventory->enabled = enabled;
  if (!enabled) {
    cancel_refresh_source(inventory);
    return;
  }
  inventory->retry_attempt = 0;
  schedule_queued_refresh(inventory);
}

void
network_sidebar_awg_inventory_set_suspended(NetworkSidebarAwgInventory *inventory,
                                            gboolean suspended)
{
  if (inventory == NULL || inventory->stopping || inventory->suspended == suspended)
    return;
  inventory->suspended = suspended;
  if (!suspended)
    schedule_queued_refresh(inventory);
}

void
network_sidebar_awg_inventory_preempt(NetworkSidebarAwgInventory *inventory)
{
  g_autoptr(NetworkSidebarAwgInventory) guard =
    network_sidebar_awg_inventory_ref(inventory);

  if (!inventory_is_running(inventory))
    return;
  network_sidebar_awg_inventory_set_suspended(inventory, TRUE);
  if (!inventory->pending)
    return;
  cancel_request(inventory);
  network_sidebar_awg_inventory_queue_refresh(inventory,
                                               AWG_INVENTORY_RECHECK_DELAY_MSEC,
                                               NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE);
  notify_changed(inventory, NETWORK_SIDEBAR_AWG_INVENTORY_STATE_CHANGED,
                 NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE);
}

static gboolean
refresh_source_cb(gpointer user_data)
{
  NetworkSidebarAwgInventory *inventory = user_data;

  inventory->refresh_source = 0;
  inventory->source_deadline_us = 0;
  if (!inventory_is_running(inventory) || !inventory->authorized) {
    inventory->dirty = FALSE;
    inventory->dirty_delay_ms = 0;
    inventory->queued_purpose = NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
    return G_SOURCE_REMOVE;
  }
  if (!inventory->enabled || inventory->suspended)
    return G_SOURCE_REMOVE;
  network_sidebar_awg_inventory_request_refresh(inventory, TRUE,
                                                 inventory->queued_purpose);
  return G_SOURCE_REMOVE;
}

static void
schedule_queued_refresh(NetworkSidebarAwgInventory *inventory)
{
  guint delay_ms;

  if (!inventory_is_running(inventory) || !inventory->authorized ||
      !inventory->enabled || inventory->suspended || !inventory->dirty ||
      inventory->pending || inventory->refresh_source != 0)
    return;
  delay_ms = MAX(inventory->dirty_delay_ms, 1u);
  inventory->refresh_source = g_timeout_add_full(
    G_PRIORITY_DEFAULT, delay_ms, refresh_source_cb,
    network_sidebar_awg_inventory_ref(inventory),
    (GDestroyNotify) network_sidebar_awg_inventory_unref);
  inventory->source_deadline_us = g_get_monotonic_time() + (gint64) delay_ms * 1000;
}

void
network_sidebar_awg_inventory_queue_refresh(NetworkSidebarAwgInventory *inventory,
                                            guint delay_ms,
                                            NetworkSidebarAwgRefreshPurpose purpose)
{
  gint64 requested_deadline_us;
  gboolean rearm_source;

  if (!inventory_is_running(inventory))
    return;
  delay_ms = MAX(delay_ms, 1u);
  requested_deadline_us = g_get_monotonic_time() + (gint64) delay_ms * 1000;
  rearm_source = inventory->refresh_source != 0 &&
    (inventory->source_deadline_us == 0 ||
     requested_deadline_us < inventory->source_deadline_us);
  if (!inventory->dirty || inventory->dirty_delay_ms == 0 ||
      delay_ms < inventory->dirty_delay_ms)
    inventory->dirty_delay_ms = delay_ms;
  inventory->dirty = TRUE;
  inventory->queued_purpose = MAX(inventory->queued_purpose, purpose);
  if (rearm_source)
    cancel_refresh_source(inventory);
  schedule_queued_refresh(inventory);
}

void
network_sidebar_awg_inventory_schedule_retry(NetworkSidebarAwgInventory *inventory,
                                             NetworkSidebarAwgRefreshPurpose purpose)
{
  static const guint delays_ms[] = { 2000u, 4000u, 8000u, 16000u, 32000u, 60000u };
  guint index;

  if (!inventory_is_running(inventory))
    return;
  index = MIN(inventory->retry_attempt, G_N_ELEMENTS(delays_ms) - 1);
  if (inventory->retry_attempt < G_N_ELEMENTS(delays_ms) - 1)
    inventory->retry_attempt++;
  network_sidebar_awg_inventory_queue_refresh(inventory, delays_ms[index], purpose);
}

static void
set_service_reachable(NetworkSidebarAwgInventory *inventory, gboolean reachable)
{
  if (reachable)
    inventory->missing_dependencies &= ~NETWORK_SIDEBAR_AWG_DEPENDENCY_SERVICE;
  else
    inventory->missing_dependencies |= NETWORK_SIDEBAR_AWG_DEPENDENCY_SERVICE;
}

void
network_sidebar_awg_inventory_set_service_reachable(
  NetworkSidebarAwgInventory *inventory,
  gboolean reachable)
{
  NetworkSidebarAwgDependency previous;

  if (!inventory_is_running(inventory))
    return;
  previous = inventory->missing_dependencies;
  set_service_reachable(inventory, reachable);
  if (previous != inventory->missing_dependencies)
    notify_changed(inventory, NETWORK_SIDEBAR_AWG_INVENTORY_STATE_CHANGED,
                   NETWORK_SIDEBAR_AWG_REFRESH_VISIBLE);
}

static gboolean
request_is_current(const InventoryRequest *request)
{
  NetworkSidebarAwgInventory *inventory = request->inventory;

  return inventory_is_running(inventory) && inventory->authorized &&
         inventory->pending && inventory->request_id == request->id;
}

static void
request_free(InventoryRequest *request)
{
  network_sidebar_awg_inventory_unref(request->inventory);
  g_free(request);
}

static void
finish_request(InventoryRequest *request, NetworkSidebarAwgInventoryChange change)
{
  NetworkSidebarAwgInventory *inventory = request->inventory;

  if (request_is_current(request)) {
    NetworkSidebarAwgRefreshPurpose purpose = inventory->pending_purpose;

    inventory->pending = FALSE;
    inventory->pending_purpose = NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
    g_clear_object(&inventory->cancellable);
    notify_changed(inventory, change, purpose);
    schedule_queued_refresh(inventory);
  }
  request_free(request);
}

static gboolean
apply_inventory_reply(NetworkSidebarAwgInventory *inventory,
                      const NetworkSidebarAwgBackendResult *result)
{
  gboolean status_is_inventory =
    result->status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS ||
    result->status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVENTORY_PARTIAL;
  gboolean shape_valid =
    (result->status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS &&
     result->inventory_complete) ||
    (result->status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVENTORY_PARTIAL &&
     !result->inventory_complete);
  NetworkSidebarAwgProfileWarningKind warning;

  inventory->capabilities_known = TRUE;
  inventory->missing_dependencies = NETWORK_SIDEBAR_AWG_DEPENDENCY_NONE;
  if ((result->capabilities & NETWORK_SIDEBAR_AMNEZIAWG_CAPABILITY_HELPER) == 0)
    inventory->missing_dependencies |= NETWORK_SIDEBAR_AWG_DEPENDENCY_HELPER;
  if ((result->capabilities & NETWORK_SIDEBAR_AMNEZIAWG_CAPABILITY_AWG_QUICK) == 0)
    inventory->missing_dependencies |= NETWORK_SIDEBAR_AWG_DEPENDENCY_AWG_QUICK;

  if (status_is_inventory && shape_valid &&
      network_sidebar_awg_model_replace_profiles(inventory->model, result->profiles)) {
    network_sidebar_awg_model_set_inventory_valid(inventory->model, TRUE);
    network_sidebar_awg_model_set_profiles_warning(
      inventory->model,
      result->inventory_complete ? NETWORK_SIDEBAR_AWG_PROFILE_WARNING_NONE :
                                   NETWORK_SIDEBAR_AWG_PROFILE_WARNING_PARTIAL,
      0);
    inventory->retry_attempt = 0;
    return TRUE;
  }
  if (result->status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_TIMED_OUT)
    warning = NETWORK_SIDEBAR_AWG_PROFILE_WARNING_TIMED_OUT;
  else if (status_is_inventory)
    warning = NETWORK_SIDEBAR_AWG_PROFILE_WARNING_INVALID_REPLY;
  else
    warning = NETWORK_SIDEBAR_AWG_PROFILE_WARNING_HELPER_STATUS;
  network_sidebar_awg_model_set_stale_profiles_warning(inventory->model,
                                                        warning, result->status);
  network_sidebar_awg_inventory_schedule_retry(inventory, inventory->pending_purpose);
  return FALSE;
}

static void
inventory_result_cb(const NetworkSidebarAwgBackendResult *result, gpointer user_data)
{
  InventoryRequest *request = user_data;
  NetworkSidebarAwgInventory *inventory = request->inventory;
  NetworkSidebarAwgInventoryChange change = NETWORK_SIDEBAR_AWG_INVENTORY_STATE_CHANGED;
  NetworkSidebarAwgProfileWarningKind warning;

  if (!request_is_current(request)) {
    request_free(request);
    return;
  }
  switch (result->kind) {
  case NETWORK_SIDEBAR_AWG_BACKEND_RESULT_REPLY:
    if (apply_inventory_reply(inventory, result))
      change = NETWORK_SIDEBAR_AWG_INVENTORY_PROFILES_CHANGED;
    break;
  case NETWORK_SIDEBAR_AWG_BACKEND_RESULT_CANCELLED:
    break;
  case NETWORK_SIDEBAR_AWG_BACKEND_RESULT_NOT_AUTHORIZED:
    set_service_reachable(inventory, result->service_replied);
    network_sidebar_awg_backend_invalidate_authorization(inventory->backend);
    break;
  default:
    if (result->kind == NETWORK_SIDEBAR_AWG_BACKEND_RESULT_TIMED_OUT) {
      warning = NETWORK_SIDEBAR_AWG_PROFILE_WARNING_TIMED_OUT;
    } else if (result->kind == NETWORK_SIDEBAR_AWG_BACKEND_RESULT_BUS_UNAVAILABLE) {
      set_service_reachable(inventory, FALSE);
      warning = NETWORK_SIDEBAR_AWG_PROFILE_WARNING_SERVICE_UNAVAILABLE;
    } else {
      set_service_reachable(inventory, result->service_replied);
      warning = result->kind == NETWORK_SIDEBAR_AWG_BACKEND_RESULT_BUSY ?
        NETWORK_SIDEBAR_AWG_PROFILE_WARNING_SERVICE_BUSY :
        NETWORK_SIDEBAR_AWG_PROFILE_WARNING_SERVICE_FAILED;
    }
    network_sidebar_awg_model_set_stale_profiles_warning(inventory->model,
                                                          warning, 0);
    network_sidebar_awg_inventory_schedule_retry(inventory, inventory->pending_purpose);
    break;
  }
  finish_request(request, change);
}

void
network_sidebar_awg_inventory_request_refresh(NetworkSidebarAwgInventory *inventory,
                                              gboolean force,
                                              NetworkSidebarAwgRefreshPurpose purpose)
{
  g_autoptr(NetworkSidebarAwgInventory) guard =
    network_sidebar_awg_inventory_ref(inventory);
  InventoryRequest *request;
  gint64 now;

  if (!inventory_is_running(inventory) || !inventory->authorized)
    return;
  if (!inventory->enabled) {
    network_sidebar_awg_inventory_queue_refresh(inventory, 1, purpose);
    return;
  }
  if (inventory->pending) {
    if (purpose > inventory->pending_purpose) {
      inventory->pending_purpose = purpose;
      notify_changed(inventory, NETWORK_SIDEBAR_AWG_INVENTORY_REFRESH_STARTED,
                     purpose);
    }
    return;
  }
  if (inventory->suspended) {
    network_sidebar_awg_inventory_queue_refresh(inventory, 1, purpose);
    return;
  }
  if (inventory->refresh_source != 0) {
    inventory->queued_purpose = MAX(inventory->queued_purpose, purpose);
    if (!force)
      return;
    cancel_refresh_source(inventory);
  }
  now = g_get_monotonic_time();
  if (!force && inventory->last_attempt_us != 0 &&
      now - inventory->last_attempt_us <
        AWG_INVENTORY_REFRESH_INTERVAL_SECONDS * G_USEC_PER_SEC)
    return;
  inventory->dirty = FALSE;
  inventory->dirty_delay_ms = 0;
  purpose = MAX(purpose, inventory->queued_purpose);
  inventory->queued_purpose = NETWORK_SIDEBAR_AWG_REFRESH_RECONCILE;
  if (inventory->request_id == G_MAXUINT64)
    return;
  inventory->request_id++;
  inventory->pending = TRUE;
  inventory->pending_purpose = purpose;
  inventory->cancellable = g_cancellable_new();
  inventory->last_attempt_us = now;
  request = g_new0(InventoryRequest, 1);
  request->inventory = network_sidebar_awg_inventory_ref(inventory);
  request->id = inventory->request_id;
  notify_changed(inventory, NETWORK_SIDEBAR_AWG_INVENTORY_REFRESH_STARTED, purpose);
  if (!request_is_current(request)) {
    request_free(request);
    return;
  }
  if (inventory->callbacks.can_dispatch != NULL &&
      !inventory->callbacks.can_dispatch(inventory->user_data)) {
    finish_request(request, NETWORK_SIDEBAR_AWG_INVENTORY_STATE_CHANGED);
    return;
  }
  if (!request_is_current(request)) {
    request_free(request);
    return;
  }
  network_sidebar_awg_backend_execute(
    inventory->backend, NETWORK_SIDEBAR_AWG_BACKEND_LIST_PROFILES, "", NULL,
    inventory->cancellable, inventory_result_cb, request);
}

gboolean
network_sidebar_awg_inventory_is_pending(const NetworkSidebarAwgInventory *inventory)
{
  return inventory != NULL && inventory->pending;
}

gboolean
network_sidebar_awg_inventory_is_loading(const NetworkSidebarAwgInventory *inventory)
{
  return inventory != NULL && inventory->pending &&
         inventory->pending_purpose == NETWORK_SIDEBAR_AWG_REFRESH_VISIBLE;
}

gboolean
network_sidebar_awg_inventory_capabilities_known(
  const NetworkSidebarAwgInventory *inventory)
{
  return inventory != NULL && inventory->capabilities_known;
}

NetworkSidebarAwgDependency
network_sidebar_awg_inventory_missing_dependencies(
  const NetworkSidebarAwgInventory *inventory)
{
  return inventory != NULL ? inventory->missing_dependencies :
                             AWG_INVENTORY_UNKNOWN_DEPENDENCIES;
}
