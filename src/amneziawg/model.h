#ifndef NETWORK_SIDEBAR_AMNEZIAWG_MODEL_H
#define NETWORK_SIDEBAR_AMNEZIAWG_MODEL_H

#include "amneziawg/admission.h"

#include <glib.h>

G_BEGIN_DECLS

typedef struct _NetworkSidebarAwgModel NetworkSidebarAwgModel;

typedef enum {
  NETWORK_SIDEBAR_AWG_ACTIVITY_IDLE,
  NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_SELECT,
  NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_LOAD,
  NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT,
  NETWORK_SIDEBAR_AWG_ACTIVITY_REPLACE_CONFIRM,
  NETWORK_SIDEBAR_AWG_ACTIVITY_REPLACE,
  NETWORK_SIDEBAR_AWG_ACTIVITY_UP,
  NETWORK_SIDEBAR_AWG_ACTIVITY_DOWN,
  NETWORK_SIDEBAR_AWG_ACTIVITY_DELETE_CONFIRM,
  NETWORK_SIDEBAR_AWG_ACTIVITY_DELETE,
} NetworkSidebarAwgActivity;

typedef enum {
  NETWORK_SIDEBAR_AWG_PROFILE_WARNING_NONE,
  NETWORK_SIDEBAR_AWG_PROFILE_WARNING_PARTIAL,
  NETWORK_SIDEBAR_AWG_PROFILE_WARNING_TIMED_OUT,
  NETWORK_SIDEBAR_AWG_PROFILE_WARNING_INVALID_REPLY,
  NETWORK_SIDEBAR_AWG_PROFILE_WARNING_HELPER_STATUS,
  NETWORK_SIDEBAR_AWG_PROFILE_WARNING_SERVICE_UNAVAILABLE,
  NETWORK_SIDEBAR_AWG_PROFILE_WARNING_SERVICE_BUSY,
  NETWORK_SIDEBAR_AWG_PROFILE_WARNING_SERVICE_FAILED,
} NetworkSidebarAwgProfileWarningKind;

typedef struct {
  NetworkSidebarAwgProfileWarningKind kind;
  guint helper_status;
  gboolean stale;
} NetworkSidebarAwgProfileWarning;

typedef struct {
  char *name;
  gboolean has_profile;
  NetworkSidebarAmneziaWGProfileStatus profile_status;
  NetworkSidebarAmneziaWGRuntimeState runtime_state;
  /* Presentation only: may retain the previous observation during a recheck.
   * Admission and action dispatch must use runtime_state instead. */
  NetworkSidebarAmneziaWGRuntimeState display_runtime_state;
  gboolean interface_exists;
  /* Includes previous sessions being disconnected by an Up switch. */
  gboolean pending;
  NetworkSidebarAwgActionAvailability primary_action;
  NetworkSidebarAwgActionAvailability remove_action;
} NetworkSidebarAwgEntry;

typedef struct {
  guint64 generation;
  gboolean authorized;
  gboolean capabilities_known;
  NetworkSidebarAwgDependency missing_dependencies;
  NetworkSidebarAwgActivity activity;
  char *activity_name;
  gboolean inventory_pending;
  /* Runtime observation progress is independent of saved-profile inventory. */
  gboolean runtime_pending;
  /* Section-wide loading excludes quiet post-operation reconciliation.
   * Pending state above still governs runtime presentation and admission. */
  gboolean inventory_loading;
  gboolean runtime_loading;
  NetworkSidebarAwgActionAvailability import_action;
  NetworkSidebarAwgProfileWarning profiles_warning;
  GPtrArray *entries;
} NetworkSidebarAwgSnapshot;

typedef struct {
  char *name;
  guint status;
} NetworkSidebarAwgProfileRecord;

typedef struct {
  char *name;
  NetworkSidebarAmneziaWGRuntimeState state;
  gboolean interface_exists;
} NetworkSidebarAwgRuntimeRecord;

NetworkSidebarAwgProfileRecord *network_sidebar_awg_profile_record_new(
  const char *name,
  guint status);
void network_sidebar_awg_profile_record_free(
  NetworkSidebarAwgProfileRecord *record);
NetworkSidebarAwgRuntimeRecord *network_sidebar_awg_runtime_record_new(
  const char *name,
  NetworkSidebarAmneziaWGRuntimeState state,
  gboolean interface_exists);
void network_sidebar_awg_runtime_record_free(
  NetworkSidebarAwgRuntimeRecord *record);

NetworkSidebarAwgModel *network_sidebar_awg_model_new(void);
void network_sidebar_awg_model_free(NetworkSidebarAwgModel *model);

gboolean network_sidebar_awg_model_replace_profiles(
  NetworkSidebarAwgModel *model,
  const GPtrArray *profiles);
/* Record a service-confirmed import/replacement until the next inventory. */
void network_sidebar_awg_model_record_import(
  NetworkSidebarAwgModel *model,
  const char *name);
/* Record a service-confirmed deletion until the next inventory. */
void network_sidebar_awg_model_record_delete(
  NetworkSidebarAwgModel *model,
  const char *name);
void network_sidebar_awg_model_reset_profiles(NetworkSidebarAwgModel *model);
gboolean network_sidebar_awg_model_has_profile_data(
  const NetworkSidebarAwgModel *model);
gboolean network_sidebar_awg_model_get_profile_status(
  const NetworkSidebarAwgModel *model,
  const char *name,
  NetworkSidebarAmneziaWGProfileStatus *status);
GPtrArray *network_sidebar_awg_model_dup_profile_names(
  const NetworkSidebarAwgModel *model);

void network_sidebar_awg_model_set_inventory_valid(
  NetworkSidebarAwgModel *model,
  gboolean valid);
void network_sidebar_awg_model_set_profiles_warning(
  NetworkSidebarAwgModel *model,
  NetworkSidebarAwgProfileWarningKind kind,
  guint helper_status);
void network_sidebar_awg_model_set_stale_profiles_warning(
  NetworkSidebarAwgModel *model,
  NetworkSidebarAwgProfileWarningKind kind,
  guint helper_status);

/* Capture existing sessions before beginning Up; rebuilds retain their
 * progress ownership until the activity ends or authorization is lost. */
void network_sidebar_awg_model_begin_switch(NetworkSidebarAwgModel *model);
void network_sidebar_awg_model_rebuild_entries(
  NetworkSidebarAwgModel *model,
  gboolean authorized,
  const GPtrArray *runtime_records,
  gboolean runtime_pending,
  NetworkSidebarAwgActivity activity,
  const char *activity_name);
NetworkSidebarAwgSnapshot *network_sidebar_awg_model_dup_snapshot(
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
  gboolean runtime_loading);
void network_sidebar_awg_snapshot_free(NetworkSidebarAwgSnapshot *snapshot);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarAwgModel,
                              network_sidebar_awg_model_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarAwgSnapshot,
                              network_sidebar_awg_snapshot_free)

G_END_DECLS

#endif
