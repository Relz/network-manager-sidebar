#include "amneziawg/runtime_journal.h"

#include <string.h>

gboolean
network_sidebar_amneziawg_ifindex_parse(const guint8 *data,
                                         gsize length,
                                         guint *ifindex)
{
  guint value = 0;
  gsize start = 0;
  gsize end = length;

  if (data == NULL || ifindex == NULL)
    return FALSE;
  while (start < end && g_ascii_isspace(data[start]))
    start++;
  while (end > start && g_ascii_isspace(data[end - 1]))
    end--;
  if (start == end)
    return FALSE;

  for (gsize i = start; i < end; i++) {
    guint digit;

    if (!g_ascii_isdigit(data[i]))
      return FALSE;
    digit = data[i] - '0';
    if (value > (G_MAXUINT - digit) / 10)
      return FALSE;
    value = value * 10 + digit;
  }
  if (value == 0)
    return FALSE;
  *ifindex = value;
  return TRUE;
}

const char *
network_sidebar_amneziawg_dns_backend_name(NetworkSidebarAmneziaWGDnsBackend backend)
{
  switch (backend) {
  case NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_NONE:
    return "none";
  case NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_RESOLVED:
    return "resolved";
  case NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_RESOLVCONF:
    return "resolvconf";
  default:
    return NULL;
  }
}

const char *
network_sidebar_amneziawg_journal_state_name(NetworkSidebarAmneziaWGJournalState state)
{
  switch (state) {
  case NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_ACTIVATING:
    return "activating";
  case NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_ACTIVE:
    return "active";
  case NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_REQUIRED:
    return "cleanup-required";
  case NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_UNCONFIRMED:
    return "cleanup-unconfirmed";
  case NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_COMPLETE:
    return "cleanup-complete";
  case NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_FIREWALL_PENDING:
    return "firewall-verification-pending";
  default:
    return NULL;
  }
}

static const char *
journal_dns_state_name(NetworkSidebarAmneziaWGJournalDnsState state)
{
  switch (state) {
  case NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_ABSENT:
    return "absent";
  case NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_APPLIED:
    return "applied";
  case NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_UNKNOWN:
    return "unknown";
  default:
    return NULL;
  }
}

static gboolean
runtime_marker_fields_are_valid(
  const NetworkSidebarAmneziaWGRuntimeMarker *marker)
{
  if (marker == NULL ||
      network_sidebar_amneziawg_journal_state_name(marker->state) == NULL ||
      network_sidebar_amneziawg_dns_backend_name(marker->dns_backend) == NULL ||
      journal_dns_state_name(marker->dns_state) == NULL ||
      (marker->firewall_scope & ~NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_ALL) != 0 ||
      !network_sidebar_amneziawg_link_token_is_valid(marker->link_token))
    return FALSE;
  if (marker->dns_backend == NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_NONE &&
      marker->dns_state != NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_ABSENT)
    return FALSE;
  if (marker->state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_ACTIVATING &&
      marker->was_active)
    return FALSE;
  if (marker->state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_ACTIVE &&
      (marker->ifindex == 0 || !marker->was_active ||
       (marker->dns_backend != NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_NONE &&
        marker->dns_state != NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_APPLIED)))
    return FALSE;
  if ((marker->state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_COMPLETE ||
       marker->state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_FIREWALL_PENDING) &&
      (marker->was_active ||
       marker->dns_state != NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_ABSENT))
    return FALSE;
  return TRUE;
}

