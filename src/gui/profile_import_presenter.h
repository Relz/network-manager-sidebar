#ifndef NETWORK_SIDEBAR_GUI_PROFILE_IMPORT_PRESENTER_H
#define NETWORK_SIDEBAR_GUI_PROFILE_IMPORT_PRESENTER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _NetworkSidebarProfileImportPresenter
    NetworkSidebarProfileImportPresenter;
typedef void (*NetworkSidebarProfileImportInteractionCallback)(
    gboolean active, gpointer user_data);
/* The selected local path is borrowed for the duration of the callback. */
typedef void (*NetworkSidebarProfileImportSelectedCallback)(const char *path,
                                                            gpointer user_data);
typedef void (*NetworkSidebarProfileImportErrorCallback)(const char *message,
                                                         gpointer user_data);

NetworkSidebarProfileImportPresenter *
network_sidebar_profile_import_presenter_new(
    NetworkSidebarProfileImportInteractionCallback interaction,
    NetworkSidebarProfileImportSelectedCallback selected,
    NetworkSidebarProfileImportErrorCallback error, gpointer user_data);
NetworkSidebarProfileImportPresenter *
network_sidebar_profile_import_presenter_ref(
    NetworkSidebarProfileImportPresenter *presenter);
void network_sidebar_profile_import_presenter_unref(
    NetworkSidebarProfileImportPresenter *presenter);
void network_sidebar_profile_import_presenter_set_parent(
    NetworkSidebarProfileImportPresenter *presenter, GtkWindow *parent);
void network_sidebar_profile_import_presenter_import(
    NetworkSidebarProfileImportPresenter *presenter);
void network_sidebar_profile_import_presenter_cancel(
    NetworkSidebarProfileImportPresenter *presenter);
/* Silent dismissal preserves the chooser-to-editor handoff. */
void network_sidebar_profile_import_presenter_dismiss(
    NetworkSidebarProfileImportPresenter *presenter);
/* Stop is terminal and must precede teardown of callback user_data. */
void network_sidebar_profile_import_presenter_stop(
    NetworkSidebarProfileImportPresenter *presenter);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarProfileImportPresenter,
                              network_sidebar_profile_import_presenter_unref)

G_END_DECLS

#endif
