#ifndef NETWORK_SIDEBAR_AMNEZIAWG_HELPER_STORAGE_H
#define NETWORK_SIDEBAR_AMNEZIAWG_HELPER_STORAGE_H

#include "amneziawg/runtime_journal.h"

#include <sys/stat.h>

typedef struct _AwgHelperStorage AwgHelperStorage;
typedef struct _AwgHelperStorageIterator AwgHelperStorageIterator;

#define AWG_HELPER_MAX_RUNTIME_SESSIONS 256u
#define AWG_HELPER_MAX_RUNTIME_DIRECTORY_ENTRIES 1024u

typedef enum {
  AWG_HELPER_ENTRY_MISSING,
  AWG_HELPER_ENTRY_VALID,
  AWG_HELPER_ENTRY_INVALID,
} AwgHelperEntryState;

typedef enum {
  AWG_HELPER_STORAGE_CONFIG,
  AWG_HELPER_STORAGE_STAGING,
  AWG_HELPER_STORAGE_RUNTIME,
  AWG_HELPER_STORAGE_RUNTIME_CONFIGS,
} AwgHelperStorageArea;

typedef enum {
  AWG_HELPER_STORAGE_ITERATOR_ENTRY,
  AWG_HELPER_STORAGE_ITERATOR_DONE,
  AWG_HELPER_STORAGE_ITERATOR_FAILED,
} AwgHelperStorageIteratorResult;

typedef struct {
  struct stat status;
} AwgHelperStorageIdentity;

/* Opens only runtime storage and holds the shared operation lock until free. */
AwgHelperStorage *awg_helper_storage_open_runtime(gint64 deadline);
/* Adds persistent profile/staging access to an already locked runtime context.
 * Failure leaves the runtime context and its lock usable. */
gboolean awg_helper_storage_open_profiles(AwgHelperStorage *storage);
void awg_helper_storage_free(AwgHelperStorage *storage);

gboolean awg_helper_storage_make_config_filename(const char *name,
                                                  char *filename,
                                                  gsize size);
gboolean awg_helper_storage_make_runtime_config_path(const char *name,
                                                      char *path,
                                                      gsize size);
gboolean awg_helper_storage_make_dns_filename(const char *name,
                                               char *filename,
                                               gsize size);

AwgHelperEntryState awg_helper_storage_secure_file_state(
  const AwgHelperStorage *storage,
  AwgHelperStorageArea area,
  const char *name,
  mode_t mode);
AwgHelperEntryState awg_helper_storage_config_security_state(
  const AwgHelperStorage *storage,
  const char *config_name);
gboolean awg_helper_storage_read_secure_file(
  const AwgHelperStorage *storage,
  AwgHelperStorageArea area,
  const char *name,
  mode_t mode,
  gsize maximum_size,
  guint8 **contents,
  gsize *length);
NetworkSidebarAmneziaWGHelperExit awg_helper_storage_read_config(
  const AwgHelperStorage *storage,
  const char *config_name,
  guint8 **contents,
  gsize *length,
  AwgHelperStorageIdentity *identity);

gboolean awg_helper_storage_write_staging_candidate(
  const AwgHelperStorage *storage,
  const guint8 *contents,
  gsize length,
  char *temp_name,
  gsize temp_name_size,
  gboolean *candidate_created);
gboolean awg_helper_storage_atomic_write(const AwgHelperStorage *storage,
                                         AwgHelperStorageArea area,
                                         const char *target,
                                         const guint8 *contents,
                                         gsize length,
                                         mode_t mode);
gboolean awg_helper_storage_remove_and_sync(const AwgHelperStorage *storage,
                                            AwgHelperStorageArea area,
                                            const char *name);
gboolean awg_helper_storage_unlink_staging(
  const AwgHelperStorage *storage,
  const char *name);
gboolean awg_helper_storage_rename_staging_noreplace(
  const AwgHelperStorage *storage,
  const char *temp_name,
  const char *config_name);
gboolean awg_helper_storage_exchange_staging_config(
  const AwgHelperStorage *storage,
  const char *temp_name,
  const char *config_name);
gboolean awg_helper_storage_sync_config_and_staging(
  const AwgHelperStorage *storage);
gboolean awg_helper_storage_sync_staging(const AwgHelperStorage *storage);

gboolean awg_helper_storage_get_config_identity(
  const AwgHelperStorage *storage,
  const char *config_name,
  AwgHelperStorageIdentity *identity);
gboolean awg_helper_storage_config_matches_identity(
  const AwgHelperStorage *storage,
  const char *config_name,
  const AwgHelperStorageIdentity *identity);
gboolean awg_helper_storage_displaced_matches_identity(
  const AwgHelperStorage *storage,
  const char *temp_name,
  const AwgHelperStorageIdentity *identity);

AwgHelperStorageIterator *awg_helper_storage_iterator_new(
  const AwgHelperStorage *storage,
  AwgHelperStorageArea area);
AwgHelperStorageIteratorResult awg_helper_storage_iterator_next(
  AwgHelperStorageIterator *iterator,
  const char **entry_name);
gboolean awg_helper_storage_iterator_free(
  AwgHelperStorageIterator *iterator);

/* Complete, sorted journal names from the locked runtime directory. Known
 * internal entries are excluded. Failure never returns a partial list. */
NetworkSidebarAmneziaWGHelperExit awg_helper_storage_list_runtime_names(
  const AwgHelperStorage *storage,
  gint64 deadline,
  GPtrArray **names);

AwgHelperEntryState awg_helper_storage_read_runtime_marker(
  const AwgHelperStorage *storage,
  const char *name,
  NetworkSidebarAmneziaWGRuntimeMarker *marker);
gboolean awg_helper_storage_write_runtime_marker(
  const AwgHelperStorage *storage,
  const char *name,
  const NetworkSidebarAmneziaWGRuntimeMarker *marker);
#endif
