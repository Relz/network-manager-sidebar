#include "gui/profile_editor_presenter.h"

#include "actions/editor_session.h"
#include "gui/profile_import_presenter.h"

typedef enum {
  PROFILE_EDITOR_IDLE,
  PROFILE_EDITOR_CHOOSING,
  PROFILE_EDITOR_LAUNCHING,
  PROFILE_EDITOR_EDITING,
  PROFILE_EDITOR_STOPPED,
} ProfileEditorState;

struct _NetworkSidebarProfileEditorPresenter {
  gint ref_count;
  GWeakRef parent;
  NetworkSidebarProfileImportPresenter *chooser;
  NetworkSidebarEditorSession *session;
  ProfileEditorState state;
  NetworkSidebarInteractionCallback interaction;
  NetworkSidebarProfileEditorChangedCallback changed;
  NetworkSidebarProfileEditorErrorCallback error;
  gpointer user_data;
};

static void notify_interaction(NetworkSidebarProfileEditorPresenter *presenter,
                               NetworkSidebarInteractionOwner owner,
                               NetworkSidebarInteractionEvent event) {
  if (presenter->interaction != NULL)
    presenter->interaction(owner, event, presenter->user_data);
}

static void report_error(const char *message, gpointer user_data) {
  NetworkSidebarProfileEditorPresenter *presenter = user_data;

  if (presenter->error != NULL)
    presenter->error(message, presenter->user_data);
}

static void chooser_interaction_cb(gboolean active, gpointer user_data) {
  g_autoptr(NetworkSidebarProfileEditorPresenter) presenter =
      network_sidebar_profile_editor_presenter_ref(user_data);

  if (presenter->state == PROFILE_EDITOR_STOPPED)
    return;
  presenter->state = active ? PROFILE_EDITOR_CHOOSING : PROFILE_EDITOR_IDLE;
  notify_interaction(presenter, NETWORK_SIDEBAR_INTERACTION_PROFILE_IMPORT,
                     active ? NETWORK_SIDEBAR_INTERACTION_BEGIN
                            : NETWORK_SIDEBAR_INTERACTION_END);
  if (active && presenter->state == PROFILE_EDITOR_CHOOSING) {
    network_sidebar_editor_session_cancel(presenter->session);
    notify_interaction(presenter, NETWORK_SIDEBAR_INTERACTION_PROFILE_EDITOR,
                       NETWORK_SIDEBAR_INTERACTION_CANCEL);
  }
}

static void chooser_selected_cb(const char *path, gpointer user_data) {
  NetworkSidebarProfileEditorPresenter *presenter = user_data;
  g_autofree char *import_arg = g_strdup_printf("--import=%s", path);

  if (presenter->state == PROFILE_EDITOR_CHOOSING)
    network_sidebar_profile_editor_presenter_open(
        presenter, (const char *const[]){import_arg, NULL});
}

static void editor_completed_cb(const char *message, gpointer user_data) {
  g_autoptr(NetworkSidebarProfileEditorPresenter) presenter =
      network_sidebar_profile_editor_presenter_ref(user_data);

  if (presenter->state != PROFILE_EDITOR_EDITING)
    return;
  presenter->state = PROFILE_EDITOR_IDLE;
  notify_interaction(presenter, NETWORK_SIDEBAR_INTERACTION_PROFILE_EDITOR,
                     NETWORK_SIDEBAR_INTERACTION_END);
  if (presenter->changed != NULL)
    presenter->changed(1, presenter->user_data);
  if (message != NULL)
    report_error(message, presenter);
}

NetworkSidebarProfileEditorPresenter *
network_sidebar_profile_editor_presenter_new(
    NetworkSidebarInteractionCallback interaction,
    NetworkSidebarProfileEditorChangedCallback changed,
    NetworkSidebarProfileEditorErrorCallback error, gpointer user_data) {
  NetworkSidebarProfileEditorPresenter *presenter =
      g_new0(NetworkSidebarProfileEditorPresenter, 1);

  presenter->ref_count = 1;
  g_weak_ref_init(&presenter->parent, NULL);
  presenter->interaction = interaction;
  presenter->changed = changed;
  presenter->error = error;
  presenter->user_data = user_data;
  presenter->chooser = network_sidebar_profile_import_presenter_new(
      chooser_interaction_cb, chooser_selected_cb, report_error, presenter);
  presenter->session =
      network_sidebar_editor_session_new(editor_completed_cb, presenter);
  return presenter;
}

NetworkSidebarProfileEditorPresenter *
network_sidebar_profile_editor_presenter_ref(
    NetworkSidebarProfileEditorPresenter *presenter) {
  g_atomic_int_inc(&presenter->ref_count);
  return presenter;
}