gboolean
network_sidebar_amneziawg_runtime_marker_serialize(
  const NetworkSidebarAmneziaWGRuntimeMarker *marker,
  guint8 **data,
  gsize *length)
{
  const char *state;
  const char *backend;
  const char *dns_state;
  char buffer[NETWORK_SIDEBAR_AMNEZIAWG_MAX_RUNTIME_MARKER_SIZE];
  int written;

  if (marker == NULL || data == NULL || length == NULL)
    return FALSE;
  state = network_sidebar_amneziawg_journal_state_name(marker->state);
  backend = network_sidebar_amneziawg_dns_backend_name(marker->dns_backend);
  dns_state = journal_dns_state_name(marker->dns_state);
  if (!runtime_marker_fields_are_valid(marker))
    return FALSE;
  written = g_snprintf(buffer,
                       sizeof(buffer),
                       "state=%s\ntoken=%s\nifindex=%u\ndns=%s\n"
                       "was-active=%u\ndns-state=%s\nfirewall-scope=%u\n",
                       state,
                       marker->link_token,
                       marker->ifindex,
                       backend,
                       marker->was_active ? 1u : 0u,
                       dns_state,
                       marker->firewall_scope);
  if (written <= 0 || (gsize) written >= sizeof(buffer))
    return FALSE;
  *data = g_memdup2(buffer, (gsize) written);
  *length = (gsize) written;
  return TRUE;
}

