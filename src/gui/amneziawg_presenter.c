#include "gui/amneziawg_presenter.h"

#include "amneziawg/controller_internal.h"
#include "amneziawg/import_loader.h"
#include "gui/amneziawg_messages.h"

#define AWG_MAX_DEFERRED_NOTICES 16
#define AWG_OMITTED_NOTICE "Some earlier AmneziaWG notifications were omitted"

typedef struct {
  NetworkSidebarAwgPresenter *presenter;
  guint64 activity_id;
} ChooserResponseData;

typedef struct {
  NetworkSidebarAwgPresenter *presenter;
  guint64 activity_id;
} ImportLoadData;

typedef struct {
  NetworkSidebarAwgPresenter *presenter;
  NetworkSidebarAwgUiEventType type;
  guint64 activity_id;
} ConfirmationData;

typedef struct {
  NetworkSidebarAwgUiEventType type;
  guint64 activity_id;
  char *name;
} DeferredConfirmation;

struct _NetworkSidebarAwgPresenter {
  gint ref_count;
  NetworkSidebarAwgController *controller;
  NetworkSidebarAwgChangedCallback changed;
  NetworkSidebarAwgExternalInteractionCallback external_interaction;
  gpointer user_data;

  GWeakRef parent;
  GWeakRef toast_overlay;
  GCancellable *lifecycle_cancellable;
  GtkNativeDialog *chooser;
  gulong chooser_response_handler;
  AdwDialog *dialog;
  DeferredConfirmation *pending_confirmation;
  GQueue notices;
  AdwToast *toast;
  gboolean toast_is_progress;
  gboolean notices_omitted;
  guint flush_source;
  NetworkSidebarAwgImportLoader *import_loader;

  gboolean external_interaction_active;
  gboolean started;
  gboolean stopping;
};

static void dismiss_interactions(NetworkSidebarAwgPresenter *presenter);
static void schedule_flush(NetworkSidebarAwgPresenter *presenter);

NetworkSidebarAwgPresenter *
network_sidebar_awg_presenter_ref(NetworkSidebarAwgPresenter *presenter)
{
  if (presenter == NULL)
    return NULL;
  g_atomic_int_inc(&presenter->ref_count);
  return presenter;
}

static void
set_external_interaction(NetworkSidebarAwgPresenter *presenter,
                         gboolean active,
                         gboolean notify)
{
  NetworkSidebarAwgExternalInteractionCallback callback;
  gpointer user_data;

  if (presenter->external_interaction_active == active)
    return;
  presenter->external_interaction_active = active;
  if (!notify)
    return;
  callback = presenter->external_interaction;
  user_data = presenter->user_data;
  if (callback == NULL)
    return;

  network_sidebar_awg_presenter_ref(presenter);
  callback(active, user_data);
  network_sidebar_awg_presenter_unref(presenter);
}

static void
forward_changed(guint delay_ms, gpointer user_data)
{
  NetworkSidebarAwgPresenter *presenter = user_data;
  NetworkSidebarAwgChangedCallback callback;
  gpointer callback_data;

  if (presenter->stopping)
    return;
  callback = presenter->changed;
  callback_data = presenter->user_data;

  network_sidebar_awg_presenter_ref(presenter);
  schedule_flush(presenter);
  if (callback != NULL)
    callback(delay_ms, callback_data);
  network_sidebar_awg_presenter_unref(presenter);
}

static gboolean
can_accept_interaction(NetworkSidebarAwgPresenter *presenter)
{
  g_autoptr(GObject) parent = g_weak_ref_get(&presenter->parent);

  return presenter->started && !presenter->stopping && parent != NULL &&
         presenter->chooser == NULL && presenter->dialog == NULL &&
         presenter->pending_confirmation == NULL;
}

static gboolean
presentation_is_mapped(NetworkSidebarAwgPresenter *presenter)
{
  g_autoptr(GObject) parent = g_weak_ref_get(&presenter->parent);
  g_autoptr(GObject) overlay = g_weak_ref_get(&presenter->toast_overlay);

  return presenter->started && !presenter->stopping && parent != NULL &&
         overlay != NULL && gtk_widget_get_mapped(GTK_WIDGET(parent)) &&
         gtk_widget_get_mapped(GTK_WIDGET(overlay));
}

