#ifndef NETWORK_SIDEBAR_AMNEZIAWG_CONFIG_H
#define NETWORK_SIDEBAR_AMNEZIAWG_CONFIG_H

#include "amneziawg/amneziawg.h"
#include "amneziawg/dns_config.h"

G_BEGIN_DECLS

typedef enum {
  NETWORK_SIDEBAR_AMNEZIAWG_ERROR_INVALID_DATA,
  NETWORK_SIDEBAR_AMNEZIAWG_ERROR_TOO_LARGE,
  NETWORK_SIDEBAR_AMNEZIAWG_ERROR_UNSAFE_DIRECTIVE,
  NETWORK_SIDEBAR_AMNEZIAWG_ERROR_MISSING_STRUCTURE,
} NetworkSidebarAmneziaWGError;

typedef struct {
  guint8 *quick_config;
  gsize quick_config_length;
  NetworkSidebarAmneziaWGDnsConfig dns;
  /* Offset just after the physical [Interface] header line in the input. */
  gsize interface_line_end;
} NetworkSidebarAmneziaWGConfigEnvelope;

#define NETWORK_SIDEBAR_AMNEZIAWG_ERROR (network_sidebar_amneziawg_error_quark())

GQuark network_sidebar_amneziawg_error_quark(void);

/* Pure safety/envelope validation, without I/O or values in diagnostics.
 * Protocol directive names and values are checked by awg during activation,
 * not here or by awg-quick strip. */
gboolean network_sidebar_amneziawg_config_validate(const guint8 *data,
                                                  gsize length,
                                                  GError **error);

/* Rejects command hooks and extracts DNS plus the safe awg-quick directives
 * needed for teardown. Protocol validation is deferred to awg at activation. */
gboolean network_sidebar_amneziawg_config_parse_envelope(
  const guint8 *data,
  gsize length,
  NetworkSidebarAmneziaWGConfigEnvelope *envelope,
  GError **error);
void network_sidebar_amneziawg_config_envelope_clear(
  NetworkSidebarAmneziaWGConfigEnvelope *envelope);

/* Combines stripped protocol data with known-safe quick directives. Validates
 * the output envelope, not protocol syntax or compatibility. size_reserve
 * leaves room within the configuration size limit for caller-owned additions. */
gboolean network_sidebar_amneziawg_runtime_snapshot_build(
  const guint8 *protocol_config,
  gsize protocol_config_length,
  const NetworkSidebarAmneziaWGConfigEnvelope *envelope,
  gsize size_reserve,
  guint8 **snapshot,
  gsize *snapshot_length,
  GError **error);

G_END_DECLS

#endif
