#include "helper/amneziawg_firewall.h"

#include "amneziawg/deadline.h"
#include "helper/amneziawg_process.h"
#include "helper/amneziawg_tool.h"

#include <json-glib/json-glib.h>
#include <string.h>

#define FIREWALL_BUDGET_MSEC 15000u

typedef struct {
  guint scope;
  const char *reader;
  const char *fallback;
  const char *version_suffix;
} IptablesReader;

static const IptablesReader iptables_readers[] = {
  { NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_IP4_LEGACY,
    "iptables-legacy-save", "iptables-save", "(legacy)" },
  { NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_IP6_LEGACY,
    "ip6tables-legacy-save", "ip6tables-save", "(legacy)" },
  { NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_IP4_NFT,
    "iptables-nft-save", "iptables-save", "(nf_tables)" },
  { NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_IP6_NFT,
    "ip6tables-nft-save", "ip6tables-save", "(nf_tables)" },
};

static gboolean
within_deadline(gint64 deadline)
{
  gint64 now = network_sidebar_amneziawg_process_monotonic_msec();

  return now >= 0 && now < deadline &&
         !network_sidebar_amneziawg_process_termination_requested();
}

static char *
output_text(GBytes *output)
{
  gsize length;
  const char *data;

  if (output == NULL)
    return NULL;
  data = g_bytes_get_data(output, &length);
  if (length > AWG_TOOL_OUTPUT_LIMIT ||
      (length != 0 && (memchr(data, '\0', length) != NULL ||
                      !g_utf8_validate(data, length, NULL))))
    return NULL;
  return length == 0 ? g_strdup("") : g_strndup(data, length);
}

static char *
tool_version(AwgTool *executable, const char *tool, gint64 deadline)
{
  const char *const argv[] = { tool, "--version", NULL };
  g_autoptr(GBytes) output = NULL;
  char *text;

  if (!awg_tool_read(executable, argv, deadline, &output))
    return NULL;
  text = output_text(output);
  if (text != NULL)
    g_strstrip(text);
  return text;
}

gboolean
awg_firewall_capture_scope(gint64 outer_deadline, guint *scope)
{
  static const char *const generic_tools[] = { "iptables", "ip6tables" };
  gint64 deadline = network_sidebar_amneziawg_deadline_cap(
    network_sidebar_amneziawg_process_monotonic_msec(), outer_deadline,
    FIREWALL_BUDGET_MSEC);
  AwgToolAvailability availability;
  guint found = 0;

  g_return_val_if_fail(scope != NULL, FALSE);
  /* A caller retaining data after an inspection failure must be conservative. */
  *scope = NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_ALL;
  if (!within_deadline(deadline))
    return FALSE;
  availability = awg_tool_available("nft");
  if (availability == AWG_TOOL_UNTRUSTED)
    return FALSE;
  if (availability == AWG_TOOL_AVAILABLE)
    found |= NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_NFT;
  for (guint i = 0; i < G_N_ELEMENTS(iptables_readers); i++) {
    availability = awg_tool_available(iptables_readers[i].reader);
    if (availability == AWG_TOOL_UNTRUSTED)
      return FALSE;
    if (availability == AWG_TOOL_AVAILABLE)
      found |= iptables_readers[i].scope;
  }
  for (guint i = 0; i < G_N_ELEMENTS(generic_tools); i++) {
    g_autofree char *version = NULL;
    g_autoptr(AwgTool) executable = NULL;

    availability = awg_tool_available(generic_tools[i]);
    if (availability == AWG_TOOL_MISSING)
      continue;
    if (availability != AWG_TOOL_AVAILABLE)
      return FALSE;
    executable = awg_tool_open(generic_tools[i]);
    version = tool_version(executable, generic_tools[i], deadline);
    if (version == NULL)
      return FALSE;
    if (g_str_has_suffix(version, "(nf_tables)"))
      found |= iptables_readers[2 + i].scope;
    else if (g_str_has_suffix(version, "(legacy)"))
      found |= iptables_readers[i].scope;
    else
      return FALSE;
  }
  if (!within_deadline(deadline))
    return FALSE;
  *scope = found;
  return TRUE;
}

