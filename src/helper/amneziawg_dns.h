#ifndef NETWORK_SIDEBAR_AMNEZIAWG_DNS_H
#define NETWORK_SIDEBAR_AMNEZIAWG_DNS_H

#include "amneziawg/dns_config.h"
#include "amneziawg/link_identity.h"
#include "amneziawg/runtime_journal.h"

typedef enum {
  DNS_CHANGE_OK,
  DNS_CHANGE_UNAVAILABLE,
  DNS_CHANGE_FAILED,
  /* Link continuity, resolver identity, or a dispatched call's outcome was lost.
   * Automatic rollback must not issue more resolver mutations. */
  DNS_CHANGE_UNCERTAIN,
} DnsChangeResult;

typedef struct {
  DnsChangeResult result;
  /* FALSE guarantees that this operation sent no resolver mutations. */
  gboolean may_have_changed;
} DnsChangeOutcome;

gboolean dns_config_present(const NetworkSidebarAmneziaWGDnsConfig *dns);
DnsChangeResult dns_select_backend(
  const NetworkSidebarAmneziaWGDnsConfig *dns,
  NetworkSidebarAmneziaWGDnsBackend *backend,
  gint64 deadline);
/* Mutate only the selected/journaled backend. Resolver-specific executable
 * lookup and fallback commands are private implementation details. */
DnsChangeOutcome dns_apply_selected(
  const NetworkSidebarAmneziaWGLinkIdentity *identity,
  const NetworkSidebarAmneziaWGDnsConfig *dns,
  NetworkSidebarAmneziaWGDnsBackend backend,
  gint64 deadline);
DnsChangeOutcome dns_revert_selected(
  const NetworkSidebarAmneziaWGLinkIdentity *identity,
  NetworkSidebarAmneziaWGDnsBackend backend,
  gint64 deadline);

#endif
