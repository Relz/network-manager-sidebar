#include "helper/amneziawg_helper_profiles.h"

#include "amneziawg/config.h"
#include "amneziawg/profile_report.h"
#include "helper/amneziawg_helper_quick.h"
#include "helper/amneziawg_helper_session.h"
#include "helper/amneziawg_helper_util.h"
#include "helper/amneziawg_process.h"
#include "helper/amneziawg_quick_config.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct _AwgHelperProfiles {
  AwgHelperStorage *storage;
  NetworkSidebarAwgProfileReport *report;
};

struct _AwgHelperProfileInput {
  guint8 *contents;
  gsize length;
};

struct _AwgHelperPreparedProfile {
  guint8 *original;
  guint8 *runtime_snapshot;
  guint8 *claimed_snapshot;
  guint8 *dns_payload;
  gsize original_length;
  gsize runtime_snapshot_length;
  gsize claimed_snapshot_length;
  gsize dns_payload_length;
  NetworkSidebarAmneziaWGDnsConfig dns;
};

AwgHelperProfiles *
awg_helper_profiles_new(AwgHelperStorage *storage)
{
  AwgHelperProfiles *profiles = g_new0(AwgHelperProfiles, 1);

  profiles->storage = storage;
  profiles->report = network_sidebar_awg_profile_report_new();
  return profiles;
}

void
awg_helper_profiles_free(AwgHelperProfiles *profiles)
{
  if (profiles == NULL)
    return;
  network_sidebar_awg_profile_report_free(profiles->report);
  g_free(profiles);
}

static NetworkSidebarAmneziaWGHelperExit
read_config(guint8 **contents, gsize *length, gint64 deadline)
{
  guint8 *buffer = g_malloc(NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE + 1);
  gsize used = 0;

  while (used <= NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE) {
    NetworkSidebarAmneziaWGHelperExit stop =
      awg_helper_phase_stop_status(deadline);
    gint64 now;
    struct pollfd input = { STDIN_FILENO, POLLIN | POLLHUP, 0 };
    int polled;

    if (stop != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
      awg_helper_wipe_bytes(buffer,
                            NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE + 1);
      g_free(buffer);
      return stop;
    }
    now = network_sidebar_amneziawg_process_monotonic_msec();
    if (now < 0 || now >= deadline) {
      awg_helper_wipe_bytes(buffer,
                            NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE + 1);
      g_free(buffer);
      return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_TIMED_OUT;
    }
    polled = network_sidebar_amneziawg_process_poll(
      &input, 1, (int) MIN(deadline - now, (gint64) G_MAXINT));
    if (polled < 0 && errno == EINTR)
      continue;
    if (polled == 0) {
      awg_helper_wipe_bytes(buffer,
                            NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE + 1);
      g_free(buffer);
      return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_TIMED_OUT;
    }
    if (polled < 0 || (input.revents & (POLLERR | POLLNVAL)) != 0) {
      awg_helper_wipe_bytes(buffer,
                            NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE + 1);
      g_free(buffer);
      return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INPUT_FAILED;
    }
    {
      ssize_t count = read(
        STDIN_FILENO,
        buffer + used,
        NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE + 1 - used);

      if (count < 0) {
        if (errno == EINTR)
          continue;
        awg_helper_wipe_bytes(buffer,
                              NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE + 1);
        g_free(buffer);
        return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INPUT_FAILED;
      }
      if (count == 0)
        break;
      used += (gsize) count;
    }
  }
  if (used > NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE) {
    awg_helper_wipe_bytes(buffer,
                          NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE + 1);
    g_free(buffer);
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INPUT_TOO_LARGE;
  }

  *contents = buffer;
  *length = used;
  return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
}

