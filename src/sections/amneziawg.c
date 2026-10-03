#include "sections/amneziawg.h"

#include "gui/amneziawg_messages.h"
#include "sections/helpers.h"

typedef struct {
  NetworkSidebarAwgPresenter *presenter;
  char *name;
  gboolean activate;
  GtkWidget *remove_button;
  GtkWidget *cancel_button;
  gboolean seen;
} AmneziawgRowAction;

typedef struct {
  NetworkSidebarAwgPresenter *presenter;
  GtkWidget *notices;
  GtkWidget *profiles;
  GtkWidget *spinner;
  GtkWidget *import_button;
  /* The list box owns the rows; each row owns its action data. */
  GHashTable *rows;
} AmneziawgSection;

static void
amneziawg_section_free(gpointer user_data)
{
  AmneziawgSection *data = user_data;

  g_hash_table_unref(data->rows);
  network_sidebar_awg_presenter_unref(data->presenter);
  g_free(data);
}

static void
amneziawg_row_action_free(AmneziawgRowAction *data)
{
  network_sidebar_awg_presenter_unref(data->presenter);
  g_free(data->name);
  g_free(data);
}

static void
amneziawg_row_action_closure_notify(gpointer data, GClosure *closure)
{
  (void) closure;
  amneziawg_row_action_free(data);
}

static void
presenter_closure_notify(gpointer data, GClosure *closure)
{
  (void) closure;
  network_sidebar_awg_presenter_unref(data);
}

static const char *
profile_status_subtitle(NetworkSidebarAmneziaWGProfileStatus status)
{
  switch (status) {
  case NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_FILE_SECURITY:
    return "File must be root:root, regular, single-link, and mode 0600";
  case NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_INVALID_CONFIGURATION:
    return "Configuration failed safety validation or exceeds the size limit";
  case NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_INCOMPATIBLE_CONFIGURATION:
    return "Installed awg-quick rejected the configuration";
  case NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_CHANGED_OR_UNREADABLE:
    return "Configuration changed or could not be read safely";
  case NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_AVAILABLE:
  default:
    return NULL;
  }
}

static void
on_import_clicked(GtkButton *button, gpointer user_data)
{
  (void) button;
  network_sidebar_awg_presenter_import(user_data);
}

static void
on_import_cancel_clicked(GtkButton *button, gpointer user_data)
{
  (void) button;
  network_sidebar_awg_presenter_cancel_import(user_data);
}

static GtkWidget *
import_cancel_button(NetworkSidebarAwgPresenter *presenter)
{
  GtkWidget *button = network_sidebar_flat_button(
    "process-stop-symbolic", "Cancel configuration import");

  g_signal_connect_data(
    button,
    "clicked",
    G_CALLBACK(on_import_cancel_clicked),
    network_sidebar_awg_presenter_ref(presenter),
    presenter_closure_notify,
    0);
  return button;
}

static void
on_amneziawg_row_activated(GtkListBoxRow *row, gpointer user_data)
{
  AmneziawgRowAction *data = user_data;
  (void) row;

  network_sidebar_awg_presenter_set_active(data->presenter,
                                           data->name,
                                           data->activate);
}

static void
on_amneziawg_remove_clicked(GtkButton *button, gpointer user_data)
{
  AmneziawgRowAction *data = user_data;
  (void) button;

  network_sidebar_awg_presenter_request_delete(data->presenter, data->name);
}

static gboolean
activity_is_import_flow(NetworkSidebarAwgActivity activity)
{
  return activity == NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_SELECT ||
         activity == NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_LOAD ||
         activity == NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT ||
         activity == NETWORK_SIDEBAR_AWG_ACTIVITY_REPLACE_CONFIRM ||
         activity == NETWORK_SIDEBAR_AWG_ACTIVITY_REPLACE;
}

static const char *
action_blocked_tooltip(const NetworkSidebarAwgSnapshot *snapshot,
                       const NetworkSidebarAwgActionAvailability *availability)
{
  if (availability->blocked_reason == NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_BUSY &&
      (snapshot->activity == NETWORK_SIDEBAR_AWG_ACTIVITY_UP ||
       snapshot->activity == NETWORK_SIDEBAR_AWG_ACTIVITY_DOWN))
    return NULL;
  return network_sidebar_awg_message_for_action_blocked(availability);
}