static void
clear_confirmation(NetworkSidebarAwgPresenter *presenter)
{
  if (presenter->pending_confirmation != NULL) {
    g_free(presenter->pending_confirmation->name);
    g_clear_pointer(&presenter->pending_confirmation, g_free);
  }
  if (presenter->dialog != NULL) {
    AdwDialog *dialog = g_steal_pointer(&presenter->dialog);

    adw_dialog_force_close(dialog);
    g_object_unref(dialog);
  }
}

static void
withdraw_toast(NetworkSidebarAwgPresenter *presenter, gboolean retain)
{
  AdwToast *toast = g_steal_pointer(&presenter->toast);

  if (toast == NULL)
    return;
  if (retain && g_strcmp0(adw_toast_get_title(toast), AWG_OMITTED_NOTICE) == 0) {
    presenter->notices_omitted = TRUE;
  } else if (retain && !presenter->toast_is_progress) {
    if (g_queue_get_length(&presenter->notices) == AWG_MAX_DEFERRED_NOTICES) {
      g_free(g_queue_pop_head(&presenter->notices));
      presenter->notices_omitted = TRUE;
    }
    g_queue_push_head(&presenter->notices, g_strdup(adw_toast_get_title(toast)));
  }
  presenter->toast_is_progress = FALSE;
  g_signal_handlers_disconnect_by_data(toast, presenter);
  adw_toast_dismiss(toast);
  g_object_unref(toast);
}

static void
toast_dismissed_cb(AdwToast *toast, gpointer user_data)
{
  g_autoptr(NetworkSidebarAwgPresenter) presenter =
    network_sidebar_awg_presenter_ref(user_data);

  if (presenter->toast != toast)
    return;
  g_signal_handlers_disconnect_by_data(toast, presenter);
  g_clear_object(&presenter->toast);
  presenter->toast_is_progress = FALSE;
  schedule_flush(presenter);
}

static void
present_toast(NetworkSidebarAwgPresenter *presenter,
              const char *message,
              gboolean progress)
{
  g_autoptr(GObject) overlay = g_weak_ref_get(&presenter->toast_overlay);

  presenter->toast = adw_toast_new(message);
  presenter->toast_is_progress = progress;
  g_signal_connect(presenter->toast, "dismissed",
                   G_CALLBACK(toast_dismissed_cb), presenter);
  adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(overlay),
                              g_object_ref(presenter->toast));
}

static void
show_notice(NetworkSidebarAwgPresenter *presenter,
            const NetworkSidebarAwgUiEvent *event)
{
  g_autoptr(NetworkSidebarAwgSnapshot) snapshot = NULL;
  g_autofree char *message = NULL;
  NetworkSidebarAwgUiEvent safe_event = *event;
  gboolean progress = event->notice == NETWORK_SIDEBAR_AWG_NOTICE_OPERATION_STARTED;

  if (presenter->stopping || event->notice == NETWORK_SIDEBAR_AWG_NOTICE_NONE)
    return;
  if (progress && (!presentation_is_mapped(presenter) ||
                   presenter->chooser != NULL || presenter->dialog != NULL ||
                   presenter->pending_confirmation != NULL ||
                   presenter->toast != NULL || !g_queue_is_empty(&presenter->notices)))
    return;
  snapshot = network_sidebar_awg_controller_dup_snapshot(presenter->controller);
  if (snapshot == NULL || !snapshot->authorized) {
    safe_event.notice = NETWORK_SIDEBAR_AWG_NOTICE_AUTHORIZATION_UNAVAILABLE;
    safe_event.name = NULL;
    progress = FALSE;
  }
  message = network_sidebar_awg_message_for_notice(&safe_event);
  if (message == NULL)
    return;
  if (progress) {
    present_toast(presenter, message, TRUE);
    return;
  }
  if ((presenter->toast != NULL &&
       g_strcmp0(adw_toast_get_title(presenter->toast), message) == 0) ||
      g_queue_find_custom(&presenter->notices, message,
                          (GCompareFunc) g_strcmp0) != NULL)
    return;
  if (g_queue_get_length(&presenter->notices) == AWG_MAX_DEFERRED_NOTICES) {
    g_free(g_queue_pop_head(&presenter->notices));
    presenter->notices_omitted = TRUE;
  }
  g_queue_push_tail(&presenter->notices, g_steal_pointer(&message));
  if (presenter->toast_is_progress)
    withdraw_toast(presenter, FALSE);
  schedule_flush(presenter);
}