gboolean
network_sidebar_amneziawg_runtime_marker_parse(
  const guint8 *data,
  gsize length,
  NetworkSidebarAmneziaWGRuntimeMarker *marker)
{
  static const char state_prefix[] = "state=";
  const guint8 *state_start;
  const guint8 *state_end;
  const guint8 *token_start;
  const guint8 *token_end;
  const guint8 *ifindex_start;
  const guint8 *ifindex_end;
  const guint8 *dns_start;
  const guint8 *dns_end;
  const guint8 *was_active_start;
  const guint8 *was_active_end;
  const guint8 *dns_state_start;
  const guint8 *dns_state_end;
  const guint8 *firewall_start;
  const guint8 *firewall_end;
  guint firewall_scope = 0;
  guint ifindex;
  gboolean was_active;
  char link_token[NETWORK_SIDEBAR_AMNEZIAWG_LINK_TOKEN_LENGTH + 1];
  NetworkSidebarAmneziaWGDnsBackend backend;
  NetworkSidebarAmneziaWGJournalState state;
  NetworkSidebarAmneziaWGJournalDnsState dns_state;
  NetworkSidebarAmneziaWGRuntimeMarker parsed;

  if (data == NULL || marker == NULL || length <= sizeof(state_prefix) - 1 ||
      data[length - 1] != '\n' ||
      memcmp(data, state_prefix, sizeof(state_prefix) - 1) != 0)
    return FALSE;
  state_start = data + sizeof(state_prefix) - 1;
  state_end = memchr(state_start, '\n', length - (gsize) (state_start - data));
  if (state_end == NULL)
    return FALSE;
  if ((gsize) (state_end - state_start) == strlen("activating") &&
      memcmp(state_start, "activating", strlen("activating")) == 0)
    state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_ACTIVATING;
  else if ((gsize) (state_end - state_start) == strlen("active") &&
           memcmp(state_start, "active", strlen("active")) == 0)
    state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_ACTIVE;
  else if ((gsize) (state_end - state_start) == strlen("cleanup-required") &&
           memcmp(state_start, "cleanup-required", strlen("cleanup-required")) == 0)
    state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_REQUIRED;
  else if ((gsize) (state_end - state_start) == strlen("cleanup-unconfirmed") &&
           memcmp(state_start, "cleanup-unconfirmed", strlen("cleanup-unconfirmed")) == 0)
    state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_UNCONFIRMED;
  else if ((gsize) (state_end - state_start) == strlen("cleanup-complete") &&
           memcmp(state_start, "cleanup-complete", strlen("cleanup-complete")) == 0)
    state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_COMPLETE;
  else if ((gsize) (state_end - state_start) == strlen("firewall-verification-pending") &&
           memcmp(state_start, "firewall-verification-pending",
                  strlen("firewall-verification-pending")) == 0)
    state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_FIREWALL_PENDING;
  else
    return FALSE;
  token_start = state_end + 1;
  if ((gsize) (data + length - token_start) <
        sizeof("token=") - 1 + NETWORK_SIDEBAR_AMNEZIAWG_LINK_TOKEN_LENGTH + 1 ||
      memcmp(token_start, "token=", sizeof("token=") - 1) != 0)
    return FALSE;
  token_start += sizeof("token=") - 1;
  token_end = memchr(token_start,
                     '\n',
                     length - (gsize) (token_start - data));
  if (token_end == NULL ||
      (gsize) (token_end - token_start) !=
        NETWORK_SIDEBAR_AMNEZIAWG_LINK_TOKEN_LENGTH)
    return FALSE;
  memcpy(link_token,
         token_start,
         NETWORK_SIDEBAR_AMNEZIAWG_LINK_TOKEN_LENGTH);
  link_token[NETWORK_SIDEBAR_AMNEZIAWG_LINK_TOKEN_LENGTH] = '\0';
  if (!network_sidebar_amneziawg_link_token_is_valid(link_token))
    return FALSE;

  ifindex_start = token_end + 1;
  if ((gsize) (data + length - ifindex_start) < 9 || memcmp(ifindex_start, "ifindex=", 8) != 0)
    return FALSE;
  ifindex_start += 8;
  ifindex_end = memchr(ifindex_start, '\n', length - (gsize) (ifindex_start - data));
  if (ifindex_end == NULL)
    return FALSE;
  if (ifindex_start == ifindex_end) {
    return FALSE;
  } else if (ifindex_end - ifindex_start == 1 && ifindex_start[0] == '0') {
    ifindex = 0;
  } else {
    if (ifindex_start[0] == '0')
      return FALSE;
    for (const guint8 *cursor = ifindex_start; cursor < ifindex_end; cursor++) {
      if (!g_ascii_isdigit(*cursor))
        return FALSE;
    }
    if (!network_sidebar_amneziawg_ifindex_parse(
          ifindex_start, (gsize) (ifindex_end - ifindex_start), &ifindex))
      return FALSE;
  }
  dns_start = ifindex_end + 1;
  if ((gsize) (data + length - dns_start) < 5 || memcmp(dns_start, "dns=", 4) != 0)
    return FALSE;
  dns_start += 4;
  dns_end = memchr(dns_start, '\n', length - (gsize) (dns_start - data));
  if (dns_end == NULL || dns_start == dns_end)
    return FALSE;

  if ((gsize) (dns_end - dns_start) == strlen("none") &&
      memcmp(dns_start, "none", strlen("none")) == 0)
    backend = NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_NONE;
  else if ((gsize) (dns_end - dns_start) == strlen("resolved") &&
           memcmp(dns_start, "resolved", strlen("resolved")) == 0)
    backend = NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_RESOLVED;
  else if ((gsize) (dns_end - dns_start) == strlen("resolvconf") &&
           memcmp(dns_start, "resolvconf", strlen("resolvconf")) == 0)
    backend = NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_RESOLVCONF;
  else
    return FALSE;

  was_active_start = dns_end + 1;
  if ((gsize) (data + length - was_active_start) < sizeof("was-active=0\n") - 1 ||
      memcmp(was_active_start,
             "was-active=",
             sizeof("was-active=") - 1) != 0)
    return FALSE;
  was_active_start += sizeof("was-active=") - 1;
  was_active_end = memchr(was_active_start,
                          '\n',
                          length - (gsize) (was_active_start - data));
  if (was_active_end == NULL || was_active_end - was_active_start != 1 ||
      (was_active_start[0] != '0' && was_active_start[0] != '1'))
    return FALSE;
  was_active = was_active_start[0] == '1';

  dns_state_start = was_active_end + 1;
  if ((gsize) (data + length - dns_state_start) < sizeof("dns-state=x\n") - 1 ||
      memcmp(dns_state_start,
             "dns-state=",
             sizeof("dns-state=") - 1) != 0)
    return FALSE;
  dns_state_start += sizeof("dns-state=") - 1;
  dns_state_end = memchr(dns_state_start, '\n',
                         length - (gsize) (dns_state_start - data));
  if (dns_state_end == NULL || dns_state_start >= dns_state_end)
    return FALSE;
  if ((gsize) (dns_state_end - dns_state_start) == strlen("absent") &&
      memcmp(dns_state_start, "absent", strlen("absent")) == 0)
    dns_state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_ABSENT;
  else if ((gsize) (dns_state_end - dns_state_start) == strlen("applied") &&
           memcmp(dns_state_start, "applied", strlen("applied")) == 0)
    dns_state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_APPLIED;
  else if ((gsize) (dns_state_end - dns_state_start) == strlen("unknown") &&
           memcmp(dns_state_start, "unknown", strlen("unknown")) == 0)
    dns_state = NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_UNKNOWN;
  else
    return FALSE;

  /* Missing scope is not evidence that the old session used no firewall. */
  firewall_start = dns_state_end + 1;
  if ((gsize) (data + length - firewall_start) < sizeof("firewall-scope=0\n") - 1 ||
      memcmp(firewall_start, "firewall-scope=", sizeof("firewall-scope=") - 1) != 0)
    return FALSE;
  firewall_start += sizeof("firewall-scope=") - 1;
  firewall_end = data + length - 1;
  if (firewall_end - firewall_start > 2 ||
      (firewall_end - firewall_start > 1 && *firewall_start == '0'))
    return FALSE;
  for (const guint8 *cursor = firewall_start; cursor < firewall_end; cursor++) {
    if (!g_ascii_isdigit(*cursor))
      return FALSE;
    firewall_scope = firewall_scope * 10 + (*cursor - '0');
  }

  parsed = (NetworkSidebarAmneziaWGRuntimeMarker) {
    .ifindex = ifindex,
    .dns_backend = backend,
    .state = state,
    .dns_state = dns_state,
    .was_active = was_active,
    .firewall_scope = firewall_scope,
  };
  g_strlcpy(parsed.link_token, link_token, sizeof(parsed.link_token));
  if (!runtime_marker_fields_are_valid(&parsed))
    return FALSE;
  *marker = parsed;
  return TRUE;
}

