#include "amneziawg/amneziawg.h"

#include <string.h>

gboolean
network_sidebar_amneziawg_name_is_valid(const char *name)
{
  gsize length;

  if (name == NULL)
    return FALSE;
  length = strlen(name);
  if (length == 0 || length > NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH)
    return FALSE;
  if (!g_ascii_isalnum(name[0]))
    return FALSE;

  for (gsize i = 1; i < length; i++) {
    if (!g_ascii_isalnum(name[i]) && name[i] != '_' && name[i] != '-' && name[i] != '.')
      return FALSE;
  }
  return TRUE;
}

gboolean
network_sidebar_amneziawg_profile_status_is_valid(guint status)
{
  return status <= NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_CHANGED_OR_UNREADABLE;
}

gboolean
network_sidebar_amneziawg_profile_status_allows_removal(
  NetworkSidebarAmneziaWGProfileStatus status)
{
  return status == NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_AVAILABLE ||
         status == NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_INVALID_CONFIGURATION ||
         status == NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_INCOMPATIBLE_CONFIGURATION;
}

gboolean
network_sidebar_amneziawg_helper_exit_is_valid(guint status)
{
  switch (status) {
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_USAGE:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_NOT_ROOT:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_NAME:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INPUT_TOO_LARGE:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_CONFIG:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_REPLACE_CONFIRMATION_REQUIRED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_NOT_FOUND:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ACTIVE:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_AWG_QUICK_UNAVAILABLE:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_AWG_QUICK_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ROLLBACK_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INPUT_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CREATION_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_USERSPACE_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SETCONF_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ADDRESS_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_MTU_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ROUTE_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FIREWALL_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SIGNALED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_BACKEND_UNAVAILABLE:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_APPLY_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_REVERT_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_ROLLBACK_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVENTORY_PARTIAL:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_TIMED_OUT:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FIREWALL_CLEANUP_FAILED:
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SWITCH_CLEANUP_FAILED:
    return TRUE;
  default:
    return FALSE;
  }
}

char *
network_sidebar_amneziawg_interface_from_filename(const char *filename)
{
  g_autofree char *basename = NULL;
  g_autoptr(GString) normalized = NULL;
  gsize stem_length;
  gboolean pending_separator = FALSE;

  if (filename == NULL || *filename == '\0')
    return NULL;

  basename = g_path_get_basename(filename);
  stem_length = strlen(basename);
  if (stem_length >= 5 && g_ascii_strcasecmp(basename + stem_length - 5, ".conf") == 0)
    stem_length -= 5;

  normalized = g_string_sized_new(MIN(stem_length, NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH));
  for (gsize i = 0; i < stem_length; i++) {
    guint8 byte = (guint8) basename[i];

    if (g_ascii_isalnum(byte)) {
      if (pending_separator && normalized->len > 0 &&
          normalized->len + 1 < NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH)
        g_string_append_c(normalized, '-');
      pending_separator = FALSE;
      if (normalized->len < NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH)
        g_string_append_c(normalized, byte);
    } else if ((byte == '_' || byte == '-' || byte == '.') && normalized->len > 0) {
      pending_separator = FALSE;
      if (normalized->len < NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH)
        g_string_append_c(normalized, byte);
    } else if (normalized->len > 0) {
      pending_separator = TRUE;
    }
  }

  if (normalized->len == 0)
    return NULL;
  return g_string_free(g_steal_pointer(&normalized), FALSE);
}

gboolean
network_sidebar_amneziawg_link_token_is_valid(const char *token)
{
  if (token == NULL ||
      strlen(token) != NETWORK_SIDEBAR_AMNEZIAWG_LINK_TOKEN_LENGTH)
    return FALSE;
  for (gsize i = 0; i < NETWORK_SIDEBAR_AMNEZIAWG_LINK_TOKEN_LENGTH; i++) {
    if (!g_ascii_isdigit(token[i]) && (token[i] < 'a' || token[i] > 'f'))
      return FALSE;
  }
  return TRUE;
}

void
network_sidebar_amneziawg_secret_wipe(void *data, gsize length)
{
  volatile guint8 *bytes = data;

  while (length-- > 0)
    *bytes++ = 0;
}

void
network_sidebar_amneziawg_secret_free(guint8 *data, gsize length)
{
  if (data == NULL)
    return;
  network_sidebar_amneziawg_secret_wipe(data, length);
  g_free(data);
}
