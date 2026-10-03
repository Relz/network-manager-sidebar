#include "amneziawg/model.h"

#include <string.h>

typedef struct {
  char *name;
  NetworkSidebarAmneziaWGProfileStatus status;
} SavedProfile;

typedef struct {
  char *name;
  gboolean has_profile;
  NetworkSidebarAmneziaWGProfileStatus profile_status;
  NetworkSidebarAmneziaWGRuntimeState runtime_state;
  NetworkSidebarAmneziaWGRuntimeState display_runtime_state;
  gboolean interface_exists;
} StoredEntry;

struct _NetworkSidebarAwgModel {
  GPtrArray *profiles;
  GPtrArray *entries;
  GHashTable *switch_disconnect_names;
  NetworkSidebarAwgProfileWarning profiles_warning;
  gboolean inventory_valid;
};

static void
saved_profile_free(SavedProfile *profile)
{
  if (profile == NULL)
    return;
  g_free(profile->name);
  g_free(profile);
}

static void
stored_entry_free(StoredEntry *entry)
{
  if (entry == NULL)
    return;
  g_free(entry->name);
  g_free(entry);
}

static void
snapshot_entry_free(NetworkSidebarAwgEntry *entry)
{
  if (entry == NULL)
    return;
  g_free(entry->name);
  g_free(entry);
}

static gint
saved_profile_compare(gconstpointer left, gconstpointer right)
{
  const SavedProfile *left_profile = *(SavedProfile * const *) left;
  const SavedProfile *right_profile = *(SavedProfile * const *) right;

  return strcmp(left_profile->name, right_profile->name);
}

static gint
stored_entry_compare(gconstpointer left, gconstpointer right)
{
  const StoredEntry *left_entry = *(StoredEntry * const *) left;
  const StoredEntry *right_entry = *(StoredEntry * const *) right;
  gint folded = g_ascii_strcasecmp(left_entry->name, right_entry->name);

  return folded != 0 ? folded : strcmp(left_entry->name, right_entry->name);
}

static SavedProfile *
find_profile(const NetworkSidebarAwgModel *model, const char *name)
{
  for (guint i = 0; i < model->profiles->len; i++) {
    SavedProfile *profile = g_ptr_array_index(model->profiles, i);

    if (g_strcmp0(profile->name, name) == 0)
      return profile;
  }
  return NULL;
}

static const NetworkSidebarAwgRuntimeRecord *
find_runtime_record(const GPtrArray *records, const char *name)
{
  for (guint i = 0; records != NULL && i < records->len; i++) {
    const NetworkSidebarAwgRuntimeRecord *record =
      g_ptr_array_index((GPtrArray *) records, i);

    if (record != NULL && g_strcmp0(record->name, name) == 0)
      return record;
  }
  return NULL;
}

static const StoredEntry *
find_stored_entry(const NetworkSidebarAwgModel *model, const char *name)
{
  for (guint i = 0; i < model->entries->len; i++) {
    const StoredEntry *entry = g_ptr_array_index(model->entries, i);

    if (g_strcmp0(entry->name, name) == 0)
      return entry;
  }
  return NULL;
}

static gboolean
model_activity_has_named_entry(NetworkSidebarAwgActivity activity,
                               const char *activity_name)
{
  return activity_name != NULL &&
         activity != NETWORK_SIDEBAR_AWG_ACTIVITY_IDLE &&
         activity != NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_SELECT;
}

NetworkSidebarAwgProfileRecord *
network_sidebar_awg_profile_record_new(const char *name, guint status)
{
  NetworkSidebarAwgProfileRecord *record = g_new0(
    NetworkSidebarAwgProfileRecord, 1);

  record->name = g_strdup(name);
  record->status = status;
  return record;
}

void
network_sidebar_awg_profile_record_free(NetworkSidebarAwgProfileRecord *record)
{
  if (record == NULL)
    return;
  g_free(record->name);
  g_free(record);
}

NetworkSidebarAwgRuntimeRecord *
network_sidebar_awg_runtime_record_new(
  const char *name,
  NetworkSidebarAmneziaWGRuntimeState state,
  gboolean interface_exists)
{
  NetworkSidebarAwgRuntimeRecord *record = g_new0(
    NetworkSidebarAwgRuntimeRecord, 1);

  record->name = g_strdup(name);
  record->state = state;
  record->interface_exists = interface_exists;
  return record;
}

void
network_sidebar_awg_runtime_record_free(NetworkSidebarAwgRuntimeRecord *record)
{
  if (record == NULL)
    return;
  g_free(record->name);
  g_free(record);
}