static const char *
activity_pending_subtitle(NetworkSidebarAwgActivity activity)
{
  switch (activity) {
  case NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_LOAD:
    return "Reading configuration...";
  case NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT:
    return "Importing...";
  case NETWORK_SIDEBAR_AWG_ACTIVITY_REPLACE_CONFIRM:
    return "Waiting for confirmation...";
  case NETWORK_SIDEBAR_AWG_ACTIVITY_REPLACE:
    return "Replacing...";
  case NETWORK_SIDEBAR_AWG_ACTIVITY_UP:
    return "";
  case NETWORK_SIDEBAR_AWG_ACTIVITY_DOWN:
    return "";
  case NETWORK_SIDEBAR_AWG_ACTIVITY_DELETE:
    return "";
  case NETWORK_SIDEBAR_AWG_ACTIVITY_IDLE:
  case NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_SELECT:
  case NETWORK_SIDEBAR_AWG_ACTIVITY_DELETE_CONFIRM:
  default:
    return NULL;
  }
}

static const char *
entry_pending_subtitle(const NetworkSidebarAwgSnapshot *snapshot,
                       const NetworkSidebarAwgEntry *entry)
{
  if (!entry->pending)
    return NULL;
  return activity_pending_subtitle(snapshot->activity);
}

static void
add_dependency_notices(AdwPreferencesGroup *group, NetworkSidebarAwgDependency missing)
{
  if ((missing & NETWORK_SIDEBAR_AWG_DEPENDENCY_HELPER) != 0)
    network_sidebar_add_notice(group,
                               "Privileged helper unavailable",
                               "Install the root-owned nm-sidebar AmneziaWG helper",
                               "dialog-warning-symbolic");
  if ((missing & NETWORK_SIDEBAR_AWG_DEPENDENCY_SERVICE) != 0)
    network_sidebar_add_notice(group,
                               "Privileged service unavailable",
                               "Install the nm-sidebar AmneziaWG system service",
                               "dialog-warning-symbolic");
  if ((missing & NETWORK_SIDEBAR_AWG_DEPENDENCY_AWG_QUICK) != 0)
    network_sidebar_add_notice(group,
                               "AmneziaWG tools unavailable",
                               "Install awg-quick to activate AmneziaWG tunnels",
                               "dialog-warning-symbolic");
}

static gint
profile_row_compare(GtkListBoxRow *left, GtkListBoxRow *right, gpointer user_data)
{
  (void) user_data;
  const char *left_name = adw_preferences_row_get_title(ADW_PREFERENCES_ROW(left));
  const char *right_name = adw_preferences_row_get_title(ADW_PREFERENCES_ROW(right));
  gint folded = g_ascii_strcasecmp(left_name, right_name);

  return folded != 0 ? folded : g_strcmp0(left_name, right_name);
}

