#ifndef NETWORK_SIDEBAR_IPC_PATHS_H
#define NETWORK_SIDEBAR_IPC_PATHS_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  NETWORK_SIDEBAR_IPC_PATH_ERROR_FAILED,
} NetworkSidebarIPCPathError;

#define NETWORK_SIDEBAR_IPC_PATH_ERROR (network_sidebar_ipc_path_error_quark())

GQuark network_sidebar_ipc_path_error_quark(void);
char *network_sidebar_runtime_socket_path(GError **error);

G_END_DECLS

#endif
