#include "amneziawg/dns_config.h"

#include "amneziawg/amneziawg.h"

#include <arpa/inet.h>
#include <string.h>

#define DNS_PAYLOAD_HEADER_SIZE 16u
#define DNS_PAYLOAD_MIN_RECORD_SIZE 5u
#define DNS_PAYLOAD_MAX_RECORDS \
  ((NETWORK_SIDEBAR_AMNEZIAWG_MAX_DNS_PAYLOAD - DNS_PAYLOAD_HEADER_SIZE) / \
   DNS_PAYLOAD_MIN_RECORD_SIZE)

void
network_sidebar_amneziawg_dns_config_init(NetworkSidebarAmneziaWGDnsConfig *dns)
{
  *dns = (NetworkSidebarAmneziaWGDnsConfig) {
    g_array_new(FALSE, FALSE, sizeof(NetworkSidebarAmneziaWGDnsServer)),
    g_ptr_array_new(),
  };
}

void
network_sidebar_amneziawg_dns_config_clear(NetworkSidebarAmneziaWGDnsConfig *dns)
{
  if (dns == NULL)
    return;
  if (dns->servers != NULL) {
    network_sidebar_amneziawg_secret_wipe(
      dns->servers->data, dns->servers->len * sizeof(NetworkSidebarAmneziaWGDnsServer));
    g_array_free(dns->servers, TRUE);
  }
  if (dns->domains != NULL) {
    for (guint i = 0; i < dns->domains->len; i++) {
      char *domain = g_ptr_array_index(dns->domains, i);

      network_sidebar_amneziawg_secret_free((guint8 *) domain, strlen(domain));
    }
    g_ptr_array_free(dns->domains, TRUE);
  }
  *dns = (NetworkSidebarAmneziaWGDnsConfig) { 0 };
}

static void
trim_span(const guint8 **start, const guint8 **end)
{
  while (*start < *end && g_ascii_isspace(**start))
    (*start)++;
  while (*end > *start && g_ascii_isspace((*end)[-1]))
    (*end)--;
}

static gboolean
value_is_domain(const guint8 *start, gsize length)
{
  gsize label_length = 0;

  if (length == 0 || length > 253)
    return FALSE;
  if (start[length - 1] == '.') {
    length--;
    if (length == 0)
      return FALSE;
  }

  for (gsize i = 0; i < length; i++) {
    guint8 byte = start[i];

    if (byte == '.') {
      if (label_length == 0 || start[i - 1] == '-')
        return FALSE;
      label_length = 0;
      continue;
    }
    if (!g_ascii_isalnum(byte) && byte != '-')
      return FALSE;
    if (label_length == 0 && byte == '-')
      return FALSE;
    if (++label_length > 63)
      return FALSE;
  }
  return label_length > 0 && start[length - 1] != '-';
}

NetworkSidebarAmneziaWGDnsConfigMeasureResult
network_sidebar_amneziawg_dns_config_measure(
  const NetworkSidebarAmneziaWGDnsConfig *dns,
  gsize *size)
{
  gsize measured = DNS_PAYLOAD_HEADER_SIZE;
  guint64 record_count;

  if (dns == NULL || dns->servers == NULL || dns->domains == NULL ||
      size == NULL)
    return NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_MEASURE_INVALID;
  record_count = (guint64) dns->servers->len + dns->domains->len;
  if (record_count > DNS_PAYLOAD_MAX_RECORDS)
    return NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_MEASURE_TOO_LARGE;
  if (dns->domains->len > 0 && dns->servers->len == 0)
    return NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_MEASURE_INVALID;

  for (guint i = 0; i < dns->servers->len; i++) {
    const NetworkSidebarAmneziaWGDnsServer *server = &g_array_index(
      dns->servers, NetworkSidebarAmneziaWGDnsServer, i);
    gsize encoded_length;

    if ((server->family != AF_INET || server->length != 4) &&
        (server->family != AF_INET6 || server->length != 16))
      return NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_MEASURE_INVALID;
    encoded_length = 1u + server->length;
    if (measured > NETWORK_SIDEBAR_AMNEZIAWG_MAX_DNS_PAYLOAD -
                     encoded_length)
      return NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_MEASURE_TOO_LARGE;
    measured += encoded_length;
  }
  for (guint i = 0; i < dns->domains->len; i++) {
    const char *domain = g_ptr_array_index(dns->domains, i);
    gsize domain_length;
    gsize encoded_length;

    if (domain == NULL)
      return NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_MEASURE_INVALID;
    domain_length = strlen(domain);
    if (domain_length == 0 || domain_length > 253 ||
        !value_is_domain((const guint8 *) domain, domain_length))
      return NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_MEASURE_INVALID;
    encoded_length = 4u + domain_length;
    if (measured > NETWORK_SIDEBAR_AMNEZIAWG_MAX_DNS_PAYLOAD -
                     encoded_length)
      return NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_MEASURE_TOO_LARGE;
    measured += encoded_length;
  }

  *size = measured;
  return NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_MEASURE_OK;
}