AwgHelperProfileInput *
awg_helper_profiles_read_input(gint64 deadline,
                               NetworkSidebarAmneziaWGHelperExit *result)
{
  g_autoptr(GError) validation_error = NULL;
  AwgHelperProfileInput *input = g_new0(AwgHelperProfileInput, 1);

  *result = read_config(&input->contents, &input->length, deadline);
  if (*result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return input;
  if (!network_sidebar_amneziawg_config_validate(input->contents,
                                                  input->length,
                                                  &validation_error)) {
    *result = validation_error != NULL &&
              validation_error->code == NETWORK_SIDEBAR_AMNEZIAWG_ERROR_TOO_LARGE ?
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INPUT_TOO_LARGE :
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_CONFIG;
  }
  return input;
}

void
awg_helper_profile_input_free(AwgHelperProfileInput *input)
{
  if (input == NULL)
    return;
  if (input->contents != NULL) {
    awg_helper_wipe_bytes(input->contents,
                          NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE + 1);
    g_free(input->contents);
  }
  g_free(input);
}

static NetworkSidebarAmneziaWGHelperExit
write_candidate(AwgHelperProfiles *profiles,
                const guint8 *contents,
                gsize length,
                char *temp_name,
                gsize temp_name_size,
                guint8 **snapshot,
                gsize *snapshot_length,
                gint64 deadline)
{
  char temp_path[PATH_MAX];
  g_autoptr(GError) parse_error = NULL;
  NetworkSidebarAmneziaWGConfigEnvelope envelope = { 0 };
  NetworkSidebarAmneziaWGHelperExit result =
    NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_CONFIG;
  guint8 *protocol_config = NULL;
  guint8 *runtime_snapshot = NULL;
  gsize protocol_config_length = 0;
  gsize runtime_snapshot_length = 0;
  gboolean candidate_created = FALSE;
  int written;

  if ((snapshot == NULL) != (snapshot_length == NULL))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED;
  if (snapshot != NULL) {
    *snapshot = NULL;
    *snapshot_length = 0;
  }
  if (!network_sidebar_amneziawg_config_parse_envelope(contents,
                                                        length,
                                                        &envelope,
                                                        &parse_error))
    goto out;
  if (!awg_helper_storage_write_staging_candidate(profiles->storage,
                                                   contents,
                                                   length,
                                                   temp_name,
                                                   temp_name_size,
                                                   &candidate_created))
    goto filesystem_failed;
  written = snprintf(temp_path,
                     sizeof(temp_path),
                     "%s/%s",
                     NETWORK_SIDEBAR_AMNEZIAWG_STAGING_DIR,
                     temp_name);
  if (written <= 0 || (gsize) written >= sizeof(temp_path))
    goto filesystem_failed;

  result = awg_helper_quick_run("strip",
                                temp_path,
                                &protocol_config,
                                &protocol_config_length,
                                deadline).status;
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    if (result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_AWG_QUICK_FAILED)
      result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_CONFIG;
    goto out;
  }
  if (!awg_quick_config_build_snapshot(
        protocol_config,
        protocol_config_length,
        &envelope,
        &runtime_snapshot,
        &runtime_snapshot_length,
        &parse_error)) {
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_CONFIG;
    goto out;
  }
  if (snapshot != NULL) {
    *snapshot = runtime_snapshot;
    *snapshot_length = runtime_snapshot_length;
    runtime_snapshot = NULL;
    runtime_snapshot_length = 0;
  }
  result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
  goto out;

filesystem_failed:
  result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;

out:
  network_sidebar_amneziawg_secret_free(protocol_config,
                                         protocol_config_length);
  network_sidebar_amneziawg_secret_free(runtime_snapshot,
                                         runtime_snapshot_length);
  network_sidebar_amneziawg_config_envelope_clear(&envelope);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS &&
      candidate_created &&
      !awg_helper_storage_remove_and_sync(profiles->storage,
                                          AWG_HELPER_STORAGE_STAGING,
                                          temp_name))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
  return result;
}

static NetworkSidebarAmneziaWGHelperExit
load_stored_config(AwgHelperProfiles *profiles,
                   const char *config_name,
                   NetworkSidebarAmneziaWGDnsConfig *dns,
                   guint8 **original,
                   gsize *original_length,
                   AwgHelperStorageIdentity *identity)
{
  g_autoptr(GError) validation_error = NULL;
  NetworkSidebarAmneziaWGConfigEnvelope envelope = { 0 };
  guint8 *contents = NULL;
  gsize length = 0;
  NetworkSidebarAmneziaWGHelperExit result;

  *original = NULL;
  *original_length = 0;
  *dns = (NetworkSidebarAmneziaWGDnsConfig) { 0 };
  result = awg_helper_storage_read_config(profiles->storage,
                                          config_name,
                                          &contents,
                                          &length,
                                          identity);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    goto out;
  if (!network_sidebar_amneziawg_config_parse_envelope(contents,
                                                        length,
                                                        &envelope,
                                                        &validation_error)) {
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_CONFIG;
    goto out;
  }
  *dns = envelope.dns;
  envelope.dns = (NetworkSidebarAmneziaWGDnsConfig) { 0 };
  *original = contents;
  *original_length = length;
  contents = NULL;

out:
  network_sidebar_amneziawg_config_envelope_clear(&envelope);
  if (contents != NULL) {
    awg_helper_wipe_bytes(contents,
                          NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE + 1);
    g_free(contents);
  }
  return result;
}

