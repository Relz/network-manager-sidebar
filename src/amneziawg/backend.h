#ifndef NETWORK_SIDEBAR_AMNEZIAWG_BACKEND_H
#define NETWORK_SIDEBAR_AMNEZIAWG_BACKEND_H

#include "amneziawg/model.h"

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _NetworkSidebarAwgBackend NetworkSidebarAwgBackend;

typedef enum {
  NETWORK_SIDEBAR_AWG_BACKEND_IMPORT,
  NETWORK_SIDEBAR_AWG_BACKEND_REPLACE,
  NETWORK_SIDEBAR_AWG_BACKEND_UP,
  NETWORK_SIDEBAR_AWG_BACKEND_DOWN,
  NETWORK_SIDEBAR_AWG_BACKEND_DELETE,
  NETWORK_SIDEBAR_AWG_BACKEND_LIST_PROFILES,
} NetworkSidebarAwgBackendOperation;

typedef enum {
  NETWORK_SIDEBAR_AWG_BACKEND_RESULT_REPLY,
  NETWORK_SIDEBAR_AWG_BACKEND_RESULT_TIMED_OUT,
  NETWORK_SIDEBAR_AWG_BACKEND_RESULT_NOT_AUTHORIZED,
  NETWORK_SIDEBAR_AWG_BACKEND_RESULT_BUSY,
  NETWORK_SIDEBAR_AWG_BACKEND_RESULT_BUS_UNAVAILABLE,
  NETWORK_SIDEBAR_AWG_BACKEND_RESULT_FAILED,
  NETWORK_SIDEBAR_AWG_BACKEND_RESULT_CANCELLED,
} NetworkSidebarAwgBackendResultKind;

typedef struct {
  NetworkSidebarAwgBackendResultKind kind;
  gboolean service_replied;
  guint status;
  guint capabilities;
  gboolean inventory_complete;
  const GPtrArray *profiles;
} NetworkSidebarAwgBackendResult;

typedef struct {
  void (*authorization_changed)(gboolean authorized, gpointer user_data);
} NetworkSidebarAwgBackendCallbacks;

typedef void (*NetworkSidebarAwgBackendResultCallback)(
  /* The result and profile records are borrowed for this callback. */
  const NetworkSidebarAwgBackendResult *result,
  gpointer user_data);

NetworkSidebarAwgBackend *network_sidebar_awg_backend_new(
  const NetworkSidebarAwgBackendCallbacks *callbacks,
  gpointer user_data);
NetworkSidebarAwgBackend *network_sidebar_awg_backend_ref(
  NetworkSidebarAwgBackend *backend);
void network_sidebar_awg_backend_unref(NetworkSidebarAwgBackend *backend);

void network_sidebar_awg_backend_start(NetworkSidebarAwgBackend *backend);
void network_sidebar_awg_backend_stop(NetworkSidebarAwgBackend *backend);
void network_sidebar_awg_backend_invalidate_authorization(
  NetworkSidebarAwgBackend *backend);

/* One client deadline covers bus acquisition and the method call. The result is
 * delivered once on the calling context even if transport completion is late. */
void network_sidebar_awg_backend_execute(
  NetworkSidebarAwgBackend *backend,
  NetworkSidebarAwgBackendOperation operation,
  const char *name,
  GBytes *config,
  /* Cancels this client's wait, not an already dispatched service operation.
   * The backend never cancels this caller-owned token. */
  GCancellable *cancellable,
  NetworkSidebarAwgBackendResultCallback callback,
  gpointer user_data);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarAwgBackend,
                              network_sidebar_awg_backend_unref)

G_END_DECLS

#endif
