#ifndef NETWORK_SIDEBAR_TARGET_OUTPUT_H
#define NETWORK_SIDEBAR_TARGET_OUTPUT_H

#include <glib.h>

G_BEGIN_DECLS

#define NETWORK_SIDEBAR_TARGET_OUTPUT_MAX_CHARS 128

char *network_sidebar_normalize_target_output(const char *output_name);
char *network_sidebar_target_output_from_environment(void);

G_END_DECLS

#endif
