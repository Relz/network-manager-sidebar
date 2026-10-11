#include "actions/editor_session.h"

#include <string.h>
#include <unistd.h>

#define CONNECTION_EDITOR_BUS_NAME "org.gnome.nm_connection_editor"

struct _NetworkSidebarEditorSession {
  gint ref_count;
  NetworkSidebarEditorCompletedCallback completed;
  gpointer user_data;
  guint64 generation;
  gboolean pending;
  gboolean stopping;
  GCancellable *wait;
  guint owner_watch;
};

typedef struct {
  NetworkSidebarEditorSession *session;
  guint64 generation;
} EditorProcess;

typedef struct {
  /* The session owns the name watch and cancels it before being freed. */
  NetworkSidebarEditorSession *session;
  guint64 generation;
  char *owner;
} EditorOwnerWatch;

NetworkSidebarEditorSession *network_sidebar_editor_session_new(
    NetworkSidebarEditorCompletedCallback completed, gpointer user_data) {
  NetworkSidebarEditorSession *session = g_new0(NetworkSidebarEditorSession, 1);

  session->ref_count = 1;
  session->completed = completed;
  session->user_data = user_data;
  return session;
}

NetworkSidebarEditorSession *
network_sidebar_editor_session_ref(NetworkSidebarEditorSession *session) {
  g_atomic_int_inc(&session->ref_count);
  return session;
}

void network_sidebar_editor_session_cancel(
    NetworkSidebarEditorSession *session) {
  session->generation++;
  session->pending = FALSE;
  if (session->wait != NULL)
    g_cancellable_cancel(session->wait);
  g_clear_object(&session->wait);
  if (session->owner_watch != 0) {
    g_bus_unwatch_name(session->owner_watch);
    session->owner_watch = 0;
  }
}

void network_sidebar_editor_session_stop(NetworkSidebarEditorSession *session) {
  if (session == NULL || session->stopping)
    return;
  session->stopping = TRUE;
  session->completed = NULL;
  session->user_data = NULL;
  network_sidebar_editor_session_cancel(session);
}

void network_sidebar_editor_session_unref(
    NetworkSidebarEditorSession *session) {
  if (session == NULL || !g_atomic_int_dec_and_test(&session->ref_count))
    return;
  network_sidebar_editor_session_stop(session);
  g_free(session);
}

static char **child_process_env(void) {
  char **env = g_get_environ();
  const char *preload = g_environ_getenv(env, "LD_PRELOAD");

  env = g_environ_unsetenv(env, "NM_SIDEBAR_LAYER_SHELL_PRELOAD");
  if (preload != NULL && strstr(preload, "libgtk4-layer-shell.so") != NULL)
    env = g_environ_unsetenv(env, "LD_PRELOAD");
  return env;
}

static char *find_program_in_path_env(const char *program, char **env) {
  const char *path = g_environ_getenv(env, "PATH");
  g_auto(GStrv) directories = NULL;

  if (path == NULL)
    return g_find_program_in_path(program);
  directories = g_strsplit(path, G_SEARCHPATH_SEPARATOR_S, -1);
  for (guint i = 0; directories[i] != NULL; i++) {
    const char *directory = *directories[i] != '\0' ? directories[i] : ".";
    g_autofree char *candidate = g_build_filename(directory, program, NULL);

    if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE))
      return g_steal_pointer(&candidate);
  }
  return NULL;
}

static void editor_child_setup(gpointer user_data) {
  (void)user_data;
  setsid();
}

static void editor_process_free(EditorProcess *process) {
  network_sidebar_editor_session_unref(process->session);
  g_free(process);
}

static void editor_owner_watch_free(gpointer user_data) {
  EditorOwnerWatch *watch = user_data;

  g_free(watch->owner);
  g_free(watch);
}

static gboolean editor_session_is_current(NetworkSidebarEditorSession *session,
                                          guint64 generation) {
  return !session->stopping && session->pending &&
         session->generation == generation;
}

static void finish_editor_session(NetworkSidebarEditorSession *session,
                                  guint64 generation, const char *message) {
  if (!editor_session_is_current(session, generation))
    return;
  network_sidebar_editor_session_cancel(session);
  if (session->completed != NULL)
    session->completed(message, session->user_data);
}

static void editor_owner_appeared_cb(GDBusConnection *connection,
                                     const char *name, const char *owner,
                                     gpointer user_data) {
  EditorOwnerWatch *watch = user_data;
  g_autoptr(NetworkSidebarEditorSession) session =
      network_sidebar_editor_session_ref(watch->session);
  guint64 generation = watch->generation;
  (void)connection;
  (void)name;

  /* A replacement editor does not own the completed session. */
  if (g_strcmp0(owner, watch->owner) != 0)
    finish_editor_session(session, generation, NULL);
}