static const char *
string_member(JsonObject *object, const char *name)
{
  JsonNode *node = json_object_get_member(object, name);

  return node != NULL && JSON_NODE_HOLDS_VALUE(node) &&
         json_node_get_value_type(node) == G_TYPE_STRING ?
    json_node_get_string(node) : NULL;
}

static AwgFirewallState
parse_nft_tables(const char *name, GBytes *output)
{
  g_autofree char *text = output_text(output);
  g_autofree char *expected = g_strdup_printf("wg-quick-%s", name);
  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(GError) error = NULL;
  JsonNode *root;
  JsonNode *node;
  JsonArray *tables;
  gboolean present = FALSE;

  if (text == NULL || *text == '\0' ||
      !json_parser_load_from_data(parser, text, -1, &error) ||
      json_parser_has_assignment(parser, NULL))
    return AWG_FIREWALL_UNKNOWN;
  root = json_parser_get_root(parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root) ||
      json_object_get_size(json_node_get_object(root)) != 1)
    return AWG_FIREWALL_UNKNOWN;
  node = json_object_get_member(json_node_get_object(root), "nftables");
  if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
    return AWG_FIREWALL_UNKNOWN;
  tables = json_node_get_array(node);
  for (guint i = 0; i < json_array_get_length(tables); i++) {
    JsonObject *entry;
    JsonObject *table;
    const char *family;
    const char *table_name;

    node = json_array_get_element(tables, i);
    if (!JSON_NODE_HOLDS_OBJECT(node))
      return AWG_FIREWALL_UNKNOWN;
    entry = json_node_get_object(node);
    if (json_object_get_size(entry) != 1)
      return AWG_FIREWALL_UNKNOWN;
    node = json_object_get_member(entry, "metainfo");
    if (node != NULL) {
      JsonNode *schema;

      if (!JSON_NODE_HOLDS_OBJECT(node))
        return AWG_FIREWALL_UNKNOWN;
      schema = json_object_get_member(json_node_get_object(node), "json_schema_version");
      if (schema == NULL || !JSON_NODE_HOLDS_VALUE(schema) ||
          json_node_get_value_type(schema) != G_TYPE_INT64 ||
          json_node_get_int(schema) != 1)
        return AWG_FIREWALL_UNKNOWN;
      continue;
    }
    node = json_object_get_member(entry, "table");
    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
      return AWG_FIREWALL_UNKNOWN;
    table = json_node_get_object(node);
    family = string_member(table, "family");
    table_name = string_member(table, "name");
    if (family == NULL || *family == '\0' || table_name == NULL || *table_name == '\0')
      return AWG_FIREWALL_UNKNOWN;
    if (strcmp(table_name, expected) == 0)
      present = TRUE;
  }
  return present ? AWG_FIREWALL_PRESENT : AWG_FIREWALL_ABSENT;
}

static gboolean
counter_pair(const char *text)
{
  const char *cursor = text;

  if (*cursor++ != '[' || !g_ascii_isdigit(*cursor))
    return FALSE;
  while (g_ascii_isdigit(*cursor))
    cursor++;
  if (*cursor++ != ':' || !g_ascii_isdigit(*cursor))
    return FALSE;
  while (g_ascii_isdigit(*cursor))
    cursor++;
  return strcmp(cursor, "]") == 0;
}

