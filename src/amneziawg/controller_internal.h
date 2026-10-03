#ifndef NETWORK_SIDEBAR_AMNEZIAWG_CONTROLLER_INTERNAL_H
#define NETWORK_SIDEBAR_AMNEZIAWG_CONTROLLER_INTERNAL_H

#include "amneziawg/backend.h"
#include "amneziawg/controller.h"

G_BEGIN_DECLS

typedef enum {
  NETWORK_SIDEBAR_AWG_UI_NOTICE,
  NETWORK_SIDEBAR_AWG_UI_SELECT_IMPORT,
  NETWORK_SIDEBAR_AWG_UI_CONFIRM_REPLACE,
  NETWORK_SIDEBAR_AWG_UI_CONFIRM_DELETE,
  NETWORK_SIDEBAR_AWG_UI_EXTERNAL_INTERACTION_ACTIVE,
  NETWORK_SIDEBAR_AWG_UI_DISMISS_ALL,
} NetworkSidebarAwgUiEventType;

typedef enum {
  NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE,
  NETWORK_SIDEBAR_AWG_IMPORT_ERROR_INSPECT_FAILED,
  NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NOT_LOCAL,
  NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NOT_REGULAR,
  NETWORK_SIDEBAR_AWG_IMPORT_ERROR_TOO_LARGE,
  NETWORK_SIDEBAR_AWG_IMPORT_ERROR_OPEN_FAILED,
  NETWORK_SIDEBAR_AWG_IMPORT_ERROR_READ_FAILED,
  NETWORK_SIDEBAR_AWG_IMPORT_ERROR_EMPTY,
  NETWORK_SIDEBAR_AWG_IMPORT_ERROR_TIMED_OUT,
} NetworkSidebarAwgImportError;

typedef enum {
  NETWORK_SIDEBAR_AWG_NOTICE_NONE,
  NETWORK_SIDEBAR_AWG_NOTICE_AUTHORIZATION_UNAVAILABLE,
  NETWORK_SIDEBAR_AWG_NOTICE_WAIT_FOR_RUNTIME_REFRESH,
  NETWORK_SIDEBAR_AWG_NOTICE_REQUIRED_COMPONENTS_UNAVAILABLE,
  NETWORK_SIDEBAR_AWG_NOTICE_IMPORT_CHOOSER_UNAVAILABLE,
  NETWORK_SIDEBAR_AWG_NOTICE_INVALID_IMPORT_FILENAME,
  NETWORK_SIDEBAR_AWG_NOTICE_IMPORT_CANCELLED,
  NETWORK_SIDEBAR_AWG_NOTICE_IMPORT_LOAD_FAILED,
  NETWORK_SIDEBAR_AWG_NOTICE_OPERATION_TIMED_OUT,
  NETWORK_SIDEBAR_AWG_NOTICE_SERVICE_CONNECT_FAILED,
  NETWORK_SIDEBAR_AWG_NOTICE_SERVICE_FAILED,
  NETWORK_SIDEBAR_AWG_NOTICE_SERVICE_BUSY,
  NETWORK_SIDEBAR_AWG_NOTICE_CONFIGURATION_UNAVAILABLE,
  NETWORK_SIDEBAR_AWG_NOTICE_EXTERNAL_OPERATION_IN_PROGRESS,
  NETWORK_SIDEBAR_AWG_NOTICE_OPERATION_SUCCEEDED,
  NETWORK_SIDEBAR_AWG_NOTICE_HELPER_FAILED,
  NETWORK_SIDEBAR_AWG_NOTICE_PROFILE_MISSING,
  NETWORK_SIDEBAR_AWG_NOTICE_PROFILE_ACTIVATION_BLOCKED,
  NETWORK_SIDEBAR_AWG_NOTICE_INTERFACE_EXTERNALLY_MANAGED,
  NETWORK_SIDEBAR_AWG_NOTICE_CLEANUP_UNCONFIRMED,
  NETWORK_SIDEBAR_AWG_NOTICE_PROFILE_REMOVAL_BLOCKED,
  NETWORK_SIDEBAR_AWG_NOTICE_OPERATION_STARTED,
  NETWORK_SIDEBAR_AWG_NOTICE_REPLACEMENT_CONFIRMATION_UNAVAILABLE,
  NETWORK_SIDEBAR_AWG_NOTICE_REMOVAL_CONFIRMATION_UNAVAILABLE,
} NetworkSidebarAwgNotice;

typedef struct {
  NetworkSidebarAwgUiEventType type;
  guint64 activity_id;
  NetworkSidebarAwgNotice notice;
  NetworkSidebarAwgBackendOperation operation;
  NetworkSidebarAwgImportError import_error;
  guint helper_status;
  /* The name is borrowed for the synchronous event callback. */
  const char *name;
  gboolean availability_probe;
} NetworkSidebarAwgUiEvent;

/* TRUE for an actual confirmation accepts immediate or deferred presentation.
 * An availability_probe is side-effect-free: it must not reserve or enqueue
 * an interaction. */
typedef gboolean (*NetworkSidebarAwgUiEventCallback)(
  const NetworkSidebarAwgUiEvent *event,
  gpointer user_data);

void network_sidebar_awg_controller_set_ui_event_callback(
  NetworkSidebarAwgController *controller,
  NetworkSidebarAwgUiEventCallback callback,
  gpointer user_data);

void network_sidebar_awg_controller_import(
  NetworkSidebarAwgController *controller);
/* Cancels selection/loading state, never an import sent to the service. */
void network_sidebar_awg_controller_cancel_import(
  NetworkSidebarAwgController *controller);
void network_sidebar_awg_controller_set_active(
  NetworkSidebarAwgController *controller,
  const char *name,
  gboolean active);
void network_sidebar_awg_controller_confirm_delete(
  NetworkSidebarAwgController *controller,
  const char *name);

gboolean network_sidebar_awg_controller_import_file_selected(
  NetworkSidebarAwgController *controller,
  guint64 activity_id,
  const char *basename);
void network_sidebar_awg_controller_import_selection_cancelled(
  NetworkSidebarAwgController *controller,
  guint64 activity_id);
void network_sidebar_awg_controller_import_loaded(
  NetworkSidebarAwgController *controller,
  guint64 activity_id,
  GBytes *config);
void network_sidebar_awg_controller_import_failed(
  NetworkSidebarAwgController *controller,
  guint64 activity_id,
  NetworkSidebarAwgImportError error);
/* Read-only: TRUE only for a current, authorized confirmation token. */
gboolean network_sidebar_awg_controller_confirmation_is_pending(
  NetworkSidebarAwgController *controller,
  NetworkSidebarAwgUiEventType confirmation,
  guint64 activity_id);
void network_sidebar_awg_controller_confirmation_response(
  NetworkSidebarAwgController *controller,
  NetworkSidebarAwgUiEventType confirmation,
  guint64 activity_id,
  gboolean confirmed);

G_END_DECLS

#endif
