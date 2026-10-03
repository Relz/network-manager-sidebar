#ifndef NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_SOURCE_H
#define NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_SOURCE_H

#include "amneziawg/model.h"
#include "amneziawg/refresh.h"

#include <glib.h>

G_BEGIN_DECLS

typedef struct _NetworkSidebarAwgRuntimeSource NetworkSidebarAwgRuntimeSource;

typedef void (*NetworkSidebarAwgRuntimeChangedCallback)(gpointer user_data);

/* This object and its callback are confined to the default main context. */
NetworkSidebarAwgRuntimeSource *network_sidebar_awg_runtime_source_new(
  NetworkSidebarAwgRuntimeChangedCallback changed,
  gpointer user_data);
NetworkSidebarAwgRuntimeSource *network_sidebar_awg_runtime_source_ref(
  NetworkSidebarAwgRuntimeSource *source);
void network_sidebar_awg_runtime_source_unref(
  NetworkSidebarAwgRuntimeSource *source);

void network_sidebar_awg_runtime_source_start(
  NetworkSidebarAwgRuntimeSource *source);
void network_sidebar_awg_runtime_source_stop(
  NetworkSidebarAwgRuntimeSource *source);
void network_sidebar_awg_runtime_source_set_monitor_enabled(
  NetworkSidebarAwgRuntimeSource *source,
  gboolean enabled);
void network_sidebar_awg_runtime_source_ensure_monitor(
  NetworkSidebarAwgRuntimeSource *source);

/* Additional saved-profile names to inspect; runtime journals are discovered
 * independently. Activity/presentation names do not belong in this set.
 * A changed normalized name set invalidates the cache and schedules one
 * asynchronous, coalesced refresh; an unchanged set preserves freshness. */
void network_sidebar_awg_runtime_source_set_names(
  NetworkSidebarAwgRuntimeSource *source,
  const GPtrArray *names,
  NetworkSidebarAwgRefreshPurpose purpose);
/* Background recheck: retain monitored observations until the result arrives.
 * Coalesces with any scheduled/running scan. Changes and observation failures
 * still invalidate records, including during an in-flight poll. */
void network_sidebar_awg_runtime_source_poll(
  NetworkSidebarAwgRuntimeSource *source,
  NetworkSidebarAwgRefreshPurpose purpose);
/* Explicit invalidation after a mutation or other evidence of a change. */
void network_sidebar_awg_runtime_source_request_refresh(
  NetworkSidebarAwgRuntimeSource *source,
  NetworkSidebarAwgRefreshPurpose purpose);
/* Profile mutations can generate monitor events before their reply arrives.
 * The controller releases this scope on the reply; quiet refreshes retain their
 * own reconciliation scope until verification settles. Normal requests win. */
void network_sidebar_awg_runtime_source_set_mutation_active(
  NetworkSidebarAwgRuntimeSource *source, gboolean active);
/* Successful Delete confirms the journal and interface are absent. Forget the
 * old observation and invalidate in-flight scans before background rechecking. */
void network_sidebar_awg_runtime_source_record_delete(
  NetworkSidebarAwgRuntimeSource *source,
  const char *name);

/* A scan is scheduled or in flight, including awaiting publication. Retry
 * backoff alone is not checking: a failed attempt must remain visible as such.
 * Progress transitions are reported through the changed callback. */
gboolean network_sidebar_awg_runtime_source_is_checking(
  NetworkSidebarAwgRuntimeSource *source);
gboolean network_sidebar_awg_runtime_source_is_loading(
  NetworkSidebarAwgRuntimeSource *source);

/* These cache-only operations never perform filesystem or netlink I/O. */
GPtrArray *network_sidebar_awg_runtime_source_dup_cached_records(
  NetworkSidebarAwgRuntimeSource *source);
gboolean network_sidebar_awg_runtime_source_get_cached(
  NetworkSidebarAwgRuntimeSource *source,
  const char *name,
  NetworkSidebarAmneziaWGRuntimeState *state,
  gboolean *interface_exists);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarAwgRuntimeSource,
                              network_sidebar_awg_runtime_source_unref)

G_END_DECLS

#endif