NetworkSidebarAwgModel *
network_sidebar_awg_model_new(void)
{
  NetworkSidebarAwgModel *model = g_new0(NetworkSidebarAwgModel, 1);

  model->profiles = g_ptr_array_new_with_free_func(
    (GDestroyNotify) saved_profile_free);
  model->entries = g_ptr_array_new_with_free_func(
    (GDestroyNotify) stored_entry_free);
  model->switch_disconnect_names = g_hash_table_new_full(
    g_str_hash, g_str_equal, g_free, NULL);
  return model;
}

void
network_sidebar_awg_model_free(NetworkSidebarAwgModel *model)
{
  if (model == NULL)
    return;
  g_clear_pointer(&model->profiles, g_ptr_array_unref);
  g_clear_pointer(&model->entries, g_ptr_array_unref);
  g_hash_table_unref(model->switch_disconnect_names);
  g_free(model);
}

gboolean
network_sidebar_awg_model_replace_profiles(NetworkSidebarAwgModel *model,
                                            const GPtrArray *profiles)
{
  g_autoptr(GPtrArray) replacement = g_ptr_array_new_with_free_func(
    (GDestroyNotify) saved_profile_free);
  g_autoptr(GHashTable) names = g_hash_table_new_full(g_str_hash,
                                                       g_str_equal,
                                                       g_free,
                                                       NULL);

  if (model == NULL || profiles == NULL ||
      profiles->len > NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_REPORT_MAX_RECORDS)
    return FALSE;

  for (guint i = 0; i < profiles->len; i++) {
    const NetworkSidebarAwgProfileRecord *record =
      g_ptr_array_index((GPtrArray *) profiles, i);
    SavedProfile *profile;

    if (record == NULL || !network_sidebar_amneziawg_name_is_valid(record->name) ||
        !network_sidebar_amneziawg_profile_status_is_valid(record->status) ||
        g_hash_table_contains(names, record->name))
      return FALSE;
    g_hash_table_add(names, g_strdup(record->name));
    profile = g_new0(SavedProfile, 1);
    profile->name = g_strdup(record->name);
    profile->status = record->status;
    g_ptr_array_add(replacement, profile);
  }

  g_ptr_array_sort(replacement, saved_profile_compare);
  g_clear_pointer(&model->profiles, g_ptr_array_unref);
  model->profiles = g_steal_pointer(&replacement);
  return TRUE;
}

void
network_sidebar_awg_model_record_import(NetworkSidebarAwgModel *model,
                                        const char *name)
{
  SavedProfile *profile;

  if (model == NULL || !network_sidebar_amneziawg_name_is_valid(name))
    return;
  profile = find_profile(model, name);
  if (profile == NULL) {
    profile = g_new0(SavedProfile, 1);
    profile->name = g_strdup(name);
    g_ptr_array_add(model->profiles, profile);
    g_ptr_array_sort(model->profiles, saved_profile_compare);
  }
  profile->status = NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_AVAILABLE;
}

void
network_sidebar_awg_model_record_delete(NetworkSidebarAwgModel *model,
                                        const char *name)
{
  if (model == NULL || !network_sidebar_amneziawg_name_is_valid(name))
    return;
  for (guint i = 0; i < model->profiles->len; i++) {
    const SavedProfile *profile = g_ptr_array_index(model->profiles, i);

    if (g_strcmp0(profile->name, name) == 0) {
      g_ptr_array_remove_index(model->profiles, i);
      return;
    }
  }
}

void
network_sidebar_awg_model_reset_profiles(NetworkSidebarAwgModel *model)
{
  if (model == NULL)
    return;
  g_ptr_array_set_size(model->profiles, 0);
  model->profiles_warning = (NetworkSidebarAwgProfileWarning) { 0 };
  model->inventory_valid = FALSE;
}

gboolean
network_sidebar_awg_model_has_profile_data(const NetworkSidebarAwgModel *model)
{
  return model != NULL &&
         (model->profiles->len != 0 ||
          model->profiles_warning.kind !=
            NETWORK_SIDEBAR_AWG_PROFILE_WARNING_NONE ||
          model->inventory_valid);
}

gboolean
network_sidebar_awg_model_get_profile_status(
  const NetworkSidebarAwgModel *model,
  const char *name,
  NetworkSidebarAmneziaWGProfileStatus *status)
{
  SavedProfile *profile;

  if (model == NULL)
    return FALSE;
  profile = find_profile(model, name);
  if (profile == NULL)
    return FALSE;
  if (status != NULL)
    *status = profile->status;
  return TRUE;
}

GPtrArray *
network_sidebar_awg_model_dup_profile_names(const NetworkSidebarAwgModel *model)
{
  GPtrArray *names = g_ptr_array_new_with_free_func(g_free);

  if (model == NULL)
    return names;
  for (guint i = 0; i < model->profiles->len; i++) {
    const SavedProfile *profile = g_ptr_array_index(model->profiles, i);

    g_ptr_array_add(names, g_strdup(profile->name));
  }
  return names;
}