static void
chooser_response_data_free(gpointer data, GClosure *closure)
{
  ChooserResponseData *response_data = data;
  (void) closure;

  network_sidebar_awg_presenter_unref(response_data->presenter);
  g_free(response_data);
}

static void
destroy_chooser(NetworkSidebarAwgPresenter *presenter)
{
  GtkNativeDialog *chooser = g_steal_pointer(&presenter->chooser);
  gulong handler = presenter->chooser_response_handler;

  presenter->chooser_response_handler = 0;
  if (chooser == NULL)
    return;
  if (handler != 0 && g_signal_handler_is_connected(chooser, handler))
    g_signal_handler_disconnect(chooser, handler);
  gtk_native_dialog_destroy(chooser);
  g_object_unref(chooser);
}

static NetworkSidebarAwgImportError
controller_import_error(NetworkSidebarAwgImportLoadResult result)
{
  switch (result) {
  case NETWORK_SIDEBAR_AWG_IMPORT_LOAD_INSPECT_FAILED:
    return NETWORK_SIDEBAR_AWG_IMPORT_ERROR_INSPECT_FAILED;
  case NETWORK_SIDEBAR_AWG_IMPORT_LOAD_NOT_LOCAL:
    return NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NOT_LOCAL;
  case NETWORK_SIDEBAR_AWG_IMPORT_LOAD_NOT_REGULAR:
    return NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NOT_REGULAR;
  case NETWORK_SIDEBAR_AWG_IMPORT_LOAD_TOO_LARGE:
    return NETWORK_SIDEBAR_AWG_IMPORT_ERROR_TOO_LARGE;
  case NETWORK_SIDEBAR_AWG_IMPORT_LOAD_OPEN_FAILED:
    return NETWORK_SIDEBAR_AWG_IMPORT_ERROR_OPEN_FAILED;
  case NETWORK_SIDEBAR_AWG_IMPORT_LOAD_READ_FAILED:
    return NETWORK_SIDEBAR_AWG_IMPORT_ERROR_READ_FAILED;
  case NETWORK_SIDEBAR_AWG_IMPORT_LOAD_EMPTY:
    return NETWORK_SIDEBAR_AWG_IMPORT_ERROR_EMPTY;
  case NETWORK_SIDEBAR_AWG_IMPORT_LOAD_TIMED_OUT:
    return NETWORK_SIDEBAR_AWG_IMPORT_ERROR_TIMED_OUT;
  case NETWORK_SIDEBAR_AWG_IMPORT_LOAD_SUCCESS:
  case NETWORK_SIDEBAR_AWG_IMPORT_LOAD_CANCELLED:
  default:
    return NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE;
  }
}

static void
import_load_cb(NetworkSidebarAwgImportLoader *loader,
               NetworkSidebarAwgImportLoadResult result,
               GBytes *config,
               gpointer user_data)
{
  ImportLoadData *load_data = user_data;
  NetworkSidebarAwgPresenter *presenter = load_data->presenter;
  gboolean owned = presenter->import_loader == loader;

  if (owned)
    presenter->import_loader = NULL;
  if (!presenter->stopping) {
    if (result == NETWORK_SIDEBAR_AWG_IMPORT_LOAD_SUCCESS) {
      network_sidebar_awg_controller_import_loaded(presenter->controller,
                                                   load_data->activity_id,
                                                   config);
    } else if (result != NETWORK_SIDEBAR_AWG_IMPORT_LOAD_CANCELLED) {
      network_sidebar_awg_controller_import_failed(
        presenter->controller,
        load_data->activity_id,
        controller_import_error(result));
    }
  }
  if (owned)
    network_sidebar_awg_import_loader_unref(loader);
  network_sidebar_awg_presenter_unref(presenter);
  g_free(load_data);
}