GtkWidget *
network_sidebar_amneziawg_section_new(NetworkSidebarAwgPresenter *presenter)
{
  AmneziawgSection *data;
  GtkWidget *section;
  GtkWidget *header;
  GtkWidget *title_box;
  GtkWidget *title;

  g_return_val_if_fail(presenter != NULL, NULL);

  data = g_new0(AmneziawgSection, 1);
  data->presenter = network_sidebar_awg_presenter_ref(presenter);
  data->rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  g_object_set_data_full(G_OBJECT(section), "amneziawg-section", data,
                         amneziawg_section_free);
  data->notices = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  data->profiles = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(data->profiles), GTK_SELECTION_NONE);
  gtk_list_box_set_sort_func(GTK_LIST_BOX(data->profiles), profile_row_compare,
                            NULL, NULL);
  gtk_widget_add_css_class(data->profiles, "boxed-list");
  header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  title_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  title = gtk_label_new("AmneziaWG");
  data->import_button = network_sidebar_flat_button(
    "document-open-symbolic", "Import AmneziaWG configuration");

  gtk_widget_set_hexpand(title_box, TRUE);
  gtk_widget_set_valign(title_box, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(title, "heading");
  gtk_widget_add_css_class(title, "h4");
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_box_append(GTK_BOX(title_box), title);
  gtk_box_append(GTK_BOX(header), title_box);
  gtk_accessible_update_relation(GTK_ACCESSIBLE(data->profiles),
                                 GTK_ACCESSIBLE_RELATION_LABELLED_BY,
                                 title, NULL,
                                 -1);

  data->spinner = adw_spinner_new();
  gtk_widget_set_size_request(data->spinner, 16, 16);
  gtk_widget_set_halign(data->spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(data->spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(data->spinner, FALSE);
  gtk_box_append(GTK_BOX(title_box), data->spinner);
  g_signal_connect_data(
    data->import_button,
    "clicked",
    G_CALLBACK(on_import_clicked),
    network_sidebar_awg_presenter_ref(presenter),
    presenter_closure_notify,
    0);
  gtk_box_append(GTK_BOX(header), data->import_button);
  gtk_box_append(GTK_BOX(section), header);
  gtk_box_append(GTK_BOX(section), data->notices);
  gtk_box_append(GTK_BOX(section), data->profiles);
  return section;
}

static GtkWidget *
ensure_profile_row(AmneziawgSection *data, const char *name)
{
  GtkWidget *row = g_hash_table_lookup(data->rows, name);
  AmneziawgRowAction *action;

  if (row == NULL) {
    row = network_sidebar_action_row(name, NULL, "network-vpn-symbolic");
    action = g_new0(AmneziawgRowAction, 1);
    action->presenter = network_sidebar_awg_presenter_ref(data->presenter);
    action->name = g_strdup(name);
    action->remove_button = network_sidebar_flat_button("user-trash-symbolic", "Remove");
    action->cancel_button = import_cancel_button(data->presenter);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), action->remove_button);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), action->cancel_button);
    g_signal_connect(action->remove_button, "clicked",
                     G_CALLBACK(on_amneziawg_remove_clicked), action);
    g_signal_connect_data(row, "activated",
                          G_CALLBACK(on_amneziawg_row_activated), action,
                          amneziawg_row_action_closure_notify, 0);
    g_object_set_data(G_OBJECT(row), "amneziawg-action", action);
    gtk_list_box_append(GTK_LIST_BOX(data->profiles), row);
    g_hash_table_insert(data->rows, g_strdup(name), row);
  }
  action = g_object_get_data(G_OBJECT(row), "amneziawg-action");
  action->seen = TRUE;
  return row;
}

