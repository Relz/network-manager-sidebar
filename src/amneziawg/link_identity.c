#include "amneziawg/link_identity.h"

#include <errno.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LINK_DUMP_BUFFER_SIZE (64u * 1024u)
#define LINK_DUMP_TIMEOUT_MSEC 1000
#define LINK_DUMP_POLL_SLICE_MSEC 50
#define LINK_DUMP_MAX_RECORDS 65536u
#define LINK_GUARD_MAX_DRAIN_READS 256u

typedef struct {
  guint ifindex;
  char ifname[NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH + 1];
  char alias[sizeof(NETWORK_SIDEBAR_AMNEZIAWG_LINK_ALIAS_PREFIX) +
             NETWORK_SIDEBAR_AMNEZIAWG_LINK_TOKEN_LENGTH];
  gboolean has_alias;
} LinkRecord;

typedef struct {
  guint matches;
  guint first_index;
} AliasIndex;

struct _NetworkSidebarAmneziaWGLinkSnapshot {
  GArray *records;
  GHashTable *names;
  GHashTable *aliases;
};

struct _NetworkSidebarAmneziaWGLinkEventFilter {
  GHashTable *names;
  GHashTable *aliases;
  GHashTable *indices;
};

struct _NetworkSidebarAmneziaWGLinkGuard {
  LinkRecord identity;
  /* Closing the subscription permanently invalidates the guard. */
  int socket_fd;
};

static gboolean
operation_expired(gint64 deadline_us, GCancellable *cancellable)
{
  return g_get_monotonic_time() >= deadline_us ||
         (cancellable != NULL && g_cancellable_is_cancelled(cancellable));
}

static gboolean
attribute_string_is_valid(const struct rtattr *attribute)
{
  gsize payload_length = RTA_PAYLOAD(attribute);
  const char *value = RTA_DATA(attribute);

  return payload_length > 0 && value[payload_length - 1] == '\0' &&
         memchr(value, '\0', payload_length - 1) == NULL;
}

static gboolean
attribute_string_copy(const struct rtattr *attribute,
                      char *destination,
                      gsize destination_size)
{
  gsize payload_length = RTA_PAYLOAD(attribute);
  const char *value = RTA_DATA(attribute);

  if (payload_length > destination_size ||
      !attribute_string_is_valid(attribute))
    return FALSE;
  memcpy(destination, value, payload_length);
  return TRUE;
}

static gboolean
parse_link_message(const struct nlmsghdr *header,
                   LinkRecord *record,
                   gboolean require_name)
{
  const struct ifinfomsg *interface;
  const struct rtattr *attribute;
  int attribute_length;
  gboolean have_name = FALSE;
  gboolean have_alias = FALSE;

  if (record == NULL ||
      header->nlmsg_len < NLMSG_LENGTH(sizeof(*interface)))
    return FALSE;
  *record = (LinkRecord) { 0 };
  interface = NLMSG_DATA(header);
  if (interface->ifi_index <= 0)
    return FALSE;
  attribute_length = IFLA_PAYLOAD(header);
  for (attribute = IFLA_RTA(interface);
       RTA_OK(attribute, attribute_length);
       attribute = RTA_NEXT(attribute, attribute_length)) {
    guint type = attribute->rta_type & NLA_TYPE_MASK;

    if (type == IFLA_IFNAME) {
      if (have_name || attribute->rta_type != IFLA_IFNAME ||
          !attribute_string_copy(attribute,
                                  record->ifname,
                                  sizeof(record->ifname)) ||
          record->ifname[0] == '\0')
        return FALSE;
      have_name = TRUE;
    } else if (type == IFLA_IFALIAS) {
      if (have_alias || attribute->rta_type != IFLA_IFALIAS ||
          !attribute_string_is_valid(attribute))
        return FALSE;
      have_alias = TRUE;
      /* A valid, longer alias on an unrelated link cannot match our token. */
      record->has_alias = attribute_string_copy(attribute,
                                                record->alias,
                                                sizeof(record->alias));
    }
  }
  if (attribute_length != 0 || (require_name && !have_name))
    return FALSE;
  record->ifindex = (guint) interface->ifi_index;
  return TRUE;
}

