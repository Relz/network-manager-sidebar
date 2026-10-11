#ifndef NETWORK_SIDEBAR_GUI_PROFILE_EDITOR_PRESENTER_H
#define NETWORK_SIDEBAR_GUI_PROFILE_EDITOR_PRESENTER_H

#include "gui/external_interaction.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _NetworkSidebarProfileEditorPresenter
    NetworkSidebarProfileEditorPresenter;
typedef void (*NetworkSidebarProfileEditorChangedCallback)(guint delay_ms,
                                                           gpointer user_data);
typedef void (*NetworkSidebarProfileEditorErrorCallback)(const char *message,
                                                         gpointer user_data);

NetworkSidebarProfileEditorPresenter *
network_sidebar_profile_editor_presenter_new(
    NetworkSidebarInteractionCallback interaction,
    NetworkSidebarProfileEditorChangedCallback changed,
    NetworkSidebarProfileEditorErrorCallback error, gpointer user_data);
NetworkSidebarProfileEditorPresenter *
network_sidebar_profile_editor_presenter_ref(
    NetworkSidebarProfileEditorPresenter *presenter);
void network_sidebar_profile_editor_presenter_unref(
    NetworkSidebarProfileEditorPresenter *presenter);
void network_sidebar_profile_editor_presenter_set_parent(
    NetworkSidebarProfileEditorPresenter *presenter, GtkWindow *parent);
void network_sidebar_profile_editor_presenter_import(
    NetworkSidebarProfileEditorPresenter *presenter);
void network_sidebar_profile_editor_presenter_open(
    NetworkSidebarProfileEditorPresenter *presenter, const char *const *args);
/* Terminal; suppresses callbacks and must precede teardown of user_data. */
void network_sidebar_profile_editor_presenter_stop(
    NetworkSidebarProfileEditorPresenter *presenter);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarProfileEditorPresenter,
                              network_sidebar_profile_editor_presenter_unref)

G_END_DECLS

#endif
