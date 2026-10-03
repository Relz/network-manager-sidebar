#ifndef NETWORK_SIDEBAR_AMNEZIAWG_LINK_IDENTITY_H
#define NETWORK_SIDEBAR_AMNEZIAWG_LINK_IDENTITY_H

#include "amneziawg/amneziawg.h"

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
  const char *name;
  const char *link_token;
  guint ifindex;
} NetworkSidebarAmneziaWGLinkIdentity;

typedef struct _NetworkSidebarAmneziaWGLinkGuard
  NetworkSidebarAmneziaWGLinkGuard;

/* Requires a valid name/token and an ifindex in 1..G_MAXINT. Copies the identity
 * and subscribes before the initial check; returns NULL on failure.
 * Deadlines are absolute monotonic milliseconds (g_get_monotonic_time() / 1000);
 * each check also has a 1s budget. An optional cancellable bounds all waits.
 * Use on one thread, checking before and after synchronous operations. Queued
 * events are read directly, without a main loop. This does not lock the link. */
NetworkSidebarAmneziaWGLinkGuard *network_sidebar_amneziawg_link_guard_new(
  const NetworkSidebarAmneziaWGLinkIdentity *identity,
  gint64 deadline_msec,
  GCancellable *cancellable);
/* FALSE is permanent for this guard, including deadline or event-drain budget
 * exhaustion or cancellation. A later matching snapshot cannot restore
 * continuity. */
gboolean network_sidebar_amneziawg_link_guard_check(
  NetworkSidebarAmneziaWGLinkGuard *guard,
  gint64 deadline_msec,
  GCancellable *cancellable);
void network_sidebar_amneziawg_link_guard_free(
  NetworkSidebarAmneziaWGLinkGuard *guard);

typedef struct _NetworkSidebarAmneziaWGLinkSnapshot
  NetworkSidebarAmneziaWGLinkSnapshot;

/* Reads every link under one absolute deadline. */
NetworkSidebarAmneziaWGLinkSnapshot *
network_sidebar_amneziawg_link_snapshot_read(GCancellable *cancellable);
void network_sidebar_amneziawg_link_snapshot_free(
  NetworkSidebarAmneziaWGLinkSnapshot *snapshot);
gboolean network_sidebar_amneziawg_link_snapshot_observe(
  const NetworkSidebarAmneziaWGLinkSnapshot *snapshot,
  const char *expected_name,
  const char *link_token,
  NetworkSidebarAmneziaWGLinkObservation *observation);

typedef struct _NetworkSidebarAmneziaWGLinkEventFilter
  NetworkSidebarAmneziaWGLinkEventFilter;

/* Pure event filtering for a monitored runtime observation. Add every profile
 * and journal identity before using it. Names/tokens are copied; a token and
 * recorded ifindex are optional. The snapshot contributes all matching link
 * indices, including renamed links and duplicate ownership aliases. Use the
 * filter only while its associated complete runtime observation is valid. */
NetworkSidebarAmneziaWGLinkEventFilter *
network_sidebar_amneziawg_link_event_filter_new(void);
void network_sidebar_amneziawg_link_event_filter_add(
  NetworkSidebarAmneziaWGLinkEventFilter *filter,
  const NetworkSidebarAmneziaWGLinkSnapshot *snapshot,
  const NetworkSidebarAmneziaWGLinkIdentity *identity);
/* Accepts one complete kernel rtnetlink datagram. TRUE also covers malformed
 * or unexpected messages and a missing filter: uncertainty must invalidate.
 * Partial link notifications can omit name/alias and still match by ifindex. */
gboolean network_sidebar_amneziawg_link_event_filter_is_relevant(
  const NetworkSidebarAmneziaWGLinkEventFilter *filter,
  const void *data,
  gsize length);
void network_sidebar_amneziawg_link_event_filter_free(
  NetworkSidebarAmneziaWGLinkEventFilter *filter);

/* Takes one consistent rtnetlink snapshot. A token is optional when only the
 * presence of the expected interface name is needed. */
gboolean network_sidebar_amneziawg_link_observe(
  const char *expected_name,
  const char *link_token,
  NetworkSidebarAmneziaWGLinkObservation *observation);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarAmneziaWGLinkSnapshot,
                              network_sidebar_amneziawg_link_snapshot_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarAmneziaWGLinkGuard,
                              network_sidebar_amneziawg_link_guard_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarAmneziaWGLinkEventFilter,
                              network_sidebar_amneziawg_link_event_filter_free)

G_END_DECLS

#endif