void network_sidebar_profile_editor_presenter_stop(
    NetworkSidebarProfileEditorPresenter *presenter) {
  if (presenter == NULL || presenter->state == PROFILE_EDITOR_STOPPED)
    return;
  presenter->state = PROFILE_EDITOR_STOPPED;
  presenter->interaction = NULL;
  presenter->changed = NULL;
  presenter->error = NULL;
  presenter->user_data = NULL;
  network_sidebar_profile_import_presenter_stop(presenter->chooser);
  network_sidebar_editor_session_stop(presenter->session);
  g_weak_ref_set(&presenter->parent, NULL);
}

void network_sidebar_profile_editor_presenter_unref(
    NetworkSidebarProfileEditorPresenter *presenter) {
  if (presenter == NULL || !g_atomic_int_dec_and_test(&presenter->ref_count))
    return;
  network_sidebar_profile_editor_presenter_stop(presenter);
  network_sidebar_profile_import_presenter_unref(presenter->chooser);
  network_sidebar_editor_session_unref(presenter->session);
  g_weak_ref_clear(&presenter->parent);
  g_free(presenter);
}

void network_sidebar_profile_editor_presenter_set_parent(
    NetworkSidebarProfileEditorPresenter *presenter, GtkWindow *parent) {
  if (presenter->state == PROFILE_EDITOR_STOPPED)
    return;
  g_weak_ref_set(&presenter->parent, parent);
  network_sidebar_profile_import_presenter_set_parent(presenter->chooser,
                                                      parent);
}

void network_sidebar_profile_editor_presenter_import(
    NetworkSidebarProfileEditorPresenter *presenter) {
  g_autoptr(NetworkSidebarProfileEditorPresenter) keep_alive =
      network_sidebar_profile_editor_presenter_ref(presenter);

  if (presenter->state != PROFILE_EDITOR_STOPPED)
    network_sidebar_profile_import_presenter_import(presenter->chooser);
}

void network_sidebar_profile_editor_presenter_open(
    NetworkSidebarProfileEditorPresenter *presenter, const char *const *args) {
  g_autoptr(NetworkSidebarProfileEditorPresenter) keep_alive =
      network_sidebar_profile_editor_presenter_ref(presenter);
  g_autoptr(GObject) parent = NULL;
  g_autoptr(GError) error = NULL;

  if (presenter->state == PROFILE_EDITOR_STOPPED)
    return;
  parent = g_weak_ref_get(&presenter->parent);
  if (parent == NULL) {
    network_sidebar_profile_import_presenter_cancel(presenter->chooser);
    /* Selection may already have silently dismissed the chooser. */
    if (presenter->state == PROFILE_EDITOR_CHOOSING) {
      presenter->state = PROFILE_EDITOR_IDLE;
      notify_interaction(presenter, NETWORK_SIDEBAR_INTERACTION_PROFILE_IMPORT,
                         NETWORK_SIDEBAR_INTERACTION_END);
    }
    return;
  }

  network_sidebar_editor_session_cancel(presenter->session);
  presenter->state = PROFILE_EDITOR_LAUNCHING;
  notify_interaction(presenter, NETWORK_SIDEBAR_INTERACTION_PROFILE_EDITOR,
                     NETWORK_SIDEBAR_INTERACTION_BEGIN);
  if (presenter->state != PROFILE_EDITOR_LAUNCHING)
    return;
  /* Hold launch ownership before silently dismissing and releasing selection.
   */
  network_sidebar_profile_import_presenter_dismiss(presenter->chooser);
  notify_interaction(presenter, NETWORK_SIDEBAR_INTERACTION_PROFILE_IMPORT,
                     NETWORK_SIDEBAR_INTERACTION_END);
  if (presenter->state != PROFILE_EDITOR_LAUNCHING)
    return;

  if (!network_sidebar_editor_session_open(presenter->session, args, &error)) {
    g_autofree char *message =
        g_strdup_printf("Could not open nm-connection-editor: %s",
                        error != NULL ? error->message : "unknown error");

    presenter->state = PROFILE_EDITOR_IDLE;
    notify_interaction(presenter, NETWORK_SIDEBAR_INTERACTION_PROFILE_EDITOR,
                       NETWORK_SIDEBAR_INTERACTION_END);
    report_error(message, presenter);
  } else {
    presenter->state = PROFILE_EDITOR_EDITING;
    notify_interaction(presenter, NETWORK_SIDEBAR_INTERACTION_PROFILE_EDITOR,
                       NETWORK_SIDEBAR_INTERACTION_DEFER);
  }
}