static NetworkSidebarAmneziaWGHelperExit
replacement_target_status(AwgHelperProfiles *profiles,
                          const char *name,
                          const char *config_name)
{
  gboolean marker;
  gboolean interface;
  AwgHelperEntryState config_state = awg_helper_storage_secure_file_state(
    profiles->storage,
    AWG_HELPER_STORAGE_CONFIG,
    config_name,
    0600);
  NetworkSidebarAmneziaWGHelperExit result;

  if (config_state != AWG_HELPER_ENTRY_VALID)
    return config_state == AWG_HELPER_ENTRY_MISSING ?
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_NOT_FOUND :
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;

  result = awg_helper_session_runtime_state(profiles->storage,
                                            name,
                                            &marker,
                                            &interface,
                                            NULL);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  if (marker)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ACTIVE;
  if (interface)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT;
  return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
}

NetworkSidebarAmneziaWGHelperExit
awg_helper_profiles_import(AwgHelperProfiles *profiles,
                           const char *name,
                           const char *config_name,
                           const AwgHelperProfileInput *input,
                           gint64 deadline)
{
  char temp_name[64] = { 0 };
  NetworkSidebarAmneziaWGHelperExit result;

  result = replacement_target_status(profiles, name, config_name);
  if (result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_REPLACE_CONFIRMATION_REQUIRED;
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_NOT_FOUND)
    return result;

  result = write_candidate(profiles,
                           input->contents,
                           input->length,
                           temp_name,
                           sizeof(temp_name),
                           NULL,
                           NULL,
                           deadline);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  result = awg_helper_phase_stop_status(deadline);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    if (!awg_helper_storage_remove_and_sync(profiles->storage,
                                            AWG_HELPER_STORAGE_STAGING,
                                            temp_name))
      return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
    return result;
  }
  if (!awg_helper_storage_rename_staging_noreplace(profiles->storage,
                                                   temp_name,
                                                   config_name)) {
    int rename_error = errno;

    result = rename_error == EEXIST ?
      replacement_target_status(profiles, name, config_name) :
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
    if (!awg_helper_storage_remove_and_sync(profiles->storage,
                                            AWG_HELPER_STORAGE_STAGING,
                                            temp_name))
      return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
    if (result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
      return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_REPLACE_CONFIRMATION_REQUIRED;
    return result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_NOT_FOUND ?
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED : result;
  }
  if (!awg_helper_storage_sync_config_and_staging(profiles->storage)) {
    gboolean config_removed = awg_helper_storage_remove_and_sync(
      profiles->storage, AWG_HELPER_STORAGE_CONFIG, config_name);

    return config_removed ?
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED :
      NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ROLLBACK_FAILED;
  }

  return awg_helper_phase_stop_status(deadline);
}

static NetworkSidebarAmneziaWGHelperExit
rollback_replacement(AwgHelperProfiles *profiles,
                     const char *temp_name,
                     const char *config_name)
{
  if (!awg_helper_storage_exchange_staging_config(profiles->storage,
                                                  temp_name,
                                                  config_name) ||
      !awg_helper_storage_sync_config_and_staging(profiles->storage))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ROLLBACK_FAILED;
  if (!awg_helper_storage_remove_and_sync(profiles->storage,
                                          AWG_HELPER_STORAGE_STAGING,
                                          temp_name))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
  return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
}