static gboolean
value_looks_like_ipv4(const guint8 *start, gsize length)
{
  if (length == 0)
    return FALSE;
  for (gsize i = 0; i < length; i++) {
    if (!g_ascii_isdigit(start[i]) && start[i] != '.')
      return FALSE;
  }
  return TRUE;
}

static gboolean
value_is_dns_list(const guint8 *start, gsize length)
{
  const guint8 *end = start + length;

  while (start < end) {
    const guint8 *comma = memchr(start, ',', (gsize) (end - start));
    const guint8 *item_end = comma != NULL ? comma : end;
    const guint8 *item_start = start;
    g_autofree char *item = NULL;
    guint8 parsed[sizeof(struct in6_addr)];

    trim_span(&item_start, &item_end);
    if (item_start == item_end)
      return FALSE;
    item = g_strndup((const char *) item_start, (gsize) (item_end - item_start));
    if (inet_pton(strchr(item, ':') != NULL ? AF_INET6 : AF_INET, item, parsed) != 1) {
      gsize item_length = (gsize) (item_end - item_start);

      if (value_looks_like_ipv4(item_start, item_length) ||
          !value_is_domain(item_start, item_length)) {
        network_sidebar_amneziawg_secret_wipe(item, strlen(item));
        return FALSE;
      }
    }
    network_sidebar_amneziawg_secret_wipe(item, strlen(item));
    if (comma == NULL)
      return TRUE;
    start = comma + 1;
  }
  return FALSE;
}

gboolean
network_sidebar_amneziawg_dns_config_add_value(
  NetworkSidebarAmneziaWGDnsConfig *dns,
  const guint8 *start,
  gsize length)
{
  const guint8 *end;

  if (dns == NULL || dns->servers == NULL || dns->domains == NULL ||
      start == NULL || !value_is_dns_list(start, length))
    return FALSE;
  end = start + length;
  while (start < end) {
    const guint8 *comma = memchr(start, ',', (gsize) (end - start));
    const guint8 *item_end = comma != NULL ? comma : end;
    const guint8 *item_start = start;
    NetworkSidebarAmneziaWGDnsServer server = { 0 };
    g_autofree char *item = NULL;

    trim_span(&item_start, &item_end);
    item = g_strndup((const char *) item_start, (gsize) (item_end - item_start));
    server.family = strchr(item, ':') != NULL ? AF_INET6 : AF_INET;
    server.length = server.family == AF_INET ? sizeof(struct in_addr) : sizeof(struct in6_addr);
    if (inet_pton(server.family, item, server.address) == 1) {
      g_array_append_val(dns->servers, server);
      network_sidebar_amneziawg_secret_wipe(item, strlen(item));
    } else {
      g_ptr_array_add(dns->domains, g_steal_pointer(&item));
    }
    if (comma == NULL)
      return TRUE;
    start = comma + 1;
  }
  return FALSE;
}

static void
write_be32(guint8 *data, guint32 value)
{
  data[0] = (guint8) (value >> 24);
  data[1] = (guint8) (value >> 16);
  data[2] = (guint8) (value >> 8);
  data[3] = (guint8) value;
}

static guint32
read_be32(const guint8 *data)
{
  return ((guint32) data[0] << 24) | ((guint32) data[1] << 16) |
         ((guint32) data[2] << 8) | data[3];
}