static AwgFirewallState
parse_iptables_save(const char *name, GBytes *output)
{
  g_autofree char *text = output_text(output);
  g_autofree char *expected = g_strdup_printf("awg-quick(8) rule for %s", name);
  g_auto(GStrv) lines = NULL;
  gboolean in_table = FALSE;
  gboolean present = FALSE;

  if (text == NULL || (*text != '\0' && text[strlen(text) - 1] != '\n'))
    return AWG_FIREWALL_UNKNOWN;
  /* A successful save may be empty when that namespace has no loaded tables. */
  lines = g_strsplit(text, "\n", -1);
  for (guint i = 0; lines[i] != NULL; i++) {
    const char *line = lines[i];
    g_auto(GStrv) words = NULL;
    g_autoptr(GError) error = NULL;
    int count;

    if (*line == '\0' || *line == '#')
      continue;
    if (*line == '*') {
      if (in_table || line[1] == '\0' || strpbrk(line, " \t\r") != NULL)
        return AWG_FIREWALL_UNKNOWN;
      in_table = TRUE;
      continue;
    }
    if (strcmp(line, "COMMIT") == 0) {
      if (!in_table)
        return AWG_FIREWALL_UNKNOWN;
      in_table = FALSE;
      continue;
    }
    if (!in_table || !g_shell_parse_argv(line, &count, &words, &error))
      return AWG_FIREWALL_UNKNOWN;
    if (*line == ':') {
      if (count != 3 || words[0][1] == '\0' || !counter_pair(words[2]))
        return AWG_FIREWALL_UNKNOWN;
      continue;
    }
    /* A counter-only rule has a chain but no matches or jump target. */
    if (count < 2 || strcmp(words[0], "-A") != 0 || *words[1] == '\0')
      return AWG_FIREWALL_UNKNOWN;
    for (int j = 2; j < count; j++) {
      if (strcmp(words[j], "--comment") == 0) {
        if (++j == count)
          return AWG_FIREWALL_UNKNOWN;
        if (strcmp(words[j], expected) == 0)
          present = TRUE;
      }
    }
  }
  if (in_table)
    return AWG_FIREWALL_UNKNOWN;
  return present ? AWG_FIREWALL_PRESENT : AWG_FIREWALL_ABSENT;
}

static AwgFirewallState
probe_iptables(const char *name, const IptablesReader *reader, gint64 deadline)
{
  const char *tool = reader->reader;
  AwgToolAvailability availability = awg_tool_available(tool);
  g_autofree char *version = NULL;
  g_autoptr(GBytes) output = NULL;
  const char *argv[2];
  g_autoptr(AwgTool) executable = NULL;

  if (availability == AWG_TOOL_MISSING)
    tool = reader->fallback;
  else if (availability != AWG_TOOL_AVAILABLE)
    return AWG_FIREWALL_UNKNOWN;
  executable = awg_tool_open(tool);
  version = tool_version(executable, tool, deadline);
  if (version == NULL || !g_str_has_suffix(version, reader->version_suffix))
    return AWG_FIREWALL_UNKNOWN;
  argv[0] = tool;
  argv[1] = NULL;
  if (!awg_tool_read(executable, argv, deadline, &output))
    return AWG_FIREWALL_UNKNOWN;
  return parse_iptables_save(name, output);
}

AwgFirewallState
awg_firewall_probe(const char *name, guint scope, gint64 outer_deadline)
{
  gint64 deadline = network_sidebar_amneziawg_deadline_cap(
    network_sidebar_amneziawg_process_monotonic_msec(), outer_deadline,
    FIREWALL_BUDGET_MSEC);
  AwgFirewallState result = AWG_FIREWALL_ABSENT;

  if (!network_sidebar_amneziawg_name_is_valid(name) ||
      (scope & ~NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_ALL) != 0 ||
      !within_deadline(deadline))
    return AWG_FIREWALL_UNKNOWN;
  if ((scope & NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_NFT) != 0) {
    const char *const argv[] = { "nft", "--json", "list", "tables", NULL };
    g_autoptr(GBytes) output = NULL;
    g_autoptr(AwgTool) executable = awg_tool_open("nft");

    if (!awg_tool_read(executable, argv, deadline, &output))
      return AWG_FIREWALL_UNKNOWN;
    result = parse_nft_tables(name, output);
    if (result == AWG_FIREWALL_UNKNOWN)
      return result;
  }
  for (guint i = 0; i < G_N_ELEMENTS(iptables_readers); i++) {
    AwgFirewallState state;

    if ((scope & iptables_readers[i].scope) == 0)
      continue;
    state = probe_iptables(name, &iptables_readers[i], deadline);
    if (state == AWG_FIREWALL_UNKNOWN)
      return state;
    if (state == AWG_FIREWALL_PRESENT)
      result = state;
  }
  return within_deadline(deadline) ? result : AWG_FIREWALL_UNKNOWN;
}
