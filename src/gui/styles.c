#include "gui/styles.h"

#include "core/config.h"

#include <errno.h>
#include <gtk/gtk.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  NetworkSidebarErrorCallback report_error;
  gpointer user_data;
  const char *css_path;
  gboolean required;
  gboolean failed;
} CssLoadData;

typedef enum {
  CSS_LOAD_FAILED,
  CSS_LOAD_MISSING,
  CSS_LOAD_LOADED,
} CssLoadStatus;

static GtkCssProvider *user_css_provider = NULL;
static GdkDisplay *user_css_display = NULL;

static void
report_css_problem(gboolean required,
                   NetworkSidebarErrorCallback report_error,
                   gpointer user_data,
                   const char *message)
{
  if (required) {
    if (report_error != NULL)
      report_error(message, user_data);
    return;
  }

  g_printerr("nm-sidebar: warning: %s\n", message);
}

static void
on_css_parsing_error(GtkCssProvider *provider, GtkCssSection *section, const GError *error, gpointer user_data)
{
  CssLoadData *data = user_data;
  (void) provider;
  (void) section;

  if (data->failed)
    return;
  data->failed = TRUE;
  g_autofree char *message = data->required
                               ? g_strdup_printf("error: failed to load CSS stylesheet %s: %s",
                                                 data->css_path,
                                                 error != NULL ? error->message : "unknown error")
                               : g_strdup_printf("failed to load user CSS stylesheet %s: %s",
                                                 data->css_path,
                                                 error != NULL ? error->message : "unknown error");
  report_css_problem(data->required, data->report_error, data->user_data, message);
}

static char *
resolve_css_path(void)
{
  g_autofree char *dev_path = g_build_filename(NETWORK_SIDEBAR_SOURCE_DIR, "nm-sidebar.css", NULL);

  if (g_file_test(dev_path, G_FILE_TEST_IS_REGULAR))
    return g_steal_pointer(&dev_path);
  if (g_file_test(NETWORK_SIDEBAR_INSTALLED_CSS_PATH, G_FILE_TEST_IS_REGULAR))
    return g_strdup(NETWORK_SIDEBAR_INSTALLED_CSS_PATH);
  return g_steal_pointer(&dev_path);
}

static char *
resolve_user_css_path(void)
{
  return g_build_filename(g_get_user_config_dir(), "nm-sidebar", "nm-sidebar.css", NULL);
}

static CssLoadStatus
load_css_provider(const char *css_path,
                  gboolean required,
                  NetworkSidebarErrorCallback report_error,
                  gpointer user_data,
                  GtkCssProvider **provider_out)
{
  struct stat css_stat;
  GtkCssProvider *provider;
  gulong parsing_error_handler;
  CssLoadData load_data = { 0 };

  g_return_val_if_fail(provider_out != NULL, CSS_LOAD_FAILED);
  *provider_out = NULL;

  if (stat(css_path, &css_stat) != 0) {
    int stat_errno = errno;
    g_autofree char *message = NULL;

    if (!required && stat_errno == ENOENT)
      return CSS_LOAD_MISSING;

    message = required ? g_strdup_printf("error: CSS stylesheet not found: %s", css_path)
                       : g_strdup_printf("cannot inspect user CSS stylesheet %s: %s", css_path, g_strerror(stat_errno));
    report_css_problem(required, report_error, user_data, message);
    return CSS_LOAD_FAILED;
  }
  if (!S_ISREG(css_stat.st_mode)) {
    g_autofree char *message = required ? g_strdup_printf("error: CSS stylesheet is not a regular file: %s", css_path)
                                        : g_strdup_printf("user CSS stylesheet is not a regular file: %s", css_path);
    report_css_problem(required, report_error, user_data, message);
    return CSS_LOAD_FAILED;
  }
  if (access(css_path, R_OK) != 0) {
    g_autofree char *message = required ? g_strdup_printf("error: cannot read CSS stylesheet %s: %s", css_path, g_strerror(errno))
                                        : g_strdup_printf("cannot read user CSS stylesheet %s: %s", css_path, g_strerror(errno));
    report_css_problem(required, report_error, user_data, message);
    return CSS_LOAD_FAILED;
  }

  provider = gtk_css_provider_new();
  load_data.report_error = report_error;
  load_data.user_data = user_data;
  load_data.css_path = css_path;
  load_data.required = required;
  parsing_error_handler = g_signal_connect(provider, "parsing-error", G_CALLBACK(on_css_parsing_error), &load_data);
  gtk_css_provider_load_from_path(provider, css_path);
  g_signal_handler_disconnect(provider, parsing_error_handler);
  if (load_data.failed) {
    g_object_unref(provider);
    return CSS_LOAD_FAILED;
  }

  *provider_out = provider;
  return CSS_LOAD_LOADED;
}

static void
clear_user_css_provider(void)
{
  if (user_css_provider != NULL && user_css_display != NULL)
    gtk_style_context_remove_provider_for_display(user_css_display, GTK_STYLE_PROVIDER(user_css_provider));
  g_clear_object(&user_css_provider);
  g_clear_object(&user_css_display);
}

static void
set_user_css_provider(GdkDisplay *display, GtkCssProvider *provider)
{
  clear_user_css_provider();
  gtk_style_context_add_provider_for_display(display,
                                             GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_USER);
  user_css_provider = provider;
  user_css_display = g_object_ref(display);
}

void
network_sidebar_install_application_css(NetworkSidebarErrorCallback report_error, gpointer user_data)
{
  GdkDisplay *display = gdk_display_get_default();
  g_autofree char *css_path = NULL;
  GtkCssProvider *provider = NULL;

  if (display == NULL)
    return;

  css_path = resolve_css_path();
  if (load_css_provider(css_path,
                        TRUE,
                        report_error,
                        user_data,
                        &provider) != CSS_LOAD_LOADED)
    return;
  gtk_style_context_add_provider_for_display(display,
                                             GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);

  network_sidebar_reload_user_css(report_error, user_data);
}

void
network_sidebar_reload_user_css(NetworkSidebarErrorCallback report_error, gpointer user_data)
{
  GdkDisplay *display = gdk_display_get_default();
  CssLoadStatus status;
  GtkCssProvider *provider = NULL;
  g_autofree char *user_css_path = NULL;

  if (display == NULL)
    return;

  user_css_path = resolve_user_css_path();
  status = load_css_provider(user_css_path,
                             FALSE,
                             report_error,
                             user_data,
                             &provider);
  if (status == CSS_LOAD_MISSING) {
    clear_user_css_provider();
    return;
  }
  if (status != CSS_LOAD_LOADED)
    return;

  set_user_css_provider(display, provider);
}
