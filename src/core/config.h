#ifndef NETWORK_SIDEBAR_CONFIG_H
#define NETWORK_SIDEBAR_CONFIG_H

#define NETWORK_SIDEBAR_APP_ID "dev.relz.NmSidebar"
#define NETWORK_SIDEBAR_APP_NAME "Network Manager Sidebar"
#define NETWORK_SIDEBAR_SOCKET_NAME "nm-sidebar.sock"
#define NETWORK_SIDEBAR_WIDTH 440
#define NETWORK_SIDEBAR_LAYER_SHELL_REQUIRED_MESSAGE "Gtk4LayerShell support is required to show nm-sidebar"

#ifndef NETWORK_SIDEBAR_SOURCE_DIR
#define NETWORK_SIDEBAR_SOURCE_DIR "."
#endif

#ifndef NETWORK_SIDEBAR_INSTALLED_CSS_PATH
#define NETWORK_SIDEBAR_INSTALLED_CSS_PATH "/usr/share/nm-sidebar/nm-sidebar.css"
#endif

#ifndef NETWORK_SIDEBAR_INSTALLED_GUI_PATH
#define NETWORK_SIDEBAR_INSTALLED_GUI_PATH "/usr/libexec/nm-sidebar/nm-sidebar-gui"
#endif

#endif
