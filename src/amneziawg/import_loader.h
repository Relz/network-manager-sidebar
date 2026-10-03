#ifndef NETWORK_SIDEBAR_AMNEZIAWG_IMPORT_LOADER_H
#define NETWORK_SIDEBAR_AMNEZIAWG_IMPORT_LOADER_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _NetworkSidebarAwgImportLoader NetworkSidebarAwgImportLoader;

typedef enum {
  NETWORK_SIDEBAR_AWG_IMPORT_LOAD_SUCCESS,
  NETWORK_SIDEBAR_AWG_IMPORT_LOAD_INSPECT_FAILED,
  NETWORK_SIDEBAR_AWG_IMPORT_LOAD_NOT_LOCAL,
  NETWORK_SIDEBAR_AWG_IMPORT_LOAD_NOT_REGULAR,
  NETWORK_SIDEBAR_AWG_IMPORT_LOAD_TOO_LARGE,
  NETWORK_SIDEBAR_AWG_IMPORT_LOAD_OPEN_FAILED,
  NETWORK_SIDEBAR_AWG_IMPORT_LOAD_READ_FAILED,
  NETWORK_SIDEBAR_AWG_IMPORT_LOAD_EMPTY,
  NETWORK_SIDEBAR_AWG_IMPORT_LOAD_TIMED_OUT,
  NETWORK_SIDEBAR_AWG_IMPORT_LOAD_CANCELLED,
} NetworkSidebarAwgImportLoadResult;

typedef void (*NetworkSidebarAwgImportLoadCallback)(
  NetworkSidebarAwgImportLoader *loader,
  NetworkSidebarAwgImportLoadResult result,
  /* Borrowed for this callback; successful consumers must take a reference. */
  GBytes *config,
  gpointer user_data);

NetworkSidebarAwgImportLoader *network_sidebar_awg_import_loader_new(
  GFile *file,
  NetworkSidebarAwgImportLoadCallback callback,
  gpointer user_data);
NetworkSidebarAwgImportLoader *network_sidebar_awg_import_loader_ref(
  NetworkSidebarAwgImportLoader *loader);
void network_sidebar_awg_import_loader_unref(
  NetworkSidebarAwgImportLoader *loader);

/* Lifecycle calls and callbacks use the default main context's thread.
 * Only files exposing a local path are accepted. */
void network_sidebar_awg_import_loader_start(
  NetworkSidebarAwgImportLoader *loader);
void network_sidebar_awg_import_loader_cancel(
  NetworkSidebarAwgImportLoader *loader);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarAwgImportLoader,
                              network_sidebar_awg_import_loader_unref)

G_END_DECLS

#endif
