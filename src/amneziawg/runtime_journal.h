#ifndef NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_JOURNAL_H
#define NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_JOURNAL_H

#include "amneziawg/amneziawg.h"

G_BEGIN_DECLS

#define NETWORK_SIDEBAR_AMNEZIAWG_MAX_RUNTIME_MARKER_SIZE 256u

typedef enum {
  NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_INACTIVE,
  NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_ACTIVE,
  NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_RECOVERY_REQUIRED,
  NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_MANUAL_INTERVENTION,
  NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_CONFLICT,
  NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_UNKNOWN,
  NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_FIREWALL_PENDING,
} NetworkSidebarAmneziaWGRuntimeState;

typedef enum {
  NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_ACTIVATING,
  NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_ACTIVE,
  NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_REQUIRED,
  NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_UNCONFIRMED,
  NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_COMPLETE,
  NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_FIREWALL_PENDING,
} NetworkSidebarAmneziaWGJournalState;

/* Persist namespaces, not the current iptables alternatives selection. */
typedef enum {
  NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_NONE = 0,
  NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_NFT = 1u << 0,
  NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_IP4_LEGACY = 1u << 1,
  NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_IP6_LEGACY = 1u << 2,
  NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_IP4_NFT = 1u << 3,
  NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_IP6_NFT = 1u << 4,
  NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_ALL = (1u << 5) - 1,
} NetworkSidebarAmneziaWGFirewallScope;

typedef enum {
  NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_NONE,
  NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_RESOLVED,
  NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_RESOLVCONF,
} NetworkSidebarAmneziaWGDnsBackend;

typedef enum {
  NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_ABSENT,
  NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_APPLIED,
  NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_UNKNOWN,
} NetworkSidebarAmneziaWGJournalDnsState;

typedef struct {
  char link_token[NETWORK_SIDEBAR_AMNEZIAWG_LINK_TOKEN_LENGTH + 1];
  guint ifindex;
  NetworkSidebarAmneziaWGDnsBackend dns_backend;
  NetworkSidebarAmneziaWGJournalState state;
  NetworkSidebarAmneziaWGJournalDnsState dns_state;
  gboolean was_active;
  guint firewall_scope;
} NetworkSidebarAmneziaWGRuntimeMarker;

gboolean network_sidebar_amneziawg_ifindex_parse(const guint8 *data,
                                                gsize length,
                                                guint *ifindex);
gboolean network_sidebar_amneziawg_runtime_marker_parse(
  const guint8 *data,
  gsize length,
  NetworkSidebarAmneziaWGRuntimeMarker *marker);
const char *network_sidebar_amneziawg_dns_backend_name(
  NetworkSidebarAmneziaWGDnsBackend backend);
const char *network_sidebar_amneziawg_journal_state_name(
  NetworkSidebarAmneziaWGJournalState state);
gboolean network_sidebar_amneziawg_runtime_marker_serialize(
  const NetworkSidebarAmneziaWGRuntimeMarker *marker,
  guint8 **data,
  gsize *length);

/* Pure classification of a journal and an independently observed link. */
NetworkSidebarAmneziaWGRuntimeState network_sidebar_amneziawg_runtime_state_classify(
  gboolean marker_exists,
  gboolean marker_valid,
  const NetworkSidebarAmneziaWGRuntimeMarker *marker,
  const NetworkSidebarAmneziaWGLinkObservation *observation);
gboolean network_sidebar_amneziawg_runtime_state_is_recoverable(
  NetworkSidebarAmneziaWGRuntimeState state);

G_END_DECLS

#endif
