#ifndef NETWORK_SIDEBAR_AMNEZIAWG_CONTROLLER_H
#define NETWORK_SIDEBAR_AMNEZIAWG_CONTROLLER_H

#include "amneziawg/model.h"

#include <glib.h>

G_BEGIN_DECLS

typedef struct _NetworkSidebarAwgController NetworkSidebarAwgController;

typedef void (*NetworkSidebarAwgChangedCallback)(guint delay_ms,
                                                  gpointer user_data);

NetworkSidebarAwgController *network_sidebar_awg_controller_new(
  NetworkSidebarAwgChangedCallback changed,
  gpointer user_data);
NetworkSidebarAwgController *network_sidebar_awg_controller_ref(
  NetworkSidebarAwgController *controller);
void network_sidebar_awg_controller_unref(
  NetworkSidebarAwgController *controller);

void network_sidebar_awg_controller_start(
  NetworkSidebarAwgController *controller);
/* Stop is terminal and must precede teardown of callback user_data. */
void network_sidebar_awg_controller_stop(
  NetworkSidebarAwgController *controller);
/* Disabling inventory cancels queued refreshes, not in-flight operations. */
void network_sidebar_awg_controller_set_inventory_enabled(
  NetworkSidebarAwgController *controller,
  gboolean enabled);

/* The returned snapshot, its strings, and every entry are deep-owned. */
NetworkSidebarAwgSnapshot *network_sidebar_awg_controller_dup_snapshot(
  NetworkSidebarAwgController *controller);
void network_sidebar_awg_controller_free_snapshot(
  NetworkSidebarAwgSnapshot *snapshot);

void network_sidebar_awg_controller_request_inventory(
  NetworkSidebarAwgController *controller,
  gboolean force);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarAwgController,
                              network_sidebar_awg_controller_unref)

G_END_DECLS

#endif
