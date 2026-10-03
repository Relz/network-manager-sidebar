#ifndef NETWORK_SIDEBAR_AMNEZIAWG_DEADLINE_H
#define NETWORK_SIDEBAR_AMNEZIAWG_DEADLINE_H

#include <glib.h>

G_BEGIN_DECLS

#define NETWORK_SIDEBAR_AMNEZIAWG_RESOLVED_CALL_TIMEOUT_MSEC 5000u
#define NETWORK_SIDEBAR_AMNEZIAWG_CHILD_CLEANUP_RESERVE_MSEC 6000u
#define NETWORK_SIDEBAR_AMNEZIAWG_PREEMPT_TIMEOUT_MSEC 5000u

typedef struct {
  gint64 forward_at;
  gint64 cleanup_at;
  gint64 hard_at;
} NetworkSidebarAmneziaWGDeadlines;

gboolean network_sidebar_amneziawg_deadlines_init(
  const char *operation,
  gint64 start_msec,
  NetworkSidebarAmneziaWGDeadlines *deadlines);
gint64 network_sidebar_amneziawg_deadline_cap(gint64 now_msec,
                                               gint64 outer_deadline_msec,
                                               guint local_budget_msec);
gint network_sidebar_amneziawg_deadline_remaining_msec(
  gint64 now_msec,
  gint64 deadline_msec,
  guint maximum_msec);
guint network_sidebar_amneziawg_service_timeout_msec(const char *operation);
guint network_sidebar_amneziawg_client_timeout_msec(const char *operation);

G_END_DECLS

#endif
