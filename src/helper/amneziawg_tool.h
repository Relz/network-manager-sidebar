#ifndef NETWORK_SIDEBAR_AMNEZIAWG_TOOL_H
#define NETWORK_SIDEBAR_AMNEZIAWG_TOOL_H

#include <glib.h>

#define AWG_TOOL_OUTPUT_LIMIT (1024u * 1024u)

typedef enum {
  AWG_TOOL_AVAILABLE,
  AWG_TOOL_MISSING,
  AWG_TOOL_UNTRUSTED,
} AwgToolAvailability;

typedef struct _AwgTool AwgTool;

/* Fixed system paths only. Root-owned distro multicall symlinks are supported. */
AwgToolAvailability awg_tool_available(const char *name);
/* Keep the same executable pinned across version detection and collection. */
AwgTool *awg_tool_open(const char *name);
void awg_tool_free(AwgTool *tool);
/* A successful, completely captured, reaped invocation only. No shell is used.
 * Callers supply fixed query arguments. All output remains helper-private. */
gboolean awg_tool_read(AwgTool *tool, const char *const argv[],
                       gint64 deadline, GBytes **output);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AwgTool, awg_tool_free)

#endif
