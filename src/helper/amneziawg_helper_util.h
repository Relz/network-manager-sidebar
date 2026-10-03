#ifndef NETWORK_SIDEBAR_AMNEZIAWG_HELPER_UTIL_H
#define NETWORK_SIDEBAR_AMNEZIAWG_HELPER_UTIL_H

#include "amneziawg/amneziawg.h"

void awg_helper_wipe_bytes(void *data, gsize length);
gboolean sanitize_gio_environment(void);
void awg_helper_close_fd(int *fd);
gboolean awg_helper_write_all(int fd, const guint8 *contents, gsize length);

NetworkSidebarAmneziaWGHelperExit awg_helper_phase_stop_status(gint64 deadline);
NetworkSidebarAmneziaWGHelperExit awg_helper_phase_failure_or(
  gint64 deadline,
  NetworkSidebarAmneziaWGHelperExit fallback);

#endif