static void editor_owner_vanished_cb(GDBusConnection *connection,
                                     const char *name, gpointer user_data) {
  EditorOwnerWatch *watch = user_data;
  g_autoptr(NetworkSidebarEditorSession) session =
      network_sidebar_editor_session_ref(watch->session);
  guint64 generation = watch->generation;
  (void)connection;
  (void)name;

  finish_editor_session(session, generation, NULL);
}

static void editor_owner_query_cb(GObject *source, GAsyncResult *result,
                                  gpointer user_data) {
  EditorProcess *process = user_data;
  NetworkSidebarEditorSession *session = process->session;
  g_autoptr(GVariant) reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);

  if (editor_session_is_current(session, process->generation)) {
    g_clear_object(&session->wait);
    if (reply != NULL) {
      EditorOwnerWatch *watch = g_new0(EditorOwnerWatch, 1);

      watch->session = session;
      watch->generation = process->generation;
      g_variant_get(reply, "(s)", &watch->owner);
      session->owner_watch = g_bus_watch_name_on_connection(
          G_DBUS_CONNECTION(source), CONNECTION_EDITOR_BUS_NAME,
          G_BUS_NAME_WATCHER_FLAGS_NONE, editor_owner_appeared_cb,
          editor_owner_vanished_cb, watch, editor_owner_watch_free);
    } else {
      /* No running editor remains, or the session bus is unavailable. */
      finish_editor_session(session, process->generation, NULL);
    }
  }
  editor_process_free(process);
}

static void editor_bus_ready_cb(GObject *source, GAsyncResult *result,
                                gpointer user_data) {
  EditorProcess *process = user_data;
  NetworkSidebarEditorSession *session = process->session;
  g_autoptr(GDBusConnection) connection = g_bus_get_finish(result, NULL);
  (void)source;

  if (editor_session_is_current(session, process->generation) &&
      connection != NULL) {
    /* Remote command forwarding can exit before the editor window. */
    g_dbus_connection_call(connection, "org.freedesktop.DBus",
                           "/org/freedesktop/DBus", "org.freedesktop.DBus",
                           "GetNameOwner",
                           g_variant_new("(s)", CONNECTION_EDITOR_BUS_NAME),
                           G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 5000,
                           session->wait, editor_owner_query_cb, process);
    return;
  }
  finish_editor_session(session, process->generation, NULL);
  editor_process_free(process);
}

static void editor_exited_cb(GPid pid, gint wait_status, gpointer user_data) {
  EditorProcess *process = user_data;
  NetworkSidebarEditorSession *session = process->session;
  g_autoptr(GError) error = NULL;

  g_spawn_close_pid(pid);
  if (!g_spawn_check_wait_status(wait_status, &error)) {
    g_autofree char *message = g_strdup_printf(
        "nm-connection-editor exited unexpectedly: %s", error->message);

    g_printerr("nm-sidebar: %s\n", message);
    finish_editor_session(session, process->generation, message);
  } else if (editor_session_is_current(session, process->generation)) {
    session->wait = g_cancellable_new();
    g_bus_get(G_BUS_TYPE_SESSION, session->wait, editor_bus_ready_cb, process);
    return;
  }
  editor_process_free(process);
}

gboolean
network_sidebar_editor_session_open(NetworkSidebarEditorSession *session,
                                    const char *const *args, GError **error) {
  g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
  g_auto(GStrv) env = child_process_env();
  g_autofree char *editor_path =
      find_program_in_path_env("nm-connection-editor", env);
  GPid pid = 0;
  EditorProcess *process;

  if (session->stopping) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                        "Editor session is stopped");
    return FALSE;
  }
  network_sidebar_editor_session_cancel(session);
  g_ptr_array_add(argv, g_strdup(editor_path != NULL ? editor_path
                                                     : "nm-connection-editor"));
  for (guint i = 0; args != NULL && args[i] != NULL; i++)
    g_ptr_array_add(argv, g_strdup(args[i]));
  g_ptr_array_add(argv, NULL);

  if (!g_spawn_async(NULL, (char **)argv->pdata, env,
                     G_SPAWN_SEARCH_PATH_FROM_ENVP |
                         G_SPAWN_STDOUT_TO_DEV_NULL |
                         G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_DO_NOT_REAP_CHILD,
                     editor_child_setup, NULL, &pid, error))
    return FALSE;

  session->pending = TRUE;
  process = g_new0(EditorProcess, 1);
  process->session = network_sidebar_editor_session_ref(session);
  process->generation = session->generation;
  g_child_watch_add(pid, editor_exited_cb, process);
  return TRUE;
}