NetworkSidebarAmneziaWGRuntimeState
network_sidebar_amneziawg_runtime_state_classify(
  gboolean marker_exists,
  gboolean marker_valid,
  const NetworkSidebarAmneziaWGRuntimeMarker *marker,
  const NetworkSidebarAmneziaWGLinkObservation *observation)
{
  if (!marker_exists)
    return observation != NULL && observation->named_exists ?
      NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_CONFLICT :
      NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_INACTIVE;
  if (!marker_valid || marker == NULL || observation == NULL)
    return NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_MANUAL_INTERVENTION;
  if (marker->state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_UNCONFIRMED ||
      (marker->dns_backend == NETWORK_SIDEBAR_AMNEZIAWG_DNS_BACKEND_RESOLVED &&
       marker->dns_state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_UNKNOWN))
    return NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_MANUAL_INTERVENTION;
  if (observation->token_matches > 1)
    return NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_MANUAL_INTERVENTION;
  if (observation->token_matches == 0) {
    if (observation->named_exists)
      return NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_CONFLICT;
    if (marker->state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_FIREWALL_PENDING)
      return marker->dns_state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_DNS_ABSENT &&
             !marker->was_active ?
        NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_FIREWALL_PENDING :
        NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_MANUAL_INTERVENTION;
    if (marker->state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_COMPLETE)
      return NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_RECOVERY_REQUIRED;
    return NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_MANUAL_INTERVENTION;
  }
  if ((observation->named_exists &&
       observation->named_ifindex != observation->token_ifindex) ||
      (marker->ifindex != 0 &&
       marker->ifindex != observation->token_ifindex))
    return NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_CONFLICT;
  if (!observation->token_has_expected_name)
    return NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_MANUAL_INTERVENTION;
  if (marker->state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_CLEANUP_COMPLETE ||
      marker->state == NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_FIREWALL_PENDING)
    return NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_CONFLICT;
  if (marker->state != NETWORK_SIDEBAR_AMNEZIAWG_JOURNAL_ACTIVE ||
      marker->ifindex == 0)
    return NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_RECOVERY_REQUIRED;
  return NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_ACTIVE;
}

gboolean
network_sidebar_amneziawg_runtime_state_is_recoverable(
  NetworkSidebarAmneziaWGRuntimeState state)
{
  return state == NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_RECOVERY_REQUIRED ||
         state == NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_FIREWALL_PENDING;
}
