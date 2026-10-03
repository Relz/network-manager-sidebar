#include "amneziawg/profile_report.h"

#include <errno.h>
#include <string.h>

static void
profile_report_entry_free(NetworkSidebarAwgProfileReportEntry *entry)
{
  g_free(entry->name);
  g_free(entry);
}

NetworkSidebarAwgProfileReport *
network_sidebar_awg_profile_report_new(void)
{
  NetworkSidebarAwgProfileReport *report = g_new0(NetworkSidebarAwgProfileReport, 1);

  report->records = g_ptr_array_new_with_free_func(
    (GDestroyNotify) profile_report_entry_free);
  report->complete = TRUE;
  return report;
}

void
network_sidebar_awg_profile_report_free(NetworkSidebarAwgProfileReport *report)
{
  if (report == NULL)
    return;
  g_ptr_array_unref(report->records);
  g_free(report);
}

gboolean
network_sidebar_awg_profile_report_add(NetworkSidebarAwgProfileReport *report,
                                       const char *name,
                                       NetworkSidebarAmneziaWGProfileStatus status)
{
  NetworkSidebarAwgProfileReportEntry *entry;

  if (report == NULL ||
      report->records->len >= NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_REPORT_MAX_RECORDS ||
      !network_sidebar_amneziawg_name_is_valid(name) ||
      !network_sidebar_amneziawg_profile_status_is_valid(status))
    return FALSE;
  for (guint i = 0; i < report->records->len; i++) {
    const NetworkSidebarAwgProfileReportEntry *existing = g_ptr_array_index(report->records, i);

    if (strcmp(existing->name, name) == 0)
      return FALSE;
  }
  entry = g_new0(NetworkSidebarAwgProfileReportEntry, 1);
  entry->name = g_strdup(name);
  entry->status = status;
  g_ptr_array_add(report->records, entry);
  return TRUE;
}

GBytes *
network_sidebar_awg_profile_report_encode(const NetworkSidebarAwgProfileReport *report)
{
  g_autoptr(GString) output = g_string_new(NULL);
  g_autoptr(GHashTable) names = g_hash_table_new(g_str_hash, g_str_equal);

  if (report == NULL || report->records == NULL ||
      report->records->len > NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_REPORT_MAX_RECORDS)
    return NULL;
  g_string_append_printf(output,
                         NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_REPORT_MAGIC "\t%u\t%u\t%u\n",
                         NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_REPORT_VERSION,
                         report->records->len, report->complete ? 1u : 0u);
  for (guint i = 0; i < report->records->len; i++) {
    const NetworkSidebarAwgProfileReportEntry *entry = g_ptr_array_index(report->records, i);

    if (entry == NULL || !network_sidebar_amneziawg_name_is_valid(entry->name) ||
        !network_sidebar_amneziawg_profile_status_is_valid(entry->status) ||
        g_hash_table_contains(names, entry->name))
      return NULL;
    g_hash_table_add(names, entry->name);
    g_string_append_printf(output, "%u\t%s\n", (guint) entry->status, entry->name);
  }
  if (output->len > NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_REPORT_MAX_SIZE)
    return NULL;
  return g_string_free_to_bytes(g_steal_pointer(&output));
}

static gboolean
parse_uint(const char *text, guint maximum, guint *value)
{
  char *end = NULL;
  guint64 parsed;

  if (text == NULL || text[0] == '\0')
    return FALSE;
  for (const char *character = text; *character != '\0'; character++) {
    if (!g_ascii_isdigit(*character))
      return FALSE;
  }
  errno = 0;
  parsed = g_ascii_strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || parsed > maximum)
    return FALSE;
  *value = (guint) parsed;
  return TRUE;
}

NetworkSidebarAwgProfileReport *
network_sidebar_awg_profile_report_decode(const guint8 *data,
                                          gsize length,
                                          NetworkSidebarAmneziaWGHelperExit status)
{
  g_autofree char *text = NULL;
  g_auto(GStrv) lines = NULL;
  g_auto(GStrv) header = NULL;
  g_autoptr(NetworkSidebarAwgProfileReport) report = NULL;
  guint version;
  guint count;
  guint complete;

  if ((status != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS &&
       status != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVENTORY_PARTIAL) ||
      data == NULL || length == 0 ||
      length > NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_REPORT_MAX_SIZE ||
      data[length - 1] != '\n' || memchr(data, '\0', length) != NULL ||
      !g_utf8_validate((const char *) data, length, NULL))
    return NULL;
  text = g_strndup((const char *) data, length);
  lines = g_strsplit(text, "\n", -1);
  header = g_strsplit(lines[0], "\t", -1);
  if (g_strv_length(header) != 4 ||
      strcmp(header[0], NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_REPORT_MAGIC) != 0 ||
      !parse_uint(header[1], G_MAXUINT, &version) ||
      version != NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_REPORT_VERSION ||
      !parse_uint(header[2], NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_REPORT_MAX_RECORDS, &count) ||
      !parse_uint(header[3], 1, &complete) ||
      g_strv_length(lines) != count + 2 || lines[count + 1][0] != '\0' ||
      (status == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) != (complete != 0))
    return NULL;

  report = network_sidebar_awg_profile_report_new();
  report->complete = complete != 0;
  for (guint i = 0; i < count; i++) {
    g_auto(GStrv) fields = g_strsplit(lines[i + 1], "\t", -1);
    guint profile_status;

    if (g_strv_length(fields) != 2 ||
        !parse_uint(fields[0], G_MAXUINT, &profile_status) ||
        !network_sidebar_awg_profile_report_add(report, fields[1], profile_status))
      return NULL;
  }
  return g_steal_pointer(&report);
}
