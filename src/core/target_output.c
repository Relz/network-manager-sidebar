#include "core/target_output.h"

#include <string.h>

static char *
utf8_strip_copy(const char *value)
{
  const char *start;
  const char *end;

  if (value == NULL)
    return NULL;
  if (!g_utf8_validate(value, -1, NULL))
    return NULL;

  start = value;
  end = value + strlen(value);

  while (start < end) {
    gunichar ch = g_utf8_get_char(start);
    if (!g_unichar_isspace(ch))
      break;
    start = g_utf8_next_char(start);
  }

  while (end > start) {
    const char *previous = g_utf8_prev_char(end);
    gunichar ch = g_utf8_get_char(previous);
    if (!g_unichar_isspace(ch))
      break;
    end = previous;
  }

  return g_strndup(start, end - start);
}

char *
network_sidebar_normalize_target_output(const char *output_name)
{
  g_autofree char *trimmed = NULL;
  glong char_count = 0;

  if (output_name == NULL)
    return NULL;

  trimmed = utf8_strip_copy(output_name);
  if (trimmed == NULL)
    return NULL;
  if (*trimmed == '\0')
    return NULL;
  if (strchr(trimmed, '\n') != NULL || strchr(trimmed, '\t') != NULL)
    return NULL;

  char_count = g_utf8_strlen(trimmed, -1);
  if (char_count > NETWORK_SIDEBAR_TARGET_OUTPUT_MAX_CHARS)
    return NULL;

  return g_strdup(trimmed);
}

char *
network_sidebar_target_output_from_environment(void)
{
  static const char *env_vars[] = {
    "NM_SIDEBAR_OUTPUT",
    "WAYBAR_OUTPUT_NAME",
    NULL,
  };

  for (gsize i = 0; env_vars[i] != NULL; i++) {
    g_autofree char *normalized = network_sidebar_normalize_target_output(g_getenv(env_vars[i]));
    if (normalized != NULL)
      return g_steal_pointer(&normalized);
  }

  return NULL;
}
