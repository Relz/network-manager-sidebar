#include "amneziawg/config.h"

#include <string.h>

typedef enum {
  CONFIG_SECTION_NONE,
  CONFIG_SECTION_INTERFACE,
  CONFIG_SECTION_OTHER,
} ConfigSection;

GQuark
network_sidebar_amneziawg_error_quark(void)
{
  return g_quark_from_static_string("nm-sidebar-amneziawg-error");
}

static gboolean
span_equal(const guint8 *start, gsize length, const char *text)
{
  gsize text_length = strlen(text);

  return length == text_length && g_ascii_strncasecmp((const char *) start, text, length) == 0;
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
set_parse_error(GError **error,
                NetworkSidebarAmneziaWGError code,
                guint line,
                const char *message)
{
  g_set_error(error,
              NETWORK_SIDEBAR_AMNEZIAWG_ERROR,
              code,
              "configuration line %u: %s",
              line,
              message);
  return FALSE;
}

static gboolean
is_unsafe_directive(const guint8 *key, gsize key_length)
{
  static const char *const keys[] = { "PreUp", "PostUp", "PreDown", "PostDown" };

  for (gsize i = 0; i < G_N_ELEMENTS(keys); i++) {
    if (span_equal(key, key_length, keys[i]))
      return TRUE;
  }
  return FALSE;
}

static const char *
safe_quick_directive(const guint8 *key, gsize key_length)
{
  static const char *const keys[] = { "Address", "MTU", "Table", "SaveConfig" };

  for (gsize i = 0; i < G_N_ELEMENTS(keys); i++) {
    if (span_equal(key, key_length, keys[i]))
      return keys[i];
  }
  return NULL;
}

static gboolean
token_is_safe(const guint8 *start, gsize length)
{
  if (length == 0)
    return FALSE;
  for (gsize i = 0; i < length; i++) {
    if (g_ascii_isspace(start[i]) || start[i] == '[' || start[i] == ']' ||
        start[i] == '=' || start[i] == '#')
      return FALSE;
  }
  return TRUE;
}

static gboolean
scan_config_envelope(const guint8 *data,
                     gsize length,
                     GByteArray *quick_config,
                     NetworkSidebarAmneziaWGDnsConfig *dns,
                     gsize *interface_line_end,
                     GError **error)
{
  ConfigSection section = CONFIG_SECTION_NONE;
  gboolean interface_seen = FALSE;
  gsize offset = 0;
  guint line_number = 0;

  if (length > NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE) {
    g_set_error_literal(error,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR_TOO_LARGE,
                        "configuration exceeds the size limit");
    return FALSE;
  }
  if (data == NULL || length == 0) {
    g_set_error_literal(error,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR_INVALID_DATA,
                        "configuration is empty");
    return FALSE;
  }
  if (memchr(data, '\0', length) != NULL) {
    g_set_error_literal(error,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR_INVALID_DATA,
                        "configuration contains binary data");
    return FALSE;
  }

  while (offset < length) {
    const guint8 *physical_start = data + offset;
    const guint8 *line_start = physical_start;
    const guint8 *newline = memchr(line_start, '\n', length - offset);
    const guint8 *line_end = newline != NULL ? newline : data + length;
    const guint8 *physical_end = newline != NULL ? newline + 1 : data + length;
    const guint8 *comment;
    const guint8 *equals;
    const guint8 *key_start;
    const guint8 *key_end;
    const guint8 *value_start;
    const guint8 *value_end;

    line_number++;
    offset = (gsize) (physical_end - data);
    comment = memchr(line_start, '#', (gsize) (line_end - line_start));
    if (comment != NULL)
      line_end = comment;
    for (const guint8 *p = line_start; p < line_end; p++) {
      if (*p < 0x20 && *p != '\t' && *p != '\r')
        return set_parse_error(error, NETWORK_SIDEBAR_AMNEZIAWG_ERROR_INVALID_DATA, line_number, "contains unsupported characters");
    }

    trim_span(&line_start, &line_end);
    if (line_start == line_end)
      continue;

    if (*line_start == '[') {
      const guint8 *section_start = line_start + 1;
      const guint8 *section_end = line_end - 1;

      if (line_end[-1] != ']' || !token_is_safe(section_start,
                                                 (gsize) (section_end - section_start)))
        return set_parse_error(error, NETWORK_SIDEBAR_AMNEZIAWG_ERROR_INVALID_DATA, line_number, "has a malformed section");
      if (span_equal(section_start,
                     (gsize) (section_end - section_start),
                     "Interface")) {
        if (interface_seen || section != CONFIG_SECTION_NONE)
          return set_parse_error(error, NETWORK_SIDEBAR_AMNEZIAWG_ERROR_INVALID_DATA, line_number, "has a duplicate or misplaced interface section");
        interface_seen = TRUE;
        section = CONFIG_SECTION_INTERFACE;
        if (interface_line_end != NULL)
          *interface_line_end = (gsize) (physical_end - data);
      } else {
        if (!interface_seen)
          return set_parse_error(error, NETWORK_SIDEBAR_AMNEZIAWG_ERROR_INVALID_DATA, line_number, "has a section before the interface section");
        section = CONFIG_SECTION_OTHER;
      }
      continue;
    }

    equals = memchr(line_start, '=', (gsize) (line_end - line_start));
    if (equals == NULL || section == CONFIG_SECTION_NONE)
      return set_parse_error(error, NETWORK_SIDEBAR_AMNEZIAWG_ERROR_INVALID_DATA, line_number, "has a malformed directive");
    key_start = line_start;
    key_end = equals;
    value_start = equals + 1;
    value_end = line_end;
    trim_span(&key_start, &key_end);
    trim_span(&value_start, &value_end);
    if (!token_is_safe(key_start, (gsize) (key_end - key_start)))
      return set_parse_error(error, NETWORK_SIDEBAR_AMNEZIAWG_ERROR_INVALID_DATA, line_number, "has an invalid directive name");
    if (is_unsafe_directive(key_start, (gsize) (key_end - key_start)))
      return set_parse_error(error, NETWORK_SIDEBAR_AMNEZIAWG_ERROR_UNSAFE_DIRECTIVE, line_number, "uses a prohibited command directive");

    if (section == CONFIG_SECTION_INTERFACE) {
      gsize key_length = (gsize) (key_end - key_start);
      gsize value_length = (gsize) (value_end - value_start);
      const char *quick_name = safe_quick_directive(key_start, key_length);

      if (span_equal(key_start, key_length, "DNS")) {
        if (!network_sidebar_amneziawg_dns_config_add_value(dns, value_start,
                                                             value_length))
          return set_parse_error(error, NETWORK_SIDEBAR_AMNEZIAWG_ERROR_INVALID_DATA, line_number, "has an invalid DNS directive value");
      } else if (quick_name != NULL) {
        static const guint8 separator[] = " = ";
        gsize quick_name_length = strlen(quick_name);

        if (value_length > NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE -
                             quick_name_length - sizeof(separator) ||
            quick_config->len > NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE -
                                  quick_name_length - sizeof(separator) -
                                  value_length)
          return set_parse_error(error,
                                 NETWORK_SIDEBAR_AMNEZIAWG_ERROR_TOO_LARGE,
                                 line_number,
                                 "has too many quick-only directives");

        g_byte_array_append(quick_config,
                            (const guint8 *) quick_name,
                            quick_name_length);
        g_byte_array_append(quick_config, separator, sizeof(separator) - 1);
        g_byte_array_append(quick_config, value_start, value_length);
        g_byte_array_append(quick_config, (const guint8 *) "\n", 1);
      }
    }
  }

  if (!interface_seen) {
    g_set_error_literal(error,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR_MISSING_STRUCTURE,
                        "configuration has no interface section");
    return FALSE;
  }
  if (dns->domains->len > 0 && dns->servers->len == 0) {
    g_set_error_literal(error,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR_INVALID_DATA,
                        "configuration has DNS search domains without a numeric DNS server");
    return FALSE;
  }
  return TRUE;
}

gboolean
network_sidebar_amneziawg_config_parse_envelope(
  const guint8 *data,
  gsize length,
  NetworkSidebarAmneziaWGConfigEnvelope *envelope,
  GError **error)
{
  g_autoptr(GByteArray) quick_config = NULL;
  NetworkSidebarAmneziaWGConfigEnvelope parsed = { 0 };
  NetworkSidebarAmneziaWGDnsConfigMeasureResult dns_measure;
  gsize dns_payload_size;

  g_return_val_if_fail(envelope != NULL, FALSE);
  *envelope = (NetworkSidebarAmneziaWGConfigEnvelope) { 0 };
  quick_config = g_byte_array_sized_new(
    NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE);
  network_sidebar_amneziawg_dns_config_init(&parsed.dns);
  if (!scan_config_envelope(data,
                            length,
                            quick_config,
                            &parsed.dns,
                            &parsed.interface_line_end,
                            error)) {
    network_sidebar_amneziawg_secret_wipe(quick_config->data, quick_config->len);
    network_sidebar_amneziawg_config_envelope_clear(&parsed);
    return FALSE;
  }
  dns_measure = network_sidebar_amneziawg_dns_config_measure(&parsed.dns,
                                                            &dns_payload_size);
  if (dns_measure != NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_MEASURE_OK) {
    g_set_error_literal(
      error,
      NETWORK_SIDEBAR_AMNEZIAWG_ERROR,
      dns_measure == NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_MEASURE_TOO_LARGE ?
        NETWORK_SIDEBAR_AMNEZIAWG_ERROR_TOO_LARGE :
        NETWORK_SIDEBAR_AMNEZIAWG_ERROR_INVALID_DATA,
      dns_measure == NETWORK_SIDEBAR_AMNEZIAWG_DNS_CONFIG_MEASURE_TOO_LARGE ?
        "configuration DNS data exceeds the 256 KiB runtime limit" :
        "configuration DNS data cannot be represented safely");
    network_sidebar_amneziawg_secret_wipe(quick_config->data, quick_config->len);
    network_sidebar_amneziawg_config_envelope_clear(&parsed);
    return FALSE;
  }
  if (quick_config->len > 0) {
    parsed.quick_config = g_memdup2(quick_config->data, quick_config->len);
    parsed.quick_config_length = quick_config->len;
  }
  network_sidebar_amneziawg_secret_wipe(quick_config->data, quick_config->len);
  *envelope = parsed;
  return TRUE;
}

void
network_sidebar_amneziawg_config_envelope_clear(
  NetworkSidebarAmneziaWGConfigEnvelope *envelope)
{
  if (envelope == NULL)
    return;
  network_sidebar_amneziawg_secret_free(envelope->quick_config,
                                         envelope->quick_config_length);
  network_sidebar_amneziawg_dns_config_clear(&envelope->dns);
  *envelope = (NetworkSidebarAmneziaWGConfigEnvelope) { 0 };
}

gboolean
network_sidebar_amneziawg_config_validate(const guint8 *data,
                                           gsize length,
                                           GError **error)
{
  NetworkSidebarAmneziaWGConfigEnvelope envelope = { 0 };
  gboolean valid = network_sidebar_amneziawg_config_parse_envelope(
    data, length, &envelope, error);

  network_sidebar_amneziawg_config_envelope_clear(&envelope);
  return valid;
}

gboolean
network_sidebar_amneziawg_runtime_snapshot_build(
  const guint8 *protocol_config,
  gsize protocol_config_length,
  const NetworkSidebarAmneziaWGConfigEnvelope *envelope,
  gsize size_reserve,
  guint8 **snapshot,
  gsize *snapshot_length,
  GError **error)
{
  g_autoptr(GByteArray) unexpected_quick = NULL;
  NetworkSidebarAmneziaWGDnsConfig unexpected_dns = { 0 };
  gsize interface_line_end = 0;
  gsize separator_length = 0;
  gsize result_length = 0;
  gsize output_offset = 0;
  guint8 *result = NULL;
  gboolean valid = FALSE;

  g_return_val_if_fail(envelope != NULL, FALSE);
  g_return_val_if_fail(snapshot != NULL, FALSE);
  g_return_val_if_fail(snapshot_length != NULL, FALSE);
  *snapshot = NULL;
  *snapshot_length = 0;
  unexpected_quick = g_byte_array_sized_new(
    NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE);
  network_sidebar_amneziawg_dns_config_init(&unexpected_dns);
  if (!scan_config_envelope(protocol_config,
                            protocol_config_length,
                            unexpected_quick,
                            &unexpected_dns,
                            &interface_line_end,
                            error))
    goto out;
  if (unexpected_quick->len > 0 || unexpected_dns.servers->len > 0 ||
      unexpected_dns.domains->len > 0) {
    g_set_error_literal(error,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR_INVALID_DATA,
                        "awg-quick strip output contains quick-only directives");
    goto out;
  }
  if (envelope->quick_config_length > 0 && envelope->quick_config == NULL) {
    g_set_error_literal(error,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR_INVALID_DATA,
                        "configuration envelope is incomplete");
    goto out;
  }
  if (envelope->quick_config_length > 0 && interface_line_end > 0 &&
      protocol_config[interface_line_end - 1] != '\n')
    separator_length = 1;
  if (size_reserve > NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE ||
      envelope->quick_config_length >
        NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE - size_reserve ||
      protocol_config_length >
        NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE - size_reserve -
          envelope->quick_config_length ||
      separator_length >
        NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE - size_reserve -
          envelope->quick_config_length - protocol_config_length) {
    g_set_error_literal(error,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR,
                        NETWORK_SIDEBAR_AMNEZIAWG_ERROR_TOO_LARGE,
                        "runtime configuration exceeds the size limit");
    goto out;
  }

  result_length = protocol_config_length + envelope->quick_config_length +
                  separator_length;
  result = g_malloc(result_length);
  memcpy(result, protocol_config, interface_line_end);
  output_offset = interface_line_end;
  if (separator_length > 0)
    result[output_offset++] = '\n';
  if (envelope->quick_config_length > 0) {
    memcpy(result + output_offset,
           envelope->quick_config,
           envelope->quick_config_length);
    output_offset += envelope->quick_config_length;
  }
  memcpy(result + output_offset,
         protocol_config + interface_line_end,
         protocol_config_length - interface_line_end);
  *snapshot = result;
  *snapshot_length = result_length;
  result = NULL;
  valid = TRUE;

out:
  network_sidebar_amneziawg_secret_free(result, result_length);
  network_sidebar_amneziawg_secret_wipe(unexpected_quick->data, unexpected_quick->len);
  network_sidebar_amneziawg_dns_config_clear(&unexpected_dns);
  return valid;
}
