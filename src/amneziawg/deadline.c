#include "amneziawg/deadline.h"

#include "amneziawg/amneziawg.h"

#define IMPORT_FORWARD_TIMEOUT_MSEC (30u * 1000u)
#define DELETE_PROFILE_TIMEOUT_MSEC (15u * 1000u)
#define LIST_FORWARD_TIMEOUT_MSEC (120u * 1000u)
#define TUNNEL_FORWARD_TIMEOUT_MSEC (180u * 1000u)
#define SWITCH_FORWARD_TIMEOUT_MSEC (360u * 1000u)
#define TUNNEL_CLEANUP_RESERVE_MSEC (150u * 1000u)
#define HARD_ENFORCEMENT_TIMEOUT_MSEC (5u * 1000u)
#define CLIENT_TIMEOUT_MARGIN_MSEC (20u * 1000u)

static gboolean
checked_add_msec(gint64 start, guint duration, gint64 *result)
{
  if (start < 0 || result == NULL || start > G_MAXINT64 - duration)
    return FALSE;
  *result = start + duration;
  return TRUE;
}

static gboolean
operation_budgets(const char *operation,
                  guint *forward_msec,
                  guint *cleanup_msec)
{
  *cleanup_msec = 0;
  if (g_strcmp0(operation, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_IMPORT) == 0 ||
      g_strcmp0(operation, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_REPLACE) == 0) {
    *forward_msec = IMPORT_FORWARD_TIMEOUT_MSEC;
  } else if (g_strcmp0(operation,
                       NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_DELETE) == 0) {
    /* Disconnect and deletion share one budget and rollback reserve. */
    *forward_msec = TUNNEL_FORWARD_TIMEOUT_MSEC + DELETE_PROFILE_TIMEOUT_MSEC;
    *cleanup_msec = TUNNEL_CLEANUP_RESERVE_MSEC;
  } else if (g_strcmp0(operation,
                       NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_LIST_PROFILES) == 0) {
    *forward_msec = LIST_FORWARD_TIMEOUT_MSEC;
  } else if (g_strcmp0(operation, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_UP) == 0) {
    /* Preparation, every previous disconnect, and target activation share
     * this budget. It is never restarted between sessions. */
    *forward_msec = SWITCH_FORWARD_TIMEOUT_MSEC;
    *cleanup_msec = TUNNEL_CLEANUP_RESERVE_MSEC;
  } else if (g_strcmp0(operation,
                       NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_DOWN) == 0) {
    *forward_msec = TUNNEL_FORWARD_TIMEOUT_MSEC;
    *cleanup_msec = TUNNEL_CLEANUP_RESERVE_MSEC;
  } else {
    return FALSE;
  }
  return TRUE;
}

gboolean
network_sidebar_amneziawg_deadlines_init(
  const char *operation,
  gint64 start_msec,
  NetworkSidebarAmneziaWGDeadlines *deadlines)
{
  NetworkSidebarAmneziaWGDeadlines result;
  guint forward_msec;
  guint cleanup_msec;

  if (deadlines == NULL ||
      !operation_budgets(operation, &forward_msec, &cleanup_msec) ||
      !checked_add_msec(start_msec, forward_msec, &result.forward_at) ||
      !checked_add_msec(result.forward_at, cleanup_msec, &result.cleanup_at) ||
      !checked_add_msec(result.cleanup_at,
                        HARD_ENFORCEMENT_TIMEOUT_MSEC,
                        &result.hard_at))
    return FALSE;
  *deadlines = result;
  return TRUE;
}

gint64
network_sidebar_amneziawg_deadline_cap(gint64 now_msec,
                                       gint64 outer_deadline_msec,
                                       guint local_budget_msec)
{
  gint64 local_deadline;

  if (now_msec < 0 || outer_deadline_msec < 0 ||
      now_msec >= outer_deadline_msec)
    return outer_deadline_msec;
  if (!checked_add_msec(now_msec, local_budget_msec, &local_deadline))
    return outer_deadline_msec;
  return MIN(local_deadline, outer_deadline_msec);
}

gint
network_sidebar_amneziawg_deadline_remaining_msec(
  gint64 now_msec,
  gint64 deadline_msec,
  guint maximum_msec)
{
  gint64 remaining;

  if (now_msec < 0 || deadline_msec <= now_msec || maximum_msec == 0)
    return 0;
  remaining = MIN(deadline_msec - now_msec, (gint64) maximum_msec);
  return (gint) MIN(remaining, (gint64) G_MAXINT);
}

guint
network_sidebar_amneziawg_service_timeout_msec(const char *operation)
{
  NetworkSidebarAmneziaWGDeadlines deadlines;

  return network_sidebar_amneziawg_deadlines_init(operation, 0, &deadlines) ?
    (guint) deadlines.hard_at : 0;
}

guint
network_sidebar_amneziawg_client_timeout_msec(const char *operation)
{
  guint service_timeout = network_sidebar_amneziawg_service_timeout_msec(
    operation);
  guint margin = CLIENT_TIMEOUT_MARGIN_MSEC;

  /* Every user command may wait for an authorized inventory handoff. */
  if (g_strcmp0(operation, NETWORK_SIDEBAR_AMNEZIAWG_OPERATION_LIST_PROFILES) != 0)
    margin += NETWORK_SIDEBAR_AMNEZIAWG_PREEMPT_TIMEOUT_MSEC;

  return service_timeout == 0 ||
         service_timeout > G_MAXUINT - margin ? 0 : service_timeout + margin;
}