void
network_sidebar_awg_model_set_inventory_valid(NetworkSidebarAwgModel *model,
                                               gboolean valid)
{
  if (model != NULL)
    model->inventory_valid = valid;
}

void
network_sidebar_awg_model_set_profiles_warning(NetworkSidebarAwgModel *model,
                                               NetworkSidebarAwgProfileWarningKind kind,
                                               guint helper_status)
{
  NetworkSidebarAwgProfileWarning warning = {
    .kind = kind,
    .helper_status = kind ==
        NETWORK_SIDEBAR_AWG_PROFILE_WARNING_HELPER_STATUS ?
      helper_status : 0,
  };

  if (model == NULL ||
      (model->profiles_warning.kind == warning.kind &&
       model->profiles_warning.helper_status == warning.helper_status &&
       !model->profiles_warning.stale))
    return;
  model->profiles_warning = warning;
}

void
network_sidebar_awg_model_set_stale_profiles_warning(
  NetworkSidebarAwgModel *model,
  NetworkSidebarAwgProfileWarningKind kind,
  guint helper_status)
{
  NetworkSidebarAwgProfileWarning warning = {
    .kind = kind,
    .helper_status = kind ==
        NETWORK_SIDEBAR_AWG_PROFILE_WARNING_HELPER_STATUS ?
      helper_status : 0,
  };

  if (model == NULL)
    return;
  warning.stale = model->inventory_valid;
  model->profiles_warning = warning;
}

void
network_sidebar_awg_model_begin_switch(NetworkSidebarAwgModel *model)
{
  if (model == NULL)
    return;
  g_hash_table_remove_all(model->switch_disconnect_names);
  for (guint i = 0; i < model->entries->len; i++) {
    const StoredEntry *entry = g_ptr_array_index(model->entries, i);
    NetworkSidebarAmneziaWGRuntimeState state = entry->display_runtime_state;

    /* Up owns cleanup of existing sessions as well as activation of its
     * target. Capture presentation ownership before scans can invalidate or
     * replace these observations with intermediate cleanup checkpoints. */
    if (state == NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_ACTIVE ||
        network_sidebar_amneziawg_runtime_state_is_recoverable(state))
      g_hash_table_add(model->switch_disconnect_names, g_strdup(entry->name));
  }
}

void
network_sidebar_awg_model_rebuild_entries(
  NetworkSidebarAwgModel *model,
  gboolean authorized,
  const GPtrArray *runtime_records,
  gboolean runtime_pending,
  NetworkSidebarAwgActivity activity,
  const char *activity_name)
{
  g_autoptr(GPtrArray) replacement = g_ptr_array_new_with_free_func(
    (GDestroyNotify) stored_entry_free);
  g_autoptr(GHashTable) names = g_hash_table_new_full(g_str_hash,
                                                       g_str_equal,
                                                       g_free,
                                                       NULL);
  GHashTableIter iter;
  gpointer key;
  gboolean named_activity = model_activity_has_named_entry(activity,
                                                            activity_name);

  if (model == NULL)
    return;
  if (!authorized || activity != NETWORK_SIDEBAR_AWG_ACTIVITY_UP)
    g_hash_table_remove_all(model->switch_disconnect_names);
  if (!authorized) {
    g_clear_pointer(&model->entries, g_ptr_array_unref);
    model->entries = g_steal_pointer(&replacement);
    return;
  }

  for (guint i = 0; i < model->profiles->len; i++) {
    SavedProfile *profile = g_ptr_array_index(model->profiles, i);

    g_hash_table_add(names, g_strdup(profile->name));
  }
  for (guint i = 0; runtime_records != NULL && i < runtime_records->len; i++) {
    const NetworkSidebarAwgRuntimeRecord *record =
      g_ptr_array_index((GPtrArray *) runtime_records, i);

    if (record != NULL && network_sidebar_amneziawg_name_is_valid(record->name))
      g_hash_table_add(names, g_strdup(record->name));
  }
  if (named_activity)
    g_hash_table_add(names, g_strdup(activity_name));
  g_hash_table_iter_init(&iter, model->switch_disconnect_names);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    g_hash_table_add(names, g_strdup(key));

  g_hash_table_iter_init(&iter, names);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    const char *name = key;
    SavedProfile *profile = find_profile(model, name);
    const NetworkSidebarAwgRuntimeRecord *runtime = find_runtime_record(
      runtime_records, name);
    NetworkSidebarAmneziaWGRuntimeState runtime_state = runtime != NULL ?
      runtime->state : NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_UNKNOWN;
    const StoredEntry *previous = find_stored_entry(model, name);
    gboolean activity_entry = (named_activity &&
      g_strcmp0(activity_name, name) == 0) ||
      g_hash_table_contains(model->switch_disconnect_names, name);
    StoredEntry *entry;

    if (profile == NULL &&
        runtime_state == NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_INACTIVE &&
        !activity_entry)
      continue;
    entry = g_new0(StoredEntry, 1);
    entry->name = g_strdup(name);
    entry->has_profile = profile != NULL;
    entry->profile_status = profile != NULL ? profile->status :
      NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_CHANGED_OR_UNREADABLE;
    entry->runtime_state = runtime_state;
    /* Invalidation must still block actions immediately, but it need not
     * replace every row's presentation with a transient checking message.
     * Dropping the retained state when checking ends also exposes failures. */
    entry->display_runtime_state = runtime_pending && previous != NULL &&
      runtime_state == NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_UNKNOWN ?
      previous->display_runtime_state : runtime_state;
    entry->interface_exists = runtime != NULL && runtime->interface_exists;
    g_ptr_array_add(replacement, entry);
  }

  g_ptr_array_sort(replacement, stored_entry_compare);
  g_clear_pointer(&model->entries, g_ptr_array_unref);
  model->entries = g_steal_pointer(&replacement);
}