static gboolean
wait_for_socket(int socket_fd,
                short events,
                gint64 deadline_us,
                GCancellable *cancellable)
{
  for (;;) {
    struct pollfd descriptor = { socket_fd, events, 0 };
    gint64 now;
    gint timeout;
    int result;

    if (operation_expired(deadline_us, cancellable))
      return FALSE;
    now = g_get_monotonic_time();
    timeout = (gint) MIN((deadline_us - now + 999) / 1000,
                         LINK_DUMP_POLL_SLICE_MSEC);
    result = poll(&descriptor, 1, MAX(timeout, 1));
    if (result < 0 && errno == EINTR)
      continue;
    if (result < 0)
      return FALSE;
    if (result == 0)
      continue;
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
      return FALSE;
    if ((descriptor.revents & events) != 0)
      return TRUE;
  }
}

static gboolean
send_dump_request(int socket_fd,
                  const struct sockaddr_nl *kernel,
                  const void *request,
                  gsize request_length,
                  gint64 deadline_us,
                  GCancellable *cancellable)
{
  for (;;) {
    ssize_t sent;

    if (operation_expired(deadline_us, cancellable))
      return FALSE;
    sent = sendto(socket_fd,
                  request,
                  request_length,
                  MSG_DONTWAIT,
                  (const struct sockaddr *) kernel,
                  sizeof(*kernel));

    if (sent == (ssize_t) request_length)
      return TRUE;
    if (sent < 0 && errno == EINTR)
      continue;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
        wait_for_socket(socket_fd, POLLOUT, deadline_us, cancellable))
      continue;
    return FALSE;
  }
}

