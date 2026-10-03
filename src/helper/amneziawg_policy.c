#include "helper/amneziawg_policy.h"

#include "core/config.h"

#define POLKIT_EXEC_PATH_ANNOTATION "org.freedesktop.policykit.exec.path"
#define POLKIT_EXEC_ARGV1_ANNOTATION "org.freedesktop.policykit.exec.argv1"
#define POLKIT_IMPLY_ANNOTATION "org.freedesktop.policykit.imply"

static gboolean
action_id_is_amneziawg(const char *action_id)
{
  return g_strcmp0(action_id, NETWORK_SIDEBAR_AMNEZIAWG_ACTION_ID) == 0;
}

static gboolean
action_implies_amneziawg(PolkitActionDescription *description)
{
  const char *annotation = polkit_action_description_get_annotation(
    description, POLKIT_IMPLY_ANNOTATION);
  g_auto(GStrv) implied_actions = NULL;

  if (annotation == NULL)
    return FALSE;
  implied_actions = g_strsplit_set(annotation, " \t\r\n", -1);
  for (guint i = 0; implied_actions[i] != NULL; i++) {
    if (action_id_is_amneziawg(implied_actions[i]))
      return TRUE;
  }
  return FALSE;
}

static gboolean
policy_descriptions_are_safe(const GList *descriptions)
{
  gboolean manage_found = FALSE;
  gboolean safe = TRUE;

  for (const GList *item = descriptions; item != NULL; item = item->next) {
    PolkitActionDescription *description = item->data;
    const char *action_id = polkit_action_description_get_action_id(description);
    const char *exec_path = polkit_action_description_get_annotation(
      description, POLKIT_EXEC_PATH_ANNOTATION);
    const char *exec_argv1 = polkit_action_description_get_annotation(
      description, POLKIT_EXEC_ARGV1_ANNOTATION);

    if (action_implies_amneziawg(description))
      safe = FALSE;
    if (!action_id_is_amneziawg(action_id)) {
      if (g_strcmp0(exec_path, NETWORK_SIDEBAR_INSTALLED_AWG_HELPER_PATH) == 0 ||
          g_strcmp0(exec_argv1, NETWORK_SIDEBAR_INSTALLED_AWG_HELPER_PATH) == 0)
        safe = FALSE;
      continue;
    }
    if (manage_found)
      safe = FALSE;
    manage_found = TRUE;
    if (polkit_action_description_get_annotation(
          description, POLKIT_EXEC_PATH_ANNOTATION) != NULL ||
        polkit_action_description_get_annotation(
          description, POLKIT_EXEC_ARGV1_ANNOTATION) != NULL ||
        polkit_action_description_get_implicit_any(description) !=
          POLKIT_IMPLICIT_AUTHORIZATION_NOT_AUTHORIZED ||
        polkit_action_description_get_implicit_inactive(description) !=
          POLKIT_IMPLICIT_AUTHORIZATION_NOT_AUTHORIZED ||
        polkit_action_description_get_implicit_active(description) !=
          POLKIT_IMPLICIT_AUTHORIZATION_NOT_AUTHORIZED)
      safe = FALSE;
  }
  return manage_found && safe;
}

static void
policy_enumerated_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GTask) task = user_data;
  g_autoptr(GError) error = NULL;
  GList *descriptions = polkit_authority_enumerate_actions_finish(
    POLKIT_AUTHORITY(source), result, &error);
  gboolean safe = error == NULL && policy_descriptions_are_safe(descriptions);

  g_list_free_full(descriptions, g_object_unref);
  if (error != NULL)
    g_task_return_error(task, g_steal_pointer(&error));
  else
    g_task_return_boolean(task, safe);
}

void
network_sidebar_awg_policy_check_async(PolkitAuthority *authority,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
  GTask *task;

  g_return_if_fail(POLKIT_IS_AUTHORITY(authority));
  task = g_task_new(authority, cancellable, callback, user_data);
  g_task_set_source_tag(task, network_sidebar_awg_policy_check_async);
  polkit_authority_enumerate_actions(authority, cancellable,
                                     policy_enumerated_cb, task);
}

gboolean
network_sidebar_awg_policy_check_finish(PolkitAuthority *authority,
                                        GAsyncResult *result,
                                        GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, authority), FALSE);
  g_return_val_if_fail(g_async_result_is_tagged(
    result, network_sidebar_awg_policy_check_async), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