static void
start_import_load(NetworkSidebarAwgPresenter *presenter,
                  guint64 activity_id,
                  GFile *file)
{
  ImportLoadData *load_data;

  if (presenter->stopping || presenter->import_loader != NULL)
    return;
  load_data = g_new0(ImportLoadData, 1);
  load_data->presenter = network_sidebar_awg_presenter_ref(presenter);
  load_data->activity_id = activity_id;
  presenter->import_loader = network_sidebar_awg_import_loader_new(
    file, import_load_cb, load_data);
  if (presenter->import_loader == NULL) {
    network_sidebar_awg_presenter_unref(load_data->presenter);
    g_free(load_data);
    network_sidebar_awg_controller_import_failed(
      presenter->controller,
      activity_id,
      NETWORK_SIDEBAR_AWG_IMPORT_ERROR_OPEN_FAILED);
    return;
  }
  network_sidebar_awg_import_loader_start(presenter->import_loader);
}

static void
file_chooser_response_cb(GtkNativeDialog *dialog,
                         int response,
                         gpointer user_data)
{
  ChooserResponseData *response_data = user_data;
  g_autoptr(NetworkSidebarAwgPresenter) presenter =
    network_sidebar_awg_presenter_ref(response_data->presenter);
  g_autoptr(GFile) file = NULL;
  g_autofree char *basename = NULL;
  guint64 activity_id = response_data->activity_id;

  if (response == GTK_RESPONSE_ACCEPT)
    file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
  destroy_chooser(presenter);
  set_external_interaction(presenter, FALSE, TRUE);

  if (presenter->stopping)
    return;
  if (file == NULL) {
    network_sidebar_awg_controller_import_selection_cancelled(
      presenter->controller, activity_id);
    return;
  }
  basename = g_file_get_basename(file);
  if (network_sidebar_awg_controller_import_file_selected(
        presenter->controller, activity_id, basename))
    start_import_load(presenter, activity_id, file);
}

static gboolean
present_import_chooser(NetworkSidebarAwgPresenter *presenter,
                       guint64 activity_id)
{
  GtkFileChooserNative *chooser;
  GtkFileFilter *filter;
  ChooserResponseData *response_data;

  if (!can_accept_interaction(presenter))
    return FALSE;
  chooser = gtk_file_chooser_native_new("Import AmneziaWG Configuration",
                                        NULL,
                                        GTK_FILE_CHOOSER_ACTION_OPEN,
                                        "Import",
                                        "Cancel");
  if (chooser == NULL)
    return FALSE;
  gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(chooser), FALSE);
  filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "AmneziaWG configurations (*.conf)");
  gtk_file_filter_add_pattern(filter, "*.conf");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
  gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(chooser), filter);
  g_object_unref(filter);

  presenter->chooser = GTK_NATIVE_DIALOG(chooser);
  response_data = g_new0(ChooserResponseData, 1);
  response_data->presenter = network_sidebar_awg_presenter_ref(presenter);
  response_data->activity_id = activity_id;
  presenter->chooser_response_handler = g_signal_connect_data(
    chooser,
    "response",
    G_CALLBACK(file_chooser_response_cb),
    response_data,
    chooser_response_data_free,
    0);
  set_external_interaction(presenter, TRUE, TRUE);
  if (presenter->stopping || presenter->chooser == NULL) {
    destroy_chooser(presenter);
    set_external_interaction(presenter, FALSE, TRUE);
    return FALSE;
  }
  gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
  return TRUE;
}

static void
on_dialog_backdrop_released(GtkGestureClick *gesture,
                            int n_press,
                            double x,
                            double y,
                            gpointer user_data)
{
  AdwDialog *dialog = user_data;
  GtkWidget *picked;
  (void) n_press;

  picked = gtk_widget_pick(GTK_WIDGET(dialog), x, y, GTK_PICK_DEFAULT);
  if (picked != NULL &&
      g_strcmp0(gtk_widget_get_css_name(picked), "dimming") == 0) {
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    adw_dialog_close(dialog);
  }
}

static void
close_dialog_on_backdrop_click(AdwDialog *dialog)
{
  GtkGesture *backdrop_click = gtk_gesture_click_new();

  gtk_event_controller_set_propagation_phase(
    GTK_EVENT_CONTROLLER(backdrop_click), GTK_PHASE_CAPTURE);
  g_signal_connect(backdrop_click,
                   "released",
                   G_CALLBACK(on_dialog_backdrop_released),
                   dialog);
  gtk_widget_add_controller(GTK_WIDGET(dialog),
                            GTK_EVENT_CONTROLLER(backdrop_click));
}