NetworkSidebarAmneziaWGHelperExit
awg_helper_profiles_replace(AwgHelperProfiles *profiles,
                            const char *name,
                            const char *config_name,
                            const AwgHelperProfileInput *input,
                            gint64 deadline)
{
  char temp_name[64] = { 0 };
  NetworkSidebarAmneziaWGHelperExit result;
  AwgHelperStorageIdentity original;

  result = replacement_target_status(profiles, name, config_name);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  if (!awg_helper_storage_get_config_identity(profiles->storage,
                                               config_name,
                                               &original))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;

  result = write_candidate(profiles,
                           input->contents,
                           input->length,
                           temp_name,
                           sizeof(temp_name),
                           NULL,
                           NULL,
                           deadline);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  result = replacement_target_status(profiles, name, config_name);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    if (!awg_helper_storage_remove_and_sync(profiles->storage,
                                            AWG_HELPER_STORAGE_STAGING,
                                            temp_name))
      return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
    return result;
  }
  result = awg_helper_phase_stop_status(deadline);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    if (!awg_helper_storage_remove_and_sync(profiles->storage,
                                            AWG_HELPER_STORAGE_STAGING,
                                            temp_name))
      return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
    return result;
  }
  if (!awg_helper_storage_exchange_staging_config(profiles->storage,
                                                  temp_name,
                                                  config_name)) {
    if (!awg_helper_storage_remove_and_sync(profiles->storage,
                                            AWG_HELPER_STORAGE_STAGING,
                                            temp_name))
      return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
  }
  if (!awg_helper_storage_displaced_matches_identity(profiles->storage,
                                                      temp_name,
                                                      &original))
    return rollback_replacement(profiles, temp_name, config_name);
  if (!awg_helper_storage_sync_config_and_staging(profiles->storage))
    return rollback_replacement(profiles, temp_name, config_name);
  if (!awg_helper_storage_unlink_staging(profiles->storage, temp_name))
    return rollback_replacement(profiles, temp_name, config_name);
  if (!awg_helper_storage_sync_staging(profiles->storage))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ROLLBACK_FAILED;
  return awg_helper_phase_stop_status(deadline);
}

static AwgHelperEntryState
config_entry_name(
  const char *entry_name,
  char name[NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH + 1])
{
  gsize entry_length = strlen(entry_name);
  gsize name_length;

  if (entry_length < strlen(".conf") ||
      strcmp(entry_name + entry_length - strlen(".conf"), ".conf") != 0)
    return AWG_HELPER_ENTRY_MISSING;
  name_length = entry_length - strlen(".conf");
  if (name_length > NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH)
    return AWG_HELPER_ENTRY_INVALID;
  memcpy(name, entry_name, name_length);
  name[name_length] = '\0';
  return network_sidebar_amneziawg_name_is_valid(name) ?
    AWG_HELPER_ENTRY_VALID : AWG_HELPER_ENTRY_INVALID;
}

static NetworkSidebarAmneziaWGHelperExit
inspect_profile(AwgHelperProfiles *profiles,
                const char *config_name,
                NetworkSidebarAmneziaWGProfileStatus *status,
                gint64 deadline)
{
  char temp_name[64] = { 0 };
  guint8 *original = NULL;
  guint8 *dns_payload = NULL;
  gsize original_length = 0;
  gsize dns_payload_length = 0;
  NetworkSidebarAmneziaWGDnsConfig dns = { 0 };
  NetworkSidebarAmneziaWGHelperExit result;
  AwgHelperStorageIdentity identity;
  AwgHelperEntryState config_security;

  *status = NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_CHANGED_OR_UNREADABLE;
  config_security = awg_helper_storage_config_security_state(profiles->storage,
                                                              config_name);
  if (config_security == AWG_HELPER_ENTRY_INVALID) {
    *status = NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_FILE_SECURITY;
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
  }
  if (config_security == AWG_HELPER_ENTRY_MISSING ||
      awg_helper_storage_secure_file_state(profiles->storage,
                                           AWG_HELPER_STORAGE_CONFIG,
                                           config_name,
                                           0600) != AWG_HELPER_ENTRY_VALID)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;

  result = load_stored_config(profiles,
                              config_name,
                              &dns,
                              &original,
                              &original_length,
                              &identity);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    if (result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_CONFIG)
      *status = NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_INVALID_CONFIGURATION;
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
    goto out;
  }
  if (!network_sidebar_amneziawg_dns_config_serialize(&dns,
                                                       &dns_payload,
                                                       &dns_payload_length)) {
    *status = NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_INVALID_CONFIGURATION;
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
    goto out;
  }

  result = write_candidate(profiles,
                           original,
                           original_length,
                           temp_name,
                           sizeof(temp_name),
                           NULL,
                           NULL,
                           deadline);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
    if (result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_CONFIG) {
      *status = NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_INCOMPATIBLE_CONFIGURATION;
      result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
      goto out;
    } else if (result ==
               NETWORK_SIDEBAR_AMNEZIAWG_HELPER_AWG_QUICK_UNAVAILABLE) {
      result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
    } else {
      goto out;
    }
  } else if (!awg_helper_storage_remove_and_sync(
               profiles->storage,
               AWG_HELPER_STORAGE_STAGING,
               temp_name)) {
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
    goto out;
  }

  if (!awg_helper_storage_config_matches_identity(profiles->storage,
                                                   config_name,
                                                   &identity)) {
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
    goto out;
  }
  *status = NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_AVAILABLE;

