#include "sections/helpers.h"

GtkWidget *
network_sidebar_section_group(const char *title)
{
  GtkWidget *group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), title);
  return group;
}

GtkWidget *
network_sidebar_action_row(const char *title, const char *subtitle, const char *icon_name)
{
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title != NULL ? title : "Unknown");
  if (subtitle != NULL)
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  if (icon_name != NULL) {
    GtkWidget *image = gtk_image_new_from_icon_name(icon_name);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), image);
  }
  return row;
}

GtkWidget *
network_sidebar_flat_button(const char *icon_name, const char *tooltip)
{
  GtkWidget *button = gtk_button_new_from_icon_name(icon_name);
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  if (tooltip != NULL)
    gtk_widget_set_tooltip_text(button, tooltip);
  return button;
}

void
network_sidebar_apply_row_state(GtkWidget *row, NetworkSidebarRowState state)
{
  static const char *classes[] = {
    "active-connection-row",
    "connecting-connection-row",
    "failed-connection-row",
    "disconnected-connection-row",
  };
  const char *current = NULL;

  switch (state) {
  case NETWORK_SIDEBAR_ROW_STATE_ACTIVE:
    current = "active-connection-row";
    break;
  case NETWORK_SIDEBAR_ROW_STATE_CONNECTING:
    current = "connecting-connection-row";
    break;
  case NETWORK_SIDEBAR_ROW_STATE_FAILED:
    current = "failed-connection-row";
    break;
  case NETWORK_SIDEBAR_ROW_STATE_DISCONNECTED:
    current = "disconnected-connection-row";
    break;
  default:
    break;
  }

  /* Retain the current class so a repeated update cannot restart its animation. */
  for (guint i = 0; i < G_N_ELEMENTS(classes); i++) {
    if (g_strcmp0(classes[i], current) != 0)
      gtk_widget_remove_css_class(row, classes[i]);
  }
  if (current != NULL && !gtk_widget_has_css_class(row, current))
    gtk_widget_add_css_class(row, current);
}

void
network_sidebar_clear_box(GtkBox *box)
{
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(box));
  while (child != NULL) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_box_remove(box, child);
    child = next;
  }
}

void
network_sidebar_add_notice(AdwPreferencesGroup *group, const char *title, const char *subtitle, const char *icon_name)
{
  GtkWidget *row;
  (void) icon_name;

  row = network_sidebar_action_row(title, subtitle, NULL);
  adw_preferences_group_add(group, row);
}