static void
confirmation_chosen_cb(GObject *source,
                       GAsyncResult *result,
                       gpointer user_data)
{
  ConfirmationData *confirmation = user_data;
  NetworkSidebarAwgPresenter *presenter = confirmation->presenter;
  const char *response = adw_alert_dialog_choose_finish(
    ADW_ALERT_DIALOG(source), result);
  gboolean confirmed = confirmation->type ==
      NETWORK_SIDEBAR_AWG_UI_CONFIRM_REPLACE ?
    g_strcmp0(response, "replace") == 0 :
    g_strcmp0(response, "remove") == 0;

  if (!presenter->stopping && presenter->dialog != NULL &&
      G_OBJECT(presenter->dialog) == source &&
      presenter->pending_confirmation != NULL &&
      presenter->pending_confirmation->type == confirmation->type &&
      presenter->pending_confirmation->activity_id == confirmation->activity_id) {
    g_clear_object(&presenter->dialog);
    clear_confirmation(presenter);
    if (network_sidebar_awg_controller_confirmation_is_pending(
          presenter->controller, confirmation->type, confirmation->activity_id))
      network_sidebar_awg_controller_confirmation_response(
        presenter->controller,
        confirmation->type,
        confirmation->activity_id,
        confirmed);
    schedule_flush(presenter);
  }
  network_sidebar_awg_presenter_unref(presenter);
  g_free(confirmation);
}

static void
present_confirmation(NetworkSidebarAwgPresenter *presenter)
{
  g_autoptr(GObject) parent = NULL;
  g_autofree char *heading = NULL;
  const DeferredConfirmation *pending = presenter->pending_confirmation;
  const char *body;
  const char *action_id;
  const char *action_label;
  AdwDialog *dialog;
  ConfirmationData *confirmation;

  parent = g_weak_ref_get(&presenter->parent);
  heading = g_strdup_printf(
    pending->type == NETWORK_SIDEBAR_AWG_UI_CONFIRM_REPLACE ? "Replace %s?" :
                                                              "Remove %s?",
    pending->name);
  if (pending->type == NETWORK_SIDEBAR_AWG_UI_CONFIRM_REPLACE) {
    body = "A tunnel with this name already exists. Its configuration will be replaced.";
    action_id = "replace";
    action_label = "Replace";
  } else {
    g_autoptr(NetworkSidebarAwgSnapshot) snapshot =
      network_sidebar_awg_controller_dup_snapshot(presenter->controller);

    body = "This permanently removes the saved AmneziaWG profile and its configuration.";
    action_id = "remove";
    action_label = "Remove";
    for (guint i = 0; snapshot != NULL && i < snapshot->entries->len; i++) {
      const NetworkSidebarAwgEntry *entry = g_ptr_array_index(snapshot->entries, i);

      if (g_strcmp0(entry->name, pending->name) != 0)
        continue;
      if (entry->display_runtime_state == NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_ACTIVE) {
        body = "This disconnects the tunnel and permanently removes the saved "
               "AmneziaWG profile and its configuration.";
        action_label = "Disconnect and remove";
      } else if (network_sidebar_amneziawg_runtime_state_is_recoverable(entry->display_runtime_state)) {
        body = "This cleans up the tunnel's runtime state and permanently removes "
               "the saved AmneziaWG profile and its configuration.";
        action_label = "Clean up and remove";
      }
      break;
    }
  }

  dialog = adw_alert_dialog_new(heading, body);
  gtk_widget_add_css_class(GTK_WIDGET(dialog), "nm-sidebar-dialog");
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog),
                                action_id,
                                action_label);
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
                                           action_id,
                                           ADW_RESPONSE_DESTRUCTIVE);
  close_dialog_on_backdrop_click(dialog);

  confirmation = g_new0(ConfirmationData, 1);
  confirmation->presenter = network_sidebar_awg_presenter_ref(presenter);
  confirmation->type = pending->type;
  confirmation->activity_id = pending->activity_id;
  presenter->dialog = g_object_ref_sink(dialog);
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog),
                          GTK_WIDGET(parent),
                          presenter->lifecycle_cancellable,
                          confirmation_chosen_cb,
                          confirmation);
}

