#ifndef NETWORK_SIDEBAR_ACTIONS_EDITOR_SESSION_H
#define NETWORK_SIDEBAR_ACTIONS_EDITOR_SESSION_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _NetworkSidebarEditorSession NetworkSidebarEditorSession;
typedef void (*NetworkSidebarEditorCompletedCallback)(const char *message,
                                                      gpointer user_data);

NetworkSidebarEditorSession *network_sidebar_editor_session_new(
    NetworkSidebarEditorCompletedCallback completed, gpointer user_data);
NetworkSidebarEditorSession *
network_sidebar_editor_session_ref(NetworkSidebarEditorSession *session);
void network_sidebar_editor_session_unref(NetworkSidebarEditorSession *session);
gboolean
network_sidebar_editor_session_open(NetworkSidebarEditorSession *session,
                                    const char *const *args, GError **error);
/* Cancel supervision, not the external editor. Children are still reaped. */
void network_sidebar_editor_session_cancel(
    NetworkSidebarEditorSession *session);
/* Stop is terminal and must precede teardown of callback user_data. */
void network_sidebar_editor_session_stop(NetworkSidebarEditorSession *session);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarEditorSession,
                              network_sidebar_editor_session_unref)

G_END_DECLS

#endif
