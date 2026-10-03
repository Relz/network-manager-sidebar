#ifndef NETWORK_SIDEBAR_AMNEZIAWG_INVENTORY_H
#define NETWORK_SIDEBAR_AMNEZIAWG_INVENTORY_H

#include "amneziawg/backend.h"
#include "amneziawg/refresh.h"

G_BEGIN_DECLS

typedef struct _NetworkSidebarAwgInventory NetworkSidebarAwgInventory;

typedef enum {
  NETWORK_SIDEBAR_AWG_INVENTORY_STATE_CHANGED,
  NETWORK_SIDEBAR_AWG_INVENTORY_REFRESH_STARTED,
  NETWORK_SIDEBAR_AWG_INVENTORY_PROFILES_CHANGED,
} NetworkSidebarAwgInventoryChange;

typedef struct {
  void (*changed)(NetworkSidebarAwgInventoryChange change,
                  NetworkSidebarAwgRefreshPurpose purpose, gpointer user_data);
  /* Optional final admission check after publishing the pending state. */
  gboolean (*can_dispatch)(gpointer user_data);
} NetworkSidebarAwgInventoryCallbacks;

/* Confined to the default main context. The backend is referenced; the model
 * and callback data are borrowed until stop, which must precede their teardown. */
NetworkSidebarAwgInventory *network_sidebar_awg_inventory_new(
  NetworkSidebarAwgBackend *backend,
  NetworkSidebarAwgModel *model,
  const NetworkSidebarAwgInventoryCallbacks *callbacks,
  gpointer user_data);
NetworkSidebarAwgInventory *network_sidebar_awg_inventory_ref(
  NetworkSidebarAwgInventory *inventory);
void network_sidebar_awg_inventory_unref(NetworkSidebarAwgInventory *inventory);
void network_sidebar_awg_inventory_start(NetworkSidebarAwgInventory *inventory);
/* Terminal: cancels requests, clears profiles/capabilities, and detaches users. */
void network_sidebar_awg_inventory_stop(NetworkSidebarAwgInventory *inventory);

/* Authorization loss invalidates in-flight results and clears profile data. */
void network_sidebar_awg_inventory_set_authorized(
  NetworkSidebarAwgInventory *inventory, gboolean authorized);
/* Visibility and user activity independently gate new refreshes. Neither
 * setter cancels an in-flight request; queued work survives both gates. */
void network_sidebar_awg_inventory_set_enabled(
  NetworkSidebarAwgInventory *inventory, gboolean enabled);
void network_sidebar_awg_inventory_set_suspended(
  NetworkSidebarAwgInventory *inventory, gboolean suspended);
/* Suspends new refreshes for a user command and invalidates the current wait
 * before cancellation. Queues an interrupted refresh for the next resume.
 * The authorized Execute request performs privileged inventory preemption. */
void network_sidebar_awg_inventory_preempt(NetworkSidebarAwgInventory *inventory);

void network_sidebar_awg_inventory_request_refresh(
  NetworkSidebarAwgInventory *inventory, gboolean force,
  NetworkSidebarAwgRefreshPurpose purpose);
void network_sidebar_awg_inventory_queue_refresh(
  NetworkSidebarAwgInventory *inventory, guint delay_ms,
  NetworkSidebarAwgRefreshPurpose purpose);
void network_sidebar_awg_inventory_schedule_retry(
  NetworkSidebarAwgInventory *inventory, NetworkSidebarAwgRefreshPurpose purpose);
/* Mutation replies also provide evidence about service reachability. */
void network_sidebar_awg_inventory_set_service_reachable(
  NetworkSidebarAwgInventory *inventory, gboolean reachable);

gboolean network_sidebar_awg_inventory_is_pending(
  const NetworkSidebarAwgInventory *inventory);
gboolean network_sidebar_awg_inventory_is_loading(
  const NetworkSidebarAwgInventory *inventory);
gboolean network_sidebar_awg_inventory_capabilities_known(
  const NetworkSidebarAwgInventory *inventory);
NetworkSidebarAwgDependency network_sidebar_awg_inventory_missing_dependencies(
  const NetworkSidebarAwgInventory *inventory);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarAwgInventory,
                              network_sidebar_awg_inventory_unref)

G_END_DECLS

#endif