void
network_sidebar_amneziawg_section_update(
  GtkWidget *section,
  const NetworkSidebarAwgSnapshot *snapshot)
{
  AmneziawgSection *data;
  NetworkSidebarAwgDependency missing;
  NetworkSidebarAwgDependency displayed_missing;
  gboolean inventory_loading;
  gboolean confirming;
  guint entry_count;
  gboolean activity_name_listed = FALSE;
  g_autofree char *profiles_warning = NULL;
  GtkWidget *group;
  const char *import_tooltip;
  GHashTableIter iter;
  gpointer value;

  g_return_if_fail(section != NULL && snapshot != NULL);
  data = g_object_get_data(G_OBJECT(section), "amneziawg-section");
  g_return_if_fail(data != NULL);

  gtk_widget_set_visible(data->spinner,
    snapshot->authorized && snapshot->activity == NETWORK_SIDEBAR_AWG_ACTIVITY_IDLE &&
    (snapshot->inventory_loading || snapshot->runtime_loading));
  gtk_widget_set_tooltip_text(data->spinner, snapshot->inventory_loading ?
                              "Refreshing profiles" : "Checking runtime state");

  if (!snapshot->authorized) {
    g_hash_table_iter_init(&iter, data->rows);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      gtk_list_box_remove(GTK_LIST_BOX(data->profiles), GTK_WIDGET(value));
      g_hash_table_iter_remove(&iter);
    }
    network_sidebar_clear_box(GTK_BOX(data->notices));
    gtk_widget_set_visible(data->profiles, FALSE);
    gtk_widget_set_visible(data->notices, FALSE);
    return;
  }

  missing = snapshot->missing_dependencies;
  displayed_missing = snapshot->capabilities_known ? missing :
    missing & NETWORK_SIDEBAR_AWG_DEPENDENCY_SERVICE;
  inventory_loading = snapshot->inventory_loading;
  confirming = snapshot->activity == NETWORK_SIDEBAR_AWG_ACTIVITY_REPLACE_CONFIRM ||
               snapshot->activity == NETWORK_SIDEBAR_AWG_ACTIVITY_DELETE_CONFIRM;
  entry_count = snapshot->entries != NULL ? snapshot->entries->len : 0;
  profiles_warning = network_sidebar_awg_message_for_profile_warning(
    &snapshot->profiles_warning);
  gtk_widget_set_sensitive(data->import_button, snapshot->import_action.allowed);
  import_tooltip = action_blocked_tooltip(snapshot, &snapshot->import_action);
  if (snapshot->import_action.blocked_reason == NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_BUSY &&
      activity_is_import_flow(snapshot->activity))
    import_tooltip = "An AmneziaWG import is already in progress";
  gtk_widget_set_tooltip_text(data->import_button, import_tooltip != NULL ?
                              import_tooltip : "Import AmneziaWG configuration");

  network_sidebar_clear_box(GTK_BOX(data->notices));
  group = network_sidebar_section_group("");
  gtk_box_append(GTK_BOX(data->notices), group);
  g_hash_table_iter_init(&iter, data->rows);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    AmneziawgRowAction *action = g_object_get_data(G_OBJECT(value), "amneziawg-action");
    action->seen = FALSE;
  }

  add_dependency_notices(ADW_PREFERENCES_GROUP(group), displayed_missing);
  if (inventory_loading && entry_count == 0)
    network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group),
                               "Refreshing profiles",
                               "Reading saved configurations without exposing their contents",
                               "content-loading-symbolic");
  else if (snapshot->runtime_loading && entry_count == 0)
    network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group),
                               "Checking runtime state",
                               "Verifying tunnel interfaces and runtime records",
                               "content-loading-symbolic");
  else if (!inventory_loading && profiles_warning != NULL)
    network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group),
                               "Profile list incomplete",
                               profiles_warning,
                               "dialog-warning-symbolic");

  for (guint i = 0; i < entry_count; i++) {
    const NetworkSidebarAwgEntry *entry =
      g_ptr_array_index(snapshot->entries, i);
    const char *name;

    if (entry == NULL || entry->name == NULL)
      continue;
    name = entry->name;
    if (snapshot->activity_name != NULL &&
        g_strcmp0(snapshot->activity_name, name) == 0)
      activity_name_listed = TRUE;
    NetworkSidebarAmneziaWGRuntimeState runtime_state =
      entry->display_runtime_state;
    gboolean active = runtime_state == NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_ACTIVE;
    gboolean recovery = network_sidebar_amneziawg_runtime_state_is_recoverable(runtime_state);
    gboolean firewall_pending = runtime_state == NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_FIREWALL_PENDING;
    gboolean manual = runtime_state == NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_MANUAL_INTERVENTION;
    gboolean conflict = runtime_state == NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_CONFLICT;
    gboolean unknown = runtime_state == NETWORK_SIDEBAR_AMNEZIAWG_RUNTIME_UNKNOWN;
    gboolean checking = unknown && snapshot->runtime_pending;
    gboolean profile_available = entry->has_profile &&
      entry->profile_status == NETWORK_SIDEBAR_AMNEZIAWG_PROFILE_AVAILABLE;
    const char *pending_subtitle = entry_pending_subtitle(snapshot, entry);
    const char *subtitle;
    const char *tooltip;
    GtkWidget *row;
    AmneziawgRowAction *row_action;

    if (pending_subtitle != NULL)
      subtitle = pending_subtitle;
    else if (active)
      subtitle = NULL;
    else if (firewall_pending)
      subtitle = "Firewall verification pending; select to recheck";
    else if (recovery)
      subtitle = "Recovery required; select to clean up";
    else if (manual)
      subtitle = "Manual cleanup required; runtime state retained";
    else if (conflict)
      subtitle = "Externally managed interface conflict";
    else if (checking)
      subtitle = "Checking runtime state...";
    else if (unknown)
      subtitle = "Runtime state unavailable";
    else if (!profile_available)
      subtitle = profile_status_subtitle(entry->profile_status);
    else
      subtitle = NULL;

    row = ensure_profile_row(data, name);
    row_action = g_object_get_data(G_OBJECT(row), "amneziawg-action");
    row_action->activate = entry->primary_action.action == NETWORK_SIDEBAR_AWG_ACTION_UP;
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle != NULL ? subtitle : "");
    if (pending_subtitle != NULL && !confirming)
      network_sidebar_apply_row_state(row, NETWORK_SIDEBAR_ROW_STATE_CONNECTING);
    else if (active)
      network_sidebar_apply_row_state(row, NETWORK_SIDEBAR_ROW_STATE_ACTIVE);
    else if (checking)
      network_sidebar_apply_row_state(row, NETWORK_SIDEBAR_ROW_STATE_NONE);
    else if (recovery || manual || conflict || unknown || !profile_available)
      network_sidebar_apply_row_state(row, NETWORK_SIDEBAR_ROW_STATE_FAILED);
    else
      network_sidebar_apply_row_state(row, NETWORK_SIDEBAR_ROW_STATE_NONE);

    gboolean loading = snapshot->activity == NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_LOAD &&
                       entry->pending;
    gtk_widget_set_visible(row_action->cancel_button, loading);
    gtk_widget_set_visible(row_action->remove_button, !loading && entry->remove_action.offered);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_widget_set_sensitive(row, loading || entry->primary_action.offered || entry->remove_action.offered);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row),
                                     !loading && entry->primary_action.offered && entry->primary_action.allowed);

    tooltip = action_blocked_tooltip(snapshot, &entry->primary_action);
    if (tooltip == NULL && entry->primary_action.allowed) {
      if (firewall_pending)
        tooltip = "Retry firewall inspection; remaining rules require administrator cleanup";
      else if (recovery)
        tooltip = "A previous operation was interrupted; select to retry owned cleanup";
    }
    gtk_widget_set_tooltip_text(row, loading ? NULL : tooltip);
    gtk_widget_set_sensitive(row_action->remove_button, entry->remove_action.allowed);
    tooltip = action_blocked_tooltip(snapshot, &entry->remove_action);
    gtk_widget_set_tooltip_text(row_action->remove_button, tooltip != NULL ? tooltip :
                                active ? "Disconnect and remove" :
                                recovery ? "Clean up and remove" : "Remove");
  }

  if (activity_is_import_flow(snapshot->activity) &&
      snapshot->activity_name != NULL && !activity_name_listed &&
      activity_pending_subtitle(snapshot->activity) != NULL) {
    GtkWidget *row = ensure_profile_row(data, snapshot->activity_name);
    AmneziawgRowAction *action = g_object_get_data(G_OBJECT(row), "amneziawg-action");

    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), activity_pending_subtitle(snapshot->activity));
    network_sidebar_apply_row_state(row, confirming ? NETWORK_SIDEBAR_ROW_STATE_NONE :
                                    NETWORK_SIDEBAR_ROW_STATE_CONNECTING);
    gtk_widget_set_sensitive(row, TRUE);
    gtk_widget_set_tooltip_text(row, NULL);
    gtk_widget_set_visible(action->remove_button, FALSE);
    gtk_widget_set_visible(action->cancel_button,
                            snapshot->activity == NETWORK_SIDEBAR_AWG_ACTIVITY_IMPORT_LOAD);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
  } else if (entry_count == 0 && !inventory_loading && !snapshot->runtime_loading &&
              profiles_warning == NULL) {
    const char *subtitle = missing == NETWORK_SIDEBAR_AWG_DEPENDENCY_NONE ?
      "Import a .conf file to add one" :
      "Install the required components, then import a .conf file";
    network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group),
                               "No AmneziaWG tunnels",
                               subtitle,
                               "network-vpn-symbolic");
  }

  g_hash_table_iter_init(&iter, data->rows);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    AmneziawgRowAction *action = g_object_get_data(G_OBJECT(value), "amneziawg-action");

    if (!action->seen) {
      gtk_list_box_remove(GTK_LIST_BOX(data->profiles), GTK_WIDGET(value));
      g_hash_table_iter_remove(&iter);
    }
  }
  gtk_widget_set_visible(data->profiles, g_hash_table_size(data->rows) != 0);
  gtk_widget_set_visible(data->notices,
    displayed_missing != NETWORK_SIDEBAR_AWG_DEPENDENCY_NONE ||
    (inventory_loading && entry_count == 0) ||
    (snapshot->runtime_loading && entry_count == 0) ||
    (!inventory_loading && profiles_warning != NULL) ||
    (entry_count == 0 && g_hash_table_size(data->rows) == 0));
}
