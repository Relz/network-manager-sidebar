#ifndef NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_REPORT_H
#define NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_REPORT_H

#include "amneziawg/amneziawg.h"

G_BEGIN_DECLS

typedef struct {
  char *name;
  NetworkSidebarAmneziaWGProfileStatus status;
} NetworkSidebarAwgProfileReportEntry;

typedef struct {
  GPtrArray *records;
  gboolean complete;
} NetworkSidebarAwgProfileReport;

NetworkSidebarAwgProfileReport *network_sidebar_awg_profile_report_new(void);
void network_sidebar_awg_profile_report_free(NetworkSidebarAwgProfileReport *report);
gboolean network_sidebar_awg_profile_report_add(
  NetworkSidebarAwgProfileReport *report,
  const char *name,
  NetworkSidebarAmneziaWGProfileStatus status);

/* The bounded helper wire format contains only validated names/status codes.
 * Both directions reject duplicates and invalid records. No I/O or D-Bus types
 * are involved; callers own sorting, transport, and presentation. */
GBytes *network_sidebar_awg_profile_report_encode(
  const NetworkSidebarAwgProfileReport *report);
NetworkSidebarAwgProfileReport *network_sidebar_awg_profile_report_decode(
  const guint8 *data,
  gsize length,
  NetworkSidebarAmneziaWGHelperExit status);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarAwgProfileReport,
                              network_sidebar_awg_profile_report_free)

G_END_DECLS

#endif
