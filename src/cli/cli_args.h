#ifndef NETWORK_SIDEBAR_CLI_ARGS_H
#define NETWORK_SIDEBAR_CLI_ARGS_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *command;
  gboolean should_exit;
  int exit_status;
} NetworkSidebarCliArgs;

gboolean network_sidebar_cli_args_parse(int *argc,
                                        char ***argv,
                                        NetworkSidebarCliArgs *args,
                                        GError **error);
void network_sidebar_cli_args_clear(NetworkSidebarCliArgs *args);

G_END_DECLS

#endif