static gboolean
flush_deferred_cb(gpointer user_data)
{
  NetworkSidebarAwgPresenter *presenter = user_data;
  g_autofree char *message = NULL;

  presenter->flush_source = 0;
  if (presenter->stopping)
    return G_SOURCE_REMOVE;
  if (presenter->pending_confirmation != NULL &&
      !network_sidebar_awg_controller_confirmation_is_pending(
        presenter->controller,
        presenter->pending_confirmation->type,
        presenter->pending_confirmation->activity_id))
    clear_confirmation(presenter);
  if (!presentation_is_mapped(presenter) || presenter->chooser != NULL ||
      presenter->dialog != NULL)
    return G_SOURCE_REMOVE;
  if (presenter->pending_confirmation != NULL) {
    withdraw_toast(presenter, TRUE);
    present_confirmation(presenter);
    return G_SOURCE_REMOVE;
  }
  if (presenter->toast != NULL)
    return G_SOURCE_REMOVE;
  if (presenter->notices_omitted) {
    presenter->notices_omitted = FALSE;
    message = g_strdup(AWG_OMITTED_NOTICE);
  } else {
    message = g_queue_pop_head(&presenter->notices);
  }
  if (message != NULL)
    present_toast(presenter, message, FALSE);
  return G_SOURCE_REMOVE;
}

static void
schedule_flush(NetworkSidebarAwgPresenter *presenter)
{
  if (!presenter->started || presenter->stopping || presenter->flush_source != 0 ||
      (presenter->pending_confirmation == NULL &&
       g_queue_is_empty(&presenter->notices) && !presenter->notices_omitted))
    return;
  presenter->flush_source = g_idle_add_full(
    G_PRIORITY_DEFAULT_IDLE,
    flush_deferred_cb,
    network_sidebar_awg_presenter_ref(presenter),
    (GDestroyNotify) network_sidebar_awg_presenter_unref);
}

static void
parent_mapped_cb(GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  schedule_flush(user_data);
}

static void
parent_unmapped_cb(GtkWidget *widget, gpointer user_data)
{
  g_autoptr(NetworkSidebarAwgPresenter) presenter =
    network_sidebar_awg_presenter_ref(user_data);
  (void) widget;

  withdraw_toast(presenter, !presenter->stopping);
}

static gboolean
accept_confirmation(NetworkSidebarAwgPresenter *presenter,
                    const NetworkSidebarAwgUiEvent *event)
{
  DeferredConfirmation *pending = presenter->pending_confirmation;

  if (!network_sidebar_awg_controller_confirmation_is_pending(
        presenter->controller, event->type, event->activity_id))
    return FALSE;
  if (pending != NULL)
    return pending->type == event->type &&
           pending->activity_id == event->activity_id &&
           g_strcmp0(pending->name, event->name) == 0;
  if (!can_accept_interaction(presenter) || event->name == NULL)
    return FALSE;
  pending = g_new0(DeferredConfirmation, 1);
  pending->type = event->type;
  pending->activity_id = event->activity_id;
  pending->name = g_strdup(event->name);
  presenter->pending_confirmation = pending;
  schedule_flush(presenter);
  return TRUE;
}

static void
cancel_import_interaction(NetworkSidebarAwgPresenter *presenter)
{
  destroy_chooser(presenter);
  set_external_interaction(presenter, FALSE, TRUE);
  if (presenter->import_loader != NULL)
    network_sidebar_awg_import_loader_cancel(presenter->import_loader);
}

static void
dismiss_interactions(NetworkSidebarAwgPresenter *presenter)
{
  clear_confirmation(presenter);
  g_queue_clear_full(&presenter->notices, g_free);
  presenter->notices_omitted = FALSE;
  withdraw_toast(presenter, FALSE);
  cancel_import_interaction(presenter);
}

