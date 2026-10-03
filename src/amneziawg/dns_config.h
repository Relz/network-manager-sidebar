#ifndef NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_H
#define NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

#define NETWORK_SIDEBAR_AMNEZIAWG_MAX_DNS_PAYLOAD (256u * 1024u)

typedef struct {
  gint family;
  guint8 length;
  guint8 address[16];
} NetworkSidebarAmneziaWGDnsServer;

typedef struct {
  GArray *servers;
  GPtrArray *domains;
} NetworkSidebarAmneziaWGDnsConfig;

typedef enum {
  NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_MEASURE_OK,
  NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_MEASURE_INVALID,
  NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_MEASURE_TOO_LARGE,
} NetworkSidebarAmneziaWGDnsConfigMeasureResult;

void network_sidebar_amneziawg_dns_config_init(NetworkSidebarAmneziaWGDnsConfig *dns);
void network_sidebar_amneziawg_dns_config_clear(NetworkSidebarAmneziaWGDnsConfig *dns);

/* Appends a validated comma-separated list to an initialized configuration.
 * Domain-only lines are allowed; measure() validates the complete configuration. */
gboolean network_sidebar_amneziawg_dns_config_add_value(
  NetworkSidebarAmneziaWGDnsConfig *dns,
  const guint8 *data,
  gsize length);
NetworkSidebarAmneziaWGDnsConfigMeasureResult
network_sidebar_amneziawg_dns_config_measure(
  const NetworkSidebarAmneziaWGDnsConfig *dns,
  gsize *size);

gboolean network_sidebar_amneziawg_dns_config_serialize(
  const NetworkSidebarAmneziaWGDnsConfig *dns,
  guint8 **data,
  gsize *length);
gboolean network_sidebar_amneziawg_dns_config_parse(
  const guint8 *data,
  gsize length,
  NetworkSidebarAmneziaWGDnsConfig *dns);

G_END_DECLS

#endif