static NetworkSidebarAmneziaWGLinkSnapshot *
link_snapshot_read_until(GCancellable *cancellable, gint64 deadline_us)
{
  struct {
    struct nlmsghdr header;
    struct ifinfomsg interface;
  } request = { 0 };
  struct sockaddr_nl local = { 0 };
  struct sockaddr_nl kernel = { 0 };
  g_autofree guint8 *buffer = NULL;
  NetworkSidebarAmneziaWGLinkSnapshot *snapshot = NULL;
  int socket_fd = -1;
  gboolean complete = FALSE;

  deadline_us = MIN(deadline_us, g_get_monotonic_time() +
                                  LINK_DUMP_TIMEOUT_MSEC * 1000);
  if (operation_expired(deadline_us, cancellable))
    return NULL;
  snapshot = g_new0(NetworkSidebarAmneziaWGLinkSnapshot, 1);
  snapshot->records = g_array_new(FALSE, FALSE, sizeof(LinkRecord));
  snapshot->names = g_hash_table_new_full(g_str_hash,
                                           g_str_equal,
                                           g_free,
                                           NULL);
  snapshot->aliases = g_hash_table_new_full(g_str_hash,
                                             g_str_equal,
                                             g_free,
                                             g_free);
  socket_fd = socket(AF_NETLINK,
                     SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
                     NETLINK_ROUTE);
  if (socket_fd < 0)
    goto fail;
  local.nl_family = AF_NETLINK;
  if (bind(socket_fd, (const struct sockaddr *) &local, sizeof(local)) != 0)
    goto fail;

  request.header.nlmsg_len = NLMSG_LENGTH(sizeof(request.interface));
  request.header.nlmsg_type = RTM_GETLINK;
  request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  request.header.nlmsg_seq = 1;
  request.interface.ifi_family = AF_UNSPEC;
  kernel.nl_family = AF_NETLINK;
  if (!send_dump_request(socket_fd,
                         &kernel,
                         &request,
                         request.header.nlmsg_len,
                         deadline_us,
                         cancellable))
    goto fail;

  buffer = g_malloc(LINK_DUMP_BUFFER_SIZE);
  while (!complete) {
    struct sockaddr_nl sender = { 0 };
    struct iovec vector = { buffer, LINK_DUMP_BUFFER_SIZE };
    struct msghdr message = {
      .msg_name = &sender,
      .msg_namelen = sizeof(sender),
      .msg_iov = &vector,
      .msg_iovlen = 1,
    };
    ssize_t received;
    int remaining;

    if (!wait_for_socket(socket_fd, POLLIN, deadline_us, cancellable))
      goto fail;
    for (;;) {
      if (operation_expired(deadline_us, cancellable))
        goto fail;
      received = recvmsg(socket_fd, &message, MSG_DONTWAIT);
      if (received < 0 && errno == EINTR)
        continue;
      break;
    }
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    if (received <= 0 || received > LINK_DUMP_BUFFER_SIZE ||
        (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
        message.msg_namelen != sizeof(sender) ||
        sender.nl_family != AF_NETLINK || sender.nl_pid != 0)
      goto fail;

    remaining = (int) received;
    for (struct nlmsghdr *header = (struct nlmsghdr *) buffer;
         NLMSG_OK(header, remaining);
         header = NLMSG_NEXT(header, remaining)) {
      if (operation_expired(deadline_us, cancellable) ||
          header->nlmsg_seq != request.header.nlmsg_seq ||
          (header->nlmsg_flags & NLM_F_DUMP_INTR) != 0 ||
          header->nlmsg_type == NLMSG_OVERRUN)
        goto fail;
      if (header->nlmsg_type == NLMSG_DONE) {
        gsize payload_length = NLMSG_PAYLOAD(header, 0);

        if (payload_length != 0) {
          int done_error;

          if (payload_length != sizeof(done_error))
            goto fail;
          memcpy(&done_error, NLMSG_DATA(header), sizeof(done_error));
          if (done_error != 0) {
            errno = done_error == G_MININT ? EIO : ABS(done_error);
            goto fail;
          }
        }
        remaining -= NLMSG_ALIGN(header->nlmsg_len);
        if (remaining != 0)
          goto fail;
        complete = TRUE;
        break;
      }
      if (header->nlmsg_type == NLMSG_ERROR) {
        const struct nlmsgerr *netlink_error;

        if (header->nlmsg_len < NLMSG_LENGTH(sizeof(*netlink_error)))
          goto fail;
        netlink_error = NLMSG_DATA(header);
        if (netlink_error->error != 0) {
          errno = netlink_error->error < 0 &&
                  netlink_error->error != G_MININT ?
            -netlink_error->error : EIO;
          goto fail;
        }
        continue;
      }
      if (header->nlmsg_type == NLMSG_NOOP)
        continue;
      if (header->nlmsg_type != RTM_NEWLINK)
        goto fail;
      {
        LinkRecord record;
        guint record_index;

        if (snapshot->records->len >= LINK_DUMP_MAX_RECORDS ||
            !parse_link_message(header, &record, TRUE))
          goto fail;
        record_index = snapshot->records->len;
        g_array_append_val(snapshot->records, record);
        g_hash_table_replace(snapshot->names,
                             g_strdup(record.ifname),
                             GUINT_TO_POINTER(record_index + 1));
        if (record.has_alias) {
          AliasIndex *alias_index = g_hash_table_lookup(snapshot->aliases,
                                                         record.alias);

          if (alias_index == NULL) {
            alias_index = g_new0(AliasIndex, 1);
            alias_index->matches = 1;
            alias_index->first_index = record_index;
            g_hash_table_insert(snapshot->aliases,
                                g_strdup(record.alias),
                                alias_index);
          } else if (alias_index->matches != G_MAXUINT) {
            alias_index->matches++;
          }
        }
      }
    }
    if (remaining != 0)
      goto fail;
  }

  if (operation_expired(deadline_us, cancellable))
    goto fail;
  close(socket_fd);
  return snapshot;

fail:
  if (socket_fd >= 0)
    close(socket_fd);
  network_sidebar_amneziawg_link_snapshot_free(snapshot);
  return NULL;
}

NetworkSidebarAmneziaWGLinkSnapshot *
network_sidebar_amneziawg_link_snapshot_read(GCancellable *cancellable)
{
  return link_snapshot_read_until(cancellable, G_MAXINT64);
}

void
network_sidebar_amneziawg_link_snapshot_free(
  NetworkSidebarAmneziaWGLinkSnapshot *snapshot)
{
  if (snapshot == NULL)
    return;
  g_clear_pointer(&snapshot->records, g_array_unref);
  g_clear_pointer(&snapshot->names, g_hash_table_unref);
  g_clear_pointer(&snapshot->aliases, g_hash_table_unref);
  g_free(snapshot);
}

gboolean
network_sidebar_amneziawg_link_snapshot_observe(
  const NetworkSidebarAmneziaWGLinkSnapshot *snapshot,
  const char *expected_name,
  const char *link_token,
  NetworkSidebarAmneziaWGLinkObservation *observation)
{
  char expected_alias[sizeof(NETWORK_SIDEBAR_AMNEZIAWG_LINK_ALIAS_PREFIX) +
                      NETWORK_SIDEBAR_AMNEZIAWG_LINK_TOKEN_LENGTH];
  gpointer encoded_index;

  if (snapshot == NULL ||
      !network_sidebar_amneziawg_name_is_valid(expected_name) ||
      observation == NULL ||
      (link_token != NULL &&
       !network_sidebar_amneziawg_link_token_is_valid(link_token)))
    return FALSE;
  *observation = (NetworkSidebarAmneziaWGLinkObservation) { 0 };
  encoded_index = g_hash_table_lookup(snapshot->names, expected_name);
  if (encoded_index != NULL) {
    guint record_index = GPOINTER_TO_UINT(encoded_index) - 1;
    const LinkRecord *record = &g_array_index(snapshot->records,
                                               LinkRecord,
                                               record_index);

    observation->named_exists = TRUE;
    observation->named_ifindex = record->ifindex;
  }
  if (link_token != NULL) {
    AliasIndex *alias_index;
    int written = g_snprintf(expected_alias,
                             sizeof(expected_alias),
                             NETWORK_SIDEBAR_AMNEZIAWG_LINK_ALIAS_PREFIX "%s",
                             link_token);

    if (written <= 0 || (gsize) written >= sizeof(expected_alias))
      return FALSE;
    alias_index = g_hash_table_lookup(snapshot->aliases, expected_alias);
    if (alias_index != NULL) {
      const LinkRecord *record = &g_array_index(snapshot->records,
                                                 LinkRecord,
                                                 alias_index->first_index);

      observation->token_matches = alias_index->matches;
      observation->token_ifindex = record->ifindex;
      observation->token_has_expected_name =
        strcmp(record->ifname, expected_name) == 0;
      g_strlcpy(observation->token_ifname,
                record->ifname,
                sizeof(observation->token_ifname));
    }
  }
  return TRUE;
}

NetworkSidebarAmneziaWGLinkEventFilter *
network_sidebar_amneziawg_link_event_filter_new(void)
{
  NetworkSidebarAmneziaWGLinkEventFilter *filter = g_new0(
    NetworkSidebarAmneziaWGLinkEventFilter, 1);

  filter->names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  filter->aliases = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  filter->indices = g_hash_table_new(g_direct_hash, g_direct_equal);
  return filter;
}

void
network_sidebar_amneziawg_link_event_filter_free(
  NetworkSidebarAmneziaWGLinkEventFilter *filter)
{
  if (filter == NULL)
    return;
  g_hash_table_unref(filter->names);
  g_hash_table_unref(filter->aliases);
  g_hash_table_unref(filter->indices);
  g_free(filter);
}

static void
track_link_index(NetworkSidebarAmneziaWGLinkEventFilter *filter,
                 const NetworkSidebarAmneziaWGLinkSnapshot *snapshot,
                 guint record_index)
{
  const LinkRecord *record = &g_array_index(snapshot->records,
                                             LinkRecord, record_index);

  g_hash_table_add(filter->indices, GUINT_TO_POINTER(record->ifindex));
}

void
network_sidebar_amneziawg_link_event_filter_add(
  NetworkSidebarAmneziaWGLinkEventFilter *filter,
  const NetworkSidebarAmneziaWGLinkSnapshot *snapshot,
  const NetworkSidebarAmneziaWGLinkIdentity *identity)
{
  g_autofree char *alias = NULL;
  gpointer named_index;
  const AliasIndex *alias_index;

  g_return_if_fail(filter != NULL && identity != NULL);
  g_return_if_fail(network_sidebar_amneziawg_name_is_valid(identity->name));
  g_return_if_fail(identity->link_token == NULL ||
                  network_sidebar_amneziawg_link_token_is_valid(identity->link_token));

  g_hash_table_add(filter->names, g_strdup(identity->name));
  if (identity->ifindex != 0)
    g_hash_table_add(filter->indices, GUINT_TO_POINTER(identity->ifindex));
  if (identity->link_token != NULL) {
    alias = g_strconcat(NETWORK_SIDEBAR_AMNEZIAWG_LINK_ALIAS_PREFIX,
                        identity->link_token, NULL);
    g_hash_table_add(filter->aliases, g_strdup(alias));
  }
  if (snapshot == NULL)
    return;
  named_index = g_hash_table_lookup(snapshot->names, identity->name);
  if (named_index != NULL)
    track_link_index(filter, snapshot, GPOINTER_TO_UINT(named_index) - 1);
  alias_index = alias != NULL ? g_hash_table_lookup(snapshot->aliases, alias) : NULL;
  if (alias_index == NULL)
    return;
  if (alias_index->matches == 1) {
    track_link_index(filter, snapshot, alias_index->first_index);
    return;
  }
  for (guint i = 0; i < snapshot->records->len; i++) {
    const LinkRecord *record = &g_array_index(snapshot->records, LinkRecord, i);

    if (record->has_alias && strcmp(record->alias, alias) == 0)
      track_link_index(filter, snapshot, i);
  }
}

gboolean
network_sidebar_amneziawg_link_event_filter_is_relevant(
  const NetworkSidebarAmneziaWGLinkEventFilter *filter,
  const void *data,
  gsize length)
{
  int remaining;

  if (filter == NULL || data == NULL || length == 0 || length > G_MAXINT)
    return TRUE;
  remaining = (int) length;
  for (const struct nlmsghdr *header = data;
       NLMSG_OK(header, remaining);
       header = NLMSG_NEXT(header, remaining)) {
    LinkRecord record;

    if ((header->nlmsg_flags & NLM_F_DUMP_INTR) != 0)
      return TRUE;
    if (header->nlmsg_type == NLMSG_NOOP)
      continue;
    if ((header->nlmsg_type != RTM_NEWLINK &&
         header->nlmsg_type != RTM_DELLINK) ||
        !parse_link_message(header, &record, FALSE))
      return TRUE;
    if (g_hash_table_contains(filter->indices, GUINT_TO_POINTER(record.ifindex)) ||
        g_hash_table_contains(filter->names, record.ifname) ||
        (record.has_alias && g_hash_table_contains(filter->aliases, record.alias)))
      return TRUE;
  }
  return remaining != 0;
}

gboolean
network_sidebar_amneziawg_link_observe(
  const char *expected_name,
  const char *link_token,
  NetworkSidebarAmneziaWGLinkObservation *observation)
{
  g_autoptr(NetworkSidebarAmneziaWGLinkSnapshot) snapshot = NULL;

  if (!network_sidebar_amneziawg_name_is_valid(expected_name) ||
      observation == NULL ||
      (link_token != NULL &&
       !network_sidebar_amneziawg_link_token_is_valid(link_token)))
    return FALSE;
  *observation = (NetworkSidebarAmneziaWGLinkObservation) { 0 };
  snapshot = network_sidebar_amneziawg_link_snapshot_read(NULL);
  return snapshot != NULL &&
         network_sidebar_amneziawg_link_snapshot_observe(snapshot,
                                                          expected_name,
                                                          link_token,
                                                          observation);
}

static gboolean
link_record_is_relevant(const LinkRecord *record, const LinkRecord *identity)
{
  return record->ifindex == identity->ifindex ||
         strcmp(record->ifname, identity->ifname) == 0 ||
         (record->has_alias && strcmp(record->alias, identity->alias) == 0);
}

static gboolean
link_record_matches(const LinkRecord *record, const LinkRecord *identity)
{
  return record->ifindex == identity->ifindex &&
         strcmp(record->ifname, identity->ifname) == 0 &&
         record->has_alias && strcmp(record->alias, identity->alias) == 0;
}

static gboolean
link_guard_drain(NetworkSidebarAmneziaWGLinkGuard *guard,
                  gint64 deadline_us,
                  GCancellable *cancellable)
{
  g_autofree guint8 *buffer = g_malloc(LINK_DUMP_BUFFER_SIZE);

  /* Require EAGAIN, not just a matching event. Bound even a continuously busy
   * channel (or repeated EINTR), and never suppress ENOBUFS notifications. */
  for (guint reads = 0; reads < LINK_GUARD_MAX_DRAIN_READS; reads++) {
    struct sockaddr_nl sender = { 0 };
    struct iovec vector = { buffer, LINK_DUMP_BUFFER_SIZE };
    struct msghdr message = {
      .msg_name = &sender,
      .msg_namelen = sizeof(sender),
      .msg_iov = &vector,
      .msg_iovlen = 1,
    };
    ssize_t received;
    int remaining;

    if (operation_expired(deadline_us, cancellable))
      return FALSE;
    received = recvmsg(guard->socket_fd, &message, MSG_DONTWAIT);
    if (received < 0 && errno == EINTR)
      continue;
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return !operation_expired(deadline_us, cancellable);
    if (received <= 0 || received > LINK_DUMP_BUFFER_SIZE ||
        (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
        message.msg_namelen != sizeof(sender) ||
        sender.nl_family != AF_NETLINK || sender.nl_pid != 0)
      return FALSE;

    remaining = (int) received;
    for (struct nlmsghdr *header = (struct nlmsghdr *) buffer;
         NLMSG_OK(header, remaining);
         header = NLMSG_NEXT(header, remaining)) {
      LinkRecord record;

      if (operation_expired(deadline_us, cancellable) ||
          (header->nlmsg_flags & NLM_F_DUMP_INTR) != 0)
        return FALSE;
      if (header->nlmsg_type == NLMSG_NOOP)
        continue;
      /* This socket never sends requests, so other control messages (including
       * NLMSG_OVERRUN/ERROR/DONE) cannot establish trustworthy continuity. */
      if ((header->nlmsg_type != RTM_NEWLINK &&
           header->nlmsg_type != RTM_DELLINK) ||
          !parse_link_message(header, &record, TRUE))
        return FALSE;
      if (link_record_is_relevant(&record, &guard->identity) &&
          (header->nlmsg_type == RTM_DELLINK ||
           !link_record_matches(&record, &guard->identity)))
        return FALSE;
    }
    if (remaining != 0)
      return FALSE;
  }
  return FALSE;
}

NetworkSidebarAmneziaWGLinkGuard *
network_sidebar_amneziawg_link_guard_new(
  const NetworkSidebarAmneziaWGLinkIdentity *identity,
  gint64 deadline_msec,
  GCancellable *cancellable)
{
  g_autoptr(NetworkSidebarAmneziaWGLinkGuard) guard = NULL;
  struct sockaddr_nl local = {
    .nl_family = AF_NETLINK,
    .nl_groups = RTMGRP_LINK,
  };

  if (identity == NULL ||
      !network_sidebar_amneziawg_name_is_valid(identity->name) ||
      !network_sidebar_amneziawg_link_token_is_valid(identity->link_token) ||
      identity->ifindex == 0 || identity->ifindex > G_MAXINT ||
      (cancellable != NULL && g_cancellable_is_cancelled(cancellable)) ||
      deadline_msec <= g_get_monotonic_time() / 1000)
    return NULL;

  guard = g_new0(NetworkSidebarAmneziaWGLinkGuard, 1);
  guard->identity.ifindex = identity->ifindex;
  g_strlcpy(guard->identity.ifname,
             identity->name,
             sizeof(guard->identity.ifname));
  g_snprintf(guard->identity.alias,
              sizeof(guard->identity.alias),
              NETWORK_SIDEBAR_AMNEZIAWG_LINK_ALIAS_PREFIX "%s",
              identity->link_token);
  guard->identity.has_alias = TRUE;
  guard->socket_fd = socket(AF_NETLINK,
                            SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
                            NETLINK_ROUTE);
  if (guard->socket_fd < 0 ||
      bind(guard->socket_fd, (const struct sockaddr *) &local, sizeof(local)) != 0 ||
      !network_sidebar_amneziawg_link_guard_check(guard, deadline_msec,
                                                   cancellable))
    return NULL;
  return g_steal_pointer(&guard);
}

gboolean
network_sidebar_amneziawg_link_guard_check(
  NetworkSidebarAmneziaWGLinkGuard *guard,
  gint64 deadline_msec,
  GCancellable *cancellable)
{
  g_autoptr(NetworkSidebarAmneziaWGLinkSnapshot) snapshot = NULL;
  gint64 deadline_us;
  guint matches = 0;

  if (guard == NULL || guard->socket_fd < 0)
    return FALSE;
  /* Saturate before converting caller-controlled milliseconds to microseconds.
   * The same local budget covers both drains and the entire fresh snapshot. */
  deadline_us = deadline_msec <= 0 ? 0 :
    deadline_msec > G_MAXINT64 / 1000 ? G_MAXINT64 : deadline_msec * 1000;
  deadline_us = MIN(deadline_us, g_get_monotonic_time() +
                                  LINK_DUMP_TIMEOUT_MSEC * 1000);
  if (!link_guard_drain(guard, deadline_us, cancellable))
    goto invalid;
  snapshot = link_snapshot_read_until(cancellable, deadline_us);
  if (snapshot == NULL)
    goto invalid;
  for (guint i = 0; i < snapshot->records->len; i++) {
    const LinkRecord *record = &g_array_index(snapshot->records, LinkRecord, i);

    if (operation_expired(deadline_us, cancellable))
      goto invalid;
    if (link_record_is_relevant(record, &guard->identity) &&
        (!link_record_matches(record, &guard->identity) || ++matches != 1))
      goto invalid;
  }
  if (matches != 1 || !link_guard_drain(guard, deadline_us, cancellable))
    goto invalid;
  return TRUE;

invalid:
  close(guard->socket_fd);
  guard->socket_fd = -1;
  return FALSE;
}

void
network_sidebar_amneziawg_link_guard_free(NetworkSidebarAmneziaWGLinkGuard *guard)
{
  if (guard == NULL)
    return;
  if (guard->socket_fd >= 0)
    close(guard->socket_fd);
  g_free(guard);
}