static gboolean
controller_ui_event(const NetworkSidebarAwgUiEvent *event, gpointer user_data)
{
  g_autoptr(NetworkSidebarAwgPresenter) presenter_ref =
    network_sidebar_awg_presenter_ref(user_data);
  NetworkSidebarAwgPresenter *presenter = presenter_ref;

  if (presenter == NULL)
    return FALSE;

  switch (event->type) {
  case NETWORK_SIDEBAR_AWG_UI_NOTICE:
    show_notice(presenter, event);
    return TRUE;
  case NETWORK_SIDEBAR_AWG_UI_SELECT_IMPORT:
    return event->availability_probe ? can_accept_interaction(presenter) :
      present_import_chooser(presenter, event->activity_id);
  case NETWORK_SIDEBAR_AWG_UI_CONFIRM_REPLACE:
  case NETWORK_SIDEBAR_AWG_UI_CONFIRM_DELETE:
    return event->availability_probe ? can_accept_interaction(presenter) :
      accept_confirmation(presenter, event);
  case NETWORK_SIDEBAR_AWG_UI_EXTERNAL_INTERACTION_ACTIVE:
    return presenter->external_interaction_active;
  case NETWORK_SIDEBAR_AWG_UI_DISMISS_ALL:
    dismiss_interactions(presenter);
    return TRUE;
  default:
    return FALSE;
  }
}

NetworkSidebarAwgPresenter *
network_sidebar_awg_presenter_new(
  NetworkSidebarAwgChangedCallback changed,
  NetworkSidebarAwgExternalInteractionCallback external_interaction,
  gpointer user_data)
{
  NetworkSidebarAwgPresenter *presenter = g_new0(
    NetworkSidebarAwgPresenter, 1);

  presenter->ref_count = 1;
  presenter->changed = changed;
  presenter->external_interaction = external_interaction;
  presenter->user_data = user_data;
  presenter->lifecycle_cancellable = g_cancellable_new();
  g_weak_ref_init(&presenter->parent, NULL);
  g_weak_ref_init(&presenter->toast_overlay, NULL);
  presenter->controller = network_sidebar_awg_controller_new(forward_changed,
                                                             presenter);
  network_sidebar_awg_controller_set_ui_event_callback(
    presenter->controller, controller_ui_event, presenter);
  return presenter;
}

static void
presenter_stop_internal(NetworkSidebarAwgPresenter *presenter,
                        gboolean notify_external)
{
  g_autoptr(GObject) parent = NULL;
  g_autoptr(GObject) overlay = NULL;

  if (presenter->stopping)
    return;
  presenter->stopping = TRUE;
  presenter->started = FALSE;
  g_clear_handle_id(&presenter->flush_source, g_source_remove);
  parent = g_weak_ref_get(&presenter->parent);
  overlay = g_weak_ref_get(&presenter->toast_overlay);
  if (parent != NULL)
    g_signal_handlers_disconnect_by_data(parent, presenter);
  if (overlay != NULL)
    g_signal_handlers_disconnect_by_data(overlay, presenter);
  g_cancellable_cancel(presenter->lifecycle_cancellable);
  if (!notify_external)
    presenter->external_interaction = NULL;
  /* Controller stop emits UI events; final unref must not re-enter us. */
  network_sidebar_awg_controller_set_ui_event_callback(presenter->controller,
                                                       NULL,
                                                       NULL);
  network_sidebar_awg_controller_stop(presenter->controller);
  dismiss_interactions(presenter);
  set_external_interaction(presenter, FALSE, notify_external);
  presenter->changed = NULL;
  presenter->external_interaction = NULL;
  presenter->user_data = NULL;
  g_weak_ref_set(&presenter->parent, NULL);
  g_weak_ref_set(&presenter->toast_overlay, NULL);
}

void
network_sidebar_awg_presenter_unref(NetworkSidebarAwgPresenter *presenter)
{
  if (presenter == NULL)
    return;
  if (!g_atomic_int_dec_and_test(&presenter->ref_count))
    return;

  presenter_stop_internal(presenter, FALSE);
  g_clear_pointer(&presenter->import_loader,
                  network_sidebar_awg_import_loader_unref);
  g_clear_pointer(&presenter->controller,
                  network_sidebar_awg_controller_unref);
  g_clear_object(&presenter->lifecycle_cancellable);
  g_weak_ref_clear(&presenter->parent);
  g_weak_ref_clear(&presenter->toast_overlay);
  g_free(presenter);
}

void
network_sidebar_awg_presenter_start(NetworkSidebarAwgPresenter *presenter)
{
  if (presenter == NULL || presenter->started || presenter->stopping)
    return;
  presenter->started = TRUE;
  network_sidebar_awg_controller_start(presenter->controller);
}

