#include "core/target_output.h"
#include "gui/app.h"

int
main(int argc, char **argv)
{
  g_autofree char *target_output_name = network_sidebar_target_output_from_environment();
  NetworkSidebarGuiApp *app = network_sidebar_gui_app_new(target_output_name);
  return network_sidebar_gui_app_run(app, argc, argv);
}