NetworkSidebarAwgSnapshot *
network_sidebar_awg_model_dup_snapshot(
  const NetworkSidebarAwgModel *model,
  guint64 generation,
  gboolean authorized,
  gboolean capabilities_known,
  NetworkSidebarAwgDependency missing_dependencies,
  NetworkSidebarAwgActivity activity,
  const char *activity_name,
  gboolean inventory_pending,
  gboolean runtime_pending,
  gboolean inventory_loading,
  gboolean runtime_loading)
{
  NetworkSidebarAwgSnapshot *snapshot;
  const NetworkSidebarAwgAdmissionContext admission = {
    .authorized = authorized,
    .busy = activity != NETWORK_SIDEBAR_AWG_ACTIVITY_IDLE,
    .capabilities_known = capabilities_known,
    .missing_dependencies = missing_dependencies,
  };

  if (model == NULL)
    return NULL;
  snapshot = g_new0(NetworkSidebarAwgSnapshot, 1);
  snapshot->generation = generation;
  snapshot->authorized = authorized;
  snapshot->capabilities_known = capabilities_known;
  snapshot->missing_dependencies = missing_dependencies;
  snapshot->activity = activity;
  snapshot->activity_name = g_strdup(activity_name);
  snapshot->inventory_pending = inventory_pending;
  snapshot->runtime_pending = runtime_pending;
  snapshot->inventory_loading = inventory_loading;
  snapshot->runtime_loading = runtime_loading;
  snapshot->import_action = network_sidebar_awg_action_evaluate(
    NETWORK_SIDEBAR_AWG_ACTION_IMPORT, &admission, NULL);
  snapshot->profiles_warning = model->profiles_warning;
  snapshot->entries = g_ptr_array_new_with_free_func(
    (GDestroyNotify) snapshot_entry_free);

  for (guint i = 0; i < model->entries->len; i++) {
    const StoredEntry *stored = g_ptr_array_index(model->entries, i);
    NetworkSidebarAwgEntry *entry = g_new0(NetworkSidebarAwgEntry, 1);
    const NetworkSidebarAwgAdmissionTarget target = {
      .has_profile = stored->has_profile,
      .profile_status = stored->profile_status,
      .runtime_state = stored->runtime_state,
      .interface_exists = stored->interface_exists,
    };

    entry->name = g_strdup(stored->name);
    entry->has_profile = stored->has_profile;
    entry->profile_status = stored->profile_status;
    entry->runtime_state = stored->runtime_state;
    entry->display_runtime_state = runtime_pending ?
      stored->display_runtime_state : stored->runtime_state;
    entry->interface_exists = stored->interface_exists;
    entry->pending = activity != NETWORK_SIDEBAR_AWG_ACTIVITY_IDLE &&
                     ((activity_name != NULL &&
                       g_strcmp0(activity_name, stored->name) == 0) ||
                      (activity == NETWORK_SIDEBAR_AWG_ACTIVITY_UP &&
                       g_hash_table_contains(model->switch_disconnect_names,
                                             stored->name)));
    entry->primary_action = network_sidebar_awg_action_evaluate(
      network_sidebar_awg_primary_action(stored->runtime_state), &admission, &target);
    entry->remove_action = network_sidebar_awg_action_evaluate(
      NETWORK_SIDEBAR_AWG_ACTION_DELETE, &admission, &target);
    g_ptr_array_add(snapshot->entries, entry);
  }
  return snapshot;
}

void
network_sidebar_awg_snapshot_free(NetworkSidebarAwgSnapshot *snapshot)
{
  if (snapshot == NULL)
    return;
  g_free(snapshot->activity_name);
  g_clear_pointer(&snapshot->entries, g_ptr_array_unref);
  g_free(snapshot);
}