out:
  network_sidebar_amneziawg_secret_free(original, original_length);
  network_sidebar_amneziawg_secret_free(dns_payload, dns_payload_length);
  network_sidebar_amneziawg_dns_config_clear(&dns);
  return result;
}

static gint
profile_record_compare(gconstpointer left, gconstpointer right)
{
  const NetworkSidebarAwgProfileReportEntry *left_record =
    *(NetworkSidebarAwgProfileReportEntry * const *) left;
  const NetworkSidebarAwgProfileReportEntry *right_record =
    *(NetworkSidebarAwgProfileReportEntry * const *) right;

  return strcmp(left_record->name, right_record->name);
}

static NetworkSidebarAmneziaWGHelperExit
list_profiles(AwgHelperProfiles *profiles,
              NetworkSidebarAwgProfileReport *report,
              gint64 deadline)
{
  NetworkSidebarAmneziaWGHelperExit result =
    NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
  AwgHelperStorageIterator *iterator =
    awg_helper_storage_iterator_new(profiles->storage, AWG_HELPER_STORAGE_CONFIG);

  if (iterator == NULL)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;

  while (TRUE) {
    char name[NETWORK_SIDEBAR_AMNEZIAWG_MAX_INTERFACE_LENGTH + 1];
    const char *entry_name = NULL;
    AwgHelperEntryState entry_state;
    AwgHelperStorageIteratorResult iterator_result;
    NetworkSidebarAmneziaWGProfileStatus status =
      NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_CHANGED_OR_UNREADABLE;
    NetworkSidebarAmneziaWGHelperExit profile_result;

    result = awg_helper_phase_stop_status(deadline);
    if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
      break;
    iterator_result = awg_helper_storage_iterator_next(iterator, &entry_name);
    if (iterator_result != AWG_HELPER_STORAGE_ITERATOR_ENTRY) {
      if (iterator_result == AWG_HELPER_STORAGE_ITERATOR_FAILED)
        result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
      break;
    }
    entry_state = config_entry_name(entry_name, name);
    if (entry_state == AWG_HELPER_ENTRY_MISSING)
      continue;
    if (entry_state == AWG_HELPER_ENTRY_INVALID) {
      report->complete = FALSE;
      continue;
    }
    if (report->records->len >=
        NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_REPORT_MAX_RECORDS) {
      report->complete = FALSE;
      break;
    }

    profile_result = inspect_profile(profiles, entry_name, &status, deadline);
    if (profile_result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS) {
      result = profile_result;
      break;
    }
    if (!network_sidebar_awg_profile_report_add(report, name, status))
      report->complete = FALSE;
  }

  if (!awg_helper_storage_iterator_free(iterator) &&
      result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
  if (result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS && !report->complete)
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVENTORY_PARTIAL;
  g_ptr_array_sort(report->records, profile_record_compare);
  return result;
}

static gboolean
write_profile_report(const NetworkSidebarAwgProfileReport *report)
{
  g_autoptr(GBytes) output = network_sidebar_awg_profile_report_encode(report);
  gsize length;
  const guint8 *data;

  if (output == NULL)
    return FALSE;
  data = g_bytes_get_data(output, &length);
  return awg_helper_write_all(STDOUT_FILENO, data, length);
}

NetworkSidebarAmneziaWGHelperExit
awg_helper_profiles_list(AwgHelperProfiles *profiles, gint64 deadline)
{
  NetworkSidebarAmneziaWGHelperExit result = list_profiles(profiles,
                                                           profiles->report,
                                                           deadline);

  if ((result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS ||
       result == NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVENTORY_PARTIAL) &&
      !write_profile_report(profiles->report))
    result = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED;
  return result;
}

NetworkSidebarAmneziaWGHelperExit
awg_helper_profiles_delete(AwgHelperProfiles *profiles,
                           const char *name,
                           const char *config_name,
                           gint64 deadline)
{
  gboolean marker;
  gboolean interface;
  AwgHelperEntryState config_state = awg_helper_storage_secure_file_state(
    profiles->storage,
    AWG_HELPER_STORAGE_CONFIG,
    config_name,
    0600);
  NetworkSidebarAmneziaWGHelperExit result;

  if (config_state == AWG_HELPER_ENTRY_INVALID)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;

  result = awg_helper_session_runtime_state(profiles->storage,
                                            name,
                                            &marker,
                                            &interface,
                                            NULL);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  if (marker)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ACTIVE;
  if (interface)
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT;

  result = awg_helper_phase_stop_status(deadline);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  if (config_state != AWG_HELPER_ENTRY_MISSING &&
      !awg_helper_storage_remove_and_sync(profiles->storage,
                                          AWG_HELPER_STORAGE_CONFIG,
                                          config_name))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;

  return awg_helper_phase_stop_status(deadline);
}

NetworkSidebarAmneziaWGHelperExit
awg_helper_profiles_prepare_runtime(AwgHelperProfiles *profiles,
                                    const char *config_name,
                                    gint64 deadline,
                                    AwgHelperPreparedProfile **prepared_out)
{
  char temp_name[64] = { 0 };
  AwgHelperPreparedProfile *prepared = g_new0(AwgHelperPreparedProfile, 1);
  NetworkSidebarAmneziaWGHelperExit result;

  *prepared_out = prepared;
  result = load_stored_config(profiles,
                              config_name,
                              &prepared->dns,
                              &prepared->original,
                              &prepared->original_length,
                              NULL);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  result = write_candidate(profiles,
                           prepared->original,
                           prepared->original_length,
                           temp_name,
                           sizeof(temp_name),
                           &prepared->runtime_snapshot,
                           &prepared->runtime_snapshot_length,
                           deadline);
  if (result != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  if (!awg_helper_storage_remove_and_sync(profiles->storage,
                                          AWG_HELPER_STORAGE_STAGING,
                                          temp_name))
    return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED;
  return NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS;
}

gboolean
awg_helper_prepared_profile_add_link_claim(AwgHelperPreparedProfile *prepared,
                                           const char *link_token)
{
  if (!awg_quick_config_add_link_claim(
        prepared->runtime_snapshot,
        prepared->runtime_snapshot_length,
        link_token,
        &prepared->claimed_snapshot,
        &prepared->claimed_snapshot_length,
        NULL))
    return FALSE;
  network_sidebar_amneziawg_secret_free(prepared->runtime_snapshot,
                                         prepared->runtime_snapshot_length);
  prepared->runtime_snapshot = prepared->claimed_snapshot;
  prepared->runtime_snapshot_length = prepared->claimed_snapshot_length;
  prepared->claimed_snapshot = NULL;
  prepared->claimed_snapshot_length = 0;
  return TRUE;
}

gboolean
awg_helper_prepared_profile_serialize_dns(AwgHelperPreparedProfile *prepared)
{
  return network_sidebar_amneziawg_dns_config_serialize(
    &prepared->dns,
    &prepared->dns_payload,
    &prepared->dns_payload_length);
}

const NetworkSidebarAmneziaWGDnsConfig *
awg_helper_prepared_profile_dns(const AwgHelperPreparedProfile *prepared)
{
  return &prepared->dns;
}

const guint8 *
awg_helper_prepared_profile_snapshot(const AwgHelperPreparedProfile *prepared,
                                     gsize *length)
{
  *length = prepared->runtime_snapshot_length;
  return prepared->runtime_snapshot;
}

const guint8 *
awg_helper_prepared_profile_dns_payload(const AwgHelperPreparedProfile *prepared,
                                        gsize *length)
{
  *length = prepared->dns_payload_length;
  return prepared->dns_payload;
}

void
awg_helper_prepared_profile_free(AwgHelperPreparedProfile *prepared)
{
  if (prepared == NULL)
    return;
  network_sidebar_amneziawg_secret_free(prepared->original,
                                         prepared->original_length);
  network_sidebar_amneziawg_secret_free(prepared->runtime_snapshot,
                                         prepared->runtime_snapshot_length);
  network_sidebar_amneziawg_secret_free(prepared->claimed_snapshot,
                                         prepared->claimed_snapshot_length);
  network_sidebar_amneziawg_secret_free(prepared->dns_payload,
                                         prepared->dns_payload_length);
  network_sidebar_amneziawg_dns_config_clear(&prepared->dns);
  g_free(prepared);
}
