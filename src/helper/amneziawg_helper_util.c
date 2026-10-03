#include "helper/amneziawg_helper_util.h"

#include "helper/amneziawg_process.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

gboolean
sanitize_gio_environment(void)
{
  static const char *const variables[] = {
    "DBUS_SYSTEM_BUS_ADDRESS",
    "DBUS_SESSION_BUS_ADDRESS",
    "DBUS_STARTER_ADDRESS",
    "DBUS_STARTER_BUS_TYPE",
    "G_DBUS_COOKIE_SHA1_KEYRING_DIR",
    "GIO_EXTRA_MODULES",
    "GIO_MODULE_DIR",
    "GIO_USE_FILE_MONITOR",
    "GIO_USE_MEMORY_MONITOR",
    "GIO_USE_NETWORK_MONITOR",
    "GIO_USE_PORTALS",
    "GIO_USE_POWER_PROFILE_MONITOR",
    "GIO_USE_PROXY_RESOLVER",
    "GIO_USE_TLS",
    "GIO_USE_VOLUME_MONITOR",
    "G_MESSAGES_DEBUG",
  };

  for (gsize i = 0; i < G_N_ELEMENTS(variables); i++) {
    if (unsetenv(variables[i]) != 0)
      return FALSE;
  }
  return setenv("GIO_USE_VFS", "local", 1) == 0;
}

void
awg_helper_wipe_bytes(void *data, gsize length)
{
  volatile guint8 *bytes = data;

  while (length-- > 0)
    *bytes++ = 0;
}

void
awg_helper_close_fd(int *fd)
{
  if (*fd >= 0)
    close(*fd);
  *fd = -1;
}

gboolean
awg_helper_write_all(int fd, const guint8 *contents, gsize length)
{
  gsize offset = 0;

  while (offset < length) {
    ssize_t count;

    network_sidebar_amneziawg_process_dispatch_termination();
    if (network_sidebar_amneziawg_process_termination_requested())
      return FALSE;
    count = write(fd, contents + offset, length - offset);
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return FALSE;
    }
    if (count == 0)
      return FALSE;
    offset += (gsize) count;
  }
  return TRUE;
}

NetworkSidebarAmneziaWGHelperExit
awg_helper_phase_stop_status(gint64 deadline)
{
  gint64 now;

  network_sidebar_amneziawg_process_dispatch_termination();
  if (network_sidebar_amneziawg_process_termination_requested())
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SIGNALED;
  now = network_sidebar_amneziawg_process_monotonic_msec();
  return now < 0 || now >= deadline ?
    NETWORK_SIDEBAR_AMNEZIAWG_HELPER_TIMED_OUT :
    NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
}

NetworkSidebarAmneziaWGHelperExit
awg_helper_phase_failure_or(gint64 deadline,
                            NetworkSidebarAmneziaWGHelperExit fallback)
{
  NetworkSidebarAmneziaWGHelperExit stop = awg_helper_phase_stop_status(deadline);

  return stop == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS ? fallback : stop;
}
