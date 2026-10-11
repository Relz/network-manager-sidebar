#include "gui/profile_import_presenter.h"

struct _NetworkSidebarProfileImportPresenter {
  gint ref_count;
  GWeakRef parent;
  GtkNativeDialog *chooser;
  gulong response_handler;
  gboolean stopping;
  NetworkSidebarProfileImportInteractionCallback interaction;
  NetworkSidebarProfileImportSelectedCallback selected;
  NetworkSidebarProfileImportErrorCallback error;
  gpointer user_data;
};

NetworkSidebarProfileImportPresenter *
network_sidebar_profile_import_presenter_new(
    NetworkSidebarProfileImportInteractionCallback interaction,
    NetworkSidebarProfileImportSelectedCallback selected,
    NetworkSidebarProfileImportErrorCallback error, gpointer user_data) {
  NetworkSidebarProfileImportPresenter *presenter =
      g_new0(NetworkSidebarProfileImportPresenter, 1);

  presenter->ref_count = 1;
  g_weak_ref_init(&presenter->parent, NULL);
  presenter->interaction = interaction;
  presenter->selected = selected;
  presenter->error = error;
  presenter->user_data = user_data;
  return presenter;
}

NetworkSidebarProfileImportPresenter *
network_sidebar_profile_import_presenter_ref(
    NetworkSidebarProfileImportPresenter *presenter) {
  g_atomic_int_inc(&presenter->ref_count);
  return presenter;
}

void network_sidebar_profile_import_presenter_dismiss(
    NetworkSidebarProfileImportPresenter *presenter) {
  GtkNativeDialog *chooser = g_steal_pointer(&presenter->chooser);
  gulong handler = presenter->response_handler;

  presenter->response_handler = 0;
  if (chooser == NULL)
    return;
  if (handler != 0 && g_signal_handler_is_connected(chooser, handler))
    g_signal_handler_disconnect(chooser, handler);
  gtk_native_dialog_destroy(chooser);
  g_object_unref(chooser);
}

void network_sidebar_profile_import_presenter_stop(
    NetworkSidebarProfileImportPresenter *presenter) {
  if (presenter == NULL || presenter->stopping)
    return;
  presenter->stopping = TRUE;
  presenter->interaction = NULL;
  presenter->selected = NULL;
  presenter->error = NULL;
  presenter->user_data = NULL;
  network_sidebar_profile_import_presenter_dismiss(presenter);
  g_weak_ref_set(&presenter->parent, NULL);
}

void network_sidebar_profile_import_presenter_unref(
    NetworkSidebarProfileImportPresenter *presenter) {
  if (presenter == NULL || !g_atomic_int_dec_and_test(&presenter->ref_count))
    return;
  network_sidebar_profile_import_presenter_stop(presenter);
  g_weak_ref_clear(&presenter->parent);
  g_free(presenter);
}

void network_sidebar_profile_import_presenter_set_parent(
    NetworkSidebarProfileImportPresenter *presenter, GtkWindow *parent) {
  if (!presenter->stopping)
    g_weak_ref_set(&presenter->parent, parent);
}

void network_sidebar_profile_import_presenter_cancel(
    NetworkSidebarProfileImportPresenter *presenter) {
  g_autoptr(NetworkSidebarProfileImportPresenter) keep_alive =
      network_sidebar_profile_import_presenter_ref(presenter);

  if (presenter->chooser == NULL)
    return;
  network_sidebar_profile_import_presenter_dismiss(presenter);
  if (presenter->interaction != NULL)
    presenter->interaction(FALSE, presenter->user_data);
}

static void import_response_cb(GtkNativeDialog *dialog, int response,
                               gpointer user_data) {
  g_autoptr(NetworkSidebarProfileImportPresenter) presenter =
      network_sidebar_profile_import_presenter_ref(user_data);
  g_autoptr(GFile) file = NULL;
  g_autofree char *path = NULL;

  if (presenter->stopping || presenter->chooser != dialog)
    return;
  if (response != GTK_RESPONSE_ACCEPT) {
    network_sidebar_profile_import_presenter_cancel(presenter);
    return;
  }
  file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
  path = file != NULL ? g_file_get_path(file) : NULL;
  if (path == NULL) {
    network_sidebar_profile_import_presenter_cancel(presenter);
    if (presenter->error != NULL)
      presenter->error("Select a local VPN configuration file",
                       presenter->user_data);
    return;
  }

  /* Completion is a handoff; the coordinator releases interaction ownership. */
  network_sidebar_profile_import_presenter_dismiss(presenter);
  if (presenter->selected != NULL)
    presenter->selected(path, presenter->user_data);
}

void network_sidebar_profile_import_presenter_import(
    NetworkSidebarProfileImportPresenter *presenter) {
  g_autoptr(NetworkSidebarProfileImportPresenter) keep_alive =
      network_sidebar_profile_import_presenter_ref(presenter);
  g_autoptr(GObject) parent = NULL;
  g_autoptr(GtkNativeDialog) chooser = NULL;

  if (presenter->stopping || presenter->chooser != NULL)
    return;
  parent = g_weak_ref_get(&presenter->parent);
  if (parent == NULL) {
    if (presenter->error != NULL)
      presenter->error("Could not show VPN import dialog",
                       presenter->user_data);
    return;
  }
  chooser = GTK_NATIVE_DIALOG(gtk_file_chooser_native_new(
      "Import VPN Configuration", NULL, GTK_FILE_CHOOSER_ACTION_OPEN, "Import",
      "Cancel"));
  gtk_native_dialog_set_modal(chooser, FALSE);
  presenter->chooser = g_object_ref(chooser);
  presenter->response_handler = g_signal_connect(
      chooser, "response", G_CALLBACK(import_response_cb), presenter);
  if (presenter->interaction != NULL)
    presenter->interaction(TRUE, presenter->user_data);
  /* The callback may synchronously dismiss the chooser or stop the presenter.
   */
  if (!presenter->stopping && presenter->chooser == chooser)
    gtk_native_dialog_show(presenter->chooser);
}
