#ifndef NETWORK_SIDEBAR_AMNEZIAWG_EXECUTABLE_H
#define NETWORK_SIDEBAR_AMNEZIAWG_EXECUTABLE_H

#include <glib.h>

/* Only build-time paths are accepted by callers. Resolve distro alternatives
 * when requested, validate the canonical path, and pin the executable by fd.
 * Nix exceptions apply only to the explicitly configured store output roots. */
int awg_executable_open(const char *path, gboolean resolve_symlinks);

#endif