void
network_sidebar_awg_presenter_stop(NetworkSidebarAwgPresenter *presenter)
{
  g_autoptr(NetworkSidebarAwgPresenter) presenter_ref =
    network_sidebar_awg_presenter_ref(presenter);

  if (presenter_ref != NULL)
    presenter_stop_internal(presenter_ref, TRUE);
}

void
network_sidebar_awg_presenter_set_parent(
  NetworkSidebarAwgPresenter *presenter,
  GtkWindow *parent,
  AdwToastOverlay *toast_overlay)
{
  g_autoptr(GObject) previous_parent = NULL;
  g_autoptr(GObject) previous_overlay = NULL;

  if (presenter == NULL || presenter->stopping)
    return;
  previous_parent = g_weak_ref_get(&presenter->parent);
  previous_overlay = g_weak_ref_get(&presenter->toast_overlay);
  if (previous_parent == G_OBJECT(parent) &&
      previous_overlay == G_OBJECT(toast_overlay)) {
    schedule_flush(presenter);
    return;
  }
  if (previous_parent != NULL)
    g_signal_handlers_disconnect_by_data(previous_parent, presenter);
  if (previous_overlay != NULL)
    g_signal_handlers_disconnect_by_data(previous_overlay, presenter);
  withdraw_toast(presenter, TRUE);
  if (presenter->dialog != NULL) {
    AdwDialog *dialog = g_steal_pointer(&presenter->dialog);

    /* Reparent presentation without answering the controller's confirmation. */
    adw_dialog_force_close(dialog);
    g_object_unref(dialog);
  }
  g_weak_ref_set(&presenter->parent, parent);
  g_weak_ref_set(&presenter->toast_overlay, toast_overlay);
  if (parent != NULL) {
    g_signal_connect(parent, "map", G_CALLBACK(parent_mapped_cb), presenter);
    g_signal_connect(parent, "unmap", G_CALLBACK(parent_unmapped_cb), presenter);
  }
  if (toast_overlay != NULL)
    g_signal_connect(toast_overlay, "map", G_CALLBACK(parent_mapped_cb), presenter);
  schedule_flush(presenter);
}

NetworkSidebarAwgController *
network_sidebar_awg_presenter_get_controller(
  NetworkSidebarAwgPresenter *presenter)
{
  return presenter != NULL ? presenter->controller : NULL;
}

void
network_sidebar_awg_presenter_import(NetworkSidebarAwgPresenter *presenter)
{
  g_autoptr(NetworkSidebarAwgPresenter) presenter_ref =
    network_sidebar_awg_presenter_ref(presenter);

  if (presenter_ref == NULL || presenter_ref->stopping)
    return;
  network_sidebar_awg_controller_import(presenter_ref->controller);
}

void
network_sidebar_awg_presenter_cancel_import(
  NetworkSidebarAwgPresenter *presenter)
{
  g_autoptr(NetworkSidebarAwgPresenter) presenter_ref =
    network_sidebar_awg_presenter_ref(presenter);

  if (presenter_ref == NULL || presenter_ref->stopping)
    return;
  cancel_import_interaction(presenter_ref);
  network_sidebar_awg_controller_cancel_import(presenter_ref->controller);
}

void
network_sidebar_awg_presenter_set_active(NetworkSidebarAwgPresenter *presenter,
                                         const char *name,
                                         gboolean active)
{
  g_autoptr(NetworkSidebarAwgPresenter) presenter_ref =
    network_sidebar_awg_presenter_ref(presenter);

  if (presenter_ref == NULL || presenter_ref->stopping)
    return;
  network_sidebar_awg_controller_set_active(presenter_ref->controller,
                                            name,
                                            active);
}

void
network_sidebar_awg_presenter_request_delete(
  NetworkSidebarAwgPresenter *presenter,
  const char *name)
{
  g_autoptr(NetworkSidebarAwgPresenter) presenter_ref =
    network_sidebar_awg_presenter_ref(presenter);

  if (presenter_ref == NULL || presenter_ref->stopping)
    return;
  network_sidebar_awg_controller_confirm_delete(presenter_ref->controller,
                                                name);
}