gboolean
network_sidebar_amneziawg_dns_config_serialize(
  const NetworkSidebarAmneziaWGDnsConfig *dns,
  guint8 **data,
  gsize *length)
{
  static const guint8 magic[8] = { 'N', 'M', 'S', 'D', 'N', 'S', '1', 0 };
  gsize size;
  gsize offset = DNS_PAYLOAD_HEADER_SIZE;
  guint8 *result;

  if (data == NULL || length == NULL ||
      network_sidebar_amneziawg_dns_config_measure(dns, &size) !=
        NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_MEASURE_OK)
    return FALSE;
  result = g_malloc0(size);
  memcpy(result, magic, sizeof(magic));
  write_be32(result + 8, dns->servers->len);
  write_be32(result + 12, dns->domains->len);
  for (guint i = 0; i < dns->servers->len; i++) {
    const NetworkSidebarAmneziaWGDnsServer *server = &g_array_index(
      dns->servers, NetworkSidebarAmneziaWGDnsServer, i);

    result[offset++] = server->family == AF_INET ? 4 : 6;
    memcpy(result + offset, server->address, server->length);
    offset += server->length;
  }
  for (guint i = 0; i < dns->domains->len; i++) {
    const char *domain = g_ptr_array_index(dns->domains, i);
    guint32 domain_length = (guint32) strlen(domain);

    write_be32(result + offset, domain_length);
    offset += 4;
    memcpy(result + offset, domain, domain_length);
    offset += domain_length;
  }
  *data = result;
  *length = size;
  return TRUE;
}

gboolean
network_sidebar_amneziawg_dns_config_parse(
  const guint8 *data,
  gsize length,
  NetworkSidebarAmneziaWGDnsConfig *dns)
{
  static const guint8 magic[8] = { 'N', 'M', 'S', 'D', 'N', 'S', '1', 0 };
  NetworkSidebarAmneziaWGDnsConfig parsed = { 0 };
  guint32 server_count;
  guint32 domain_count;
  gsize offset = DNS_PAYLOAD_HEADER_SIZE;

  if (dns == NULL)
    return FALSE;
  *dns = (NetworkSidebarAmneziaWGDnsConfig) { 0 };
  network_sidebar_amneziawg_dns_config_init(&parsed);
  if (data == NULL || length < DNS_PAYLOAD_HEADER_SIZE ||
      length > NETWORK_SIDEBAR_AMNEZIAWG_MAX_DNS_PAYLOAD ||
      memcmp(data, magic, sizeof(magic)) != 0)
    goto invalid;
  server_count = read_be32(data + 8);
  domain_count = read_be32(data + 12);
  if (server_count > length || domain_count > length ||
      (domain_count > 0 && server_count == 0))
    goto invalid;
  for (guint32 i = 0; i < server_count; i++) {
    NetworkSidebarAmneziaWGDnsServer server = { 0 };

    if (offset >= length)
      goto invalid;
    server.family = data[offset] == 4 ? AF_INET : data[offset] == 6 ? AF_INET6 : 0;
    server.length = server.family == AF_INET ? 4 : server.family == AF_INET6 ? 16 : 0;
    offset++;
    if (server.length == 0 || server.length > length - offset)
      goto invalid;
    memcpy(server.address, data + offset, server.length);
    offset += server.length;
    g_array_append_val(parsed.servers, server);
  }
  for (guint32 i = 0; i < domain_count; i++) {
    guint32 domain_length;
    char *domain;

    if (length - offset < 4)
      goto invalid;
    domain_length = read_be32(data + offset);
    offset += 4;
    if (domain_length == 0 || domain_length > 253 || domain_length > length - offset ||
        !value_is_domain(data + offset, domain_length))
      goto invalid;
    domain = g_strndup((const char *) data + offset, domain_length);
    g_ptr_array_add(parsed.domains, domain);
    offset += domain_length;
  }
  if (offset != length)
    goto invalid;
  *dns = parsed;
  return TRUE;

invalid:
  network_sidebar_amneziawg_dns_config_clear(&parsed);
  return FALSE;
}
