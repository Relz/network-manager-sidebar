project(
  'nm-sidebar',
  'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  license_files: ['LICENSE'],
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
  ],
)

glib_dep = dependency('glib-2.0', version: '>= 2.68')
gio_dep = dependency('gio-2.0', version: '>= 2.68')
gtk_dep = dependency('gtk4')
adw_dep = dependency('libadwaita-1', version: '>= 1.6')
nm_dep = dependency('libnm')
layer_shell_dep = dependency('gtk4-layer-shell-0')
polkit_dep = dependency('polkit-gobject-1')
json_dep = dependency('json-glib-1.0', version: '>= 1.6')
threads_dep = dependency('threads')
src_include = include_directories('src')

source_root = meson.project_source_root()
installed_css_path = join_paths(get_option('prefix'), get_option('datadir'), 'nm-sidebar', 'nm-sidebar.css')
installed_gui_path = join_paths(get_option('prefix'), get_option('libexecdir'), 'nm-sidebar', 'nm-sidebar-gui')
installed_awg_helper_path = join_paths(get_option('prefix'), get_option('libexecdir'), 'nm-sidebar', 'nm-sidebar-awg-helper')
installed_awg_service_path = join_paths(get_option('prefix'), get_option('libexecdir'), 'nm-sidebar', 'nm-sidebar-awg-service')

native_args = [
  '-DNETWORK_SIDEBAR_SOURCE_DIR="@0@"'.format(source_root),
  '-DNETWORK_SIDEBAR_INSTALLED_CSS_PATH="@0@"'.format(installed_css_path),
  '-DNETWORK_SIDEBAR_INSTALLED_GUI_PATH="@0@"'.format(installed_gui_path),
]

awg_service_args = [
  '-DNETWORK_SIDEBAR_INSTALLED_AWG_HELPER_PATH="@0@"'.format(installed_awg_helper_path),
]

amneziawg_sources = [
  'src/amneziawg/amneziawg.c',
  'src/amneziawg/deadline.c',
]

amneziawg_runtime_sources = [
  'src/amneziawg/link_identity.c',
  'src/amneziawg/runtime_journal.c',
]

amneziawg_config_sources = files(
  'src/amneziawg/config.c',
  'src/amneziawg/dns_config.c',
)

amneziawg_controller_sources = [
  'src/amneziawg/admission.c',
  'src/amneziawg/backend.c',
  'src/amneziawg/controller.c',
  'src/amneziawg/import_loader.c',
  'src/amneziawg/inventory.c',
  'src/amneziawg/model.c',
  'src/amneziawg/runtime_source.c',
]

amneziawg_gui_sources = [
  'src/gui/amneziawg_presenter.c',
]

amneziawg_controller_lib = static_library(
  'nm-sidebar-awg-controller',
  amneziawg_sources + amneziawg_runtime_sources + amneziawg_controller_sources,
  dependencies: [glib_dep, gio_dep, polkit_dep],
  include_directories: src_include,
)

amneziawg_messages_lib = static_library(
  'nm-sidebar-awg-messages',
  'src/gui/amneziawg_messages.c',
  dependencies: [glib_dep],
  include_directories: src_include,
)

core_sources = [
  'src/core/command_socket.c',
  'src/core/ipc.c',
  'src/core/ipc_commands.c',
  'src/core/ipc_paths.c',
  'src/core/target_output.c',
]

data_sources = [
  'src/data/connection_state.c',
  'src/data/labels.c',
]

action_sources = [
  'src/actions/network_actions.c',
]

section_sources = [
  'src/sections/amneziawg.c',
  'src/sections/connection_info.c',
  'src/sections/helpers.c',
  'src/sections/status.c',
  'src/sections/vpn.c',
  'src/sections/wifi.c',
]

executable(
  'nm-sidebar',
  ['src/cli/main.c', 'src/cli/cli_args.c'] + core_sources,
  dependencies: [glib_dep],
  c_args: native_args,
  include_directories: src_include,
  install: true,
  install_dir: get_option('bindir'),
)

executable(
  'nm-sidebar-gui',
  [
    'src/gui/app.c',
    'src/gui/command_server.c',
    'src/gui/layer_shell.c',
    'src/gui/main.c',
    'src/gui/styles.c',
  ] + core_sources + data_sources + action_sources + section_sources + amneziawg_gui_sources,
  dependencies: [glib_dep, gio_dep, gtk_dep, adw_dep, nm_dep, layer_shell_dep, polkit_dep],
  link_with: [amneziawg_controller_lib, amneziawg_messages_lib],
  c_args: native_args,
  include_directories: src_include,
  install: true,
  install_dir: join_paths(get_option('libexecdir'), 'nm-sidebar'),
)

executable(
  'nm-sidebar-awg-helper',
  [
    'src/helper/amneziawg_helper.c',
    'src/helper/amneziawg_helper_profiles.c',
    'src/helper/amneziawg_helper_remove.c',
    'src/helper/amneziawg_helper_quick.c',
    'src/helper/amneziawg_quick_config.c',
    'src/helper/amneziawg_quick_diagnostics.c',
    'src/helper/amneziawg_helper_session.c',
    'src/helper/amneziawg_helper_storage.c',
    'src/helper/amneziawg_helper_switch.c',
    'src/helper/amneziawg_helper_tunnel.c',
    'src/helper/amneziawg_helper_util.c',
    'src/helper/amneziawg_dns.c',
    'src/helper/amneziawg_firewall.c',
    'src/helper/amneziawg_tool.c',
    'src/helper/amneziawg_process.c',
    'src/helper/amneziawg_subprocess.c',
    'src/amneziawg/profile_report.c',
  ] + amneziawg_sources + amneziawg_runtime_sources + amneziawg_config_sources,
  dependencies: [glib_dep, gio_dep, threads_dep, json_dep],
  include_directories: src_include,
  install: true,
  install_dir: join_paths(get_option('libexecdir'), 'nm-sidebar'),
)

executable(
  'nm-sidebar-awg-service',
  [
    'src/helper/amneziawg_service.c',
    'src/helper/amneziawg_helper_runner.c',
    'src/helper/amneziawg_policy.c',
    'src/helper/amneziawg_supervisor.c',
    'src/amneziawg/profile_report.c',
  ] + amneziawg_sources,
  dependencies: [glib_dep, gio_dep, polkit_dep],
  c_args: awg_service_args,
  include_directories: src_include,
  install: true,
  install_dir: join_paths(get_option('libexecdir'), 'nm-sidebar'),
)

awg_policy = configure_file(
  input: 'data/dev.relz.NmSidebar.amneziawg.policy.in',
  output: 'dev.relz.NmSidebar.amneziawg.policy',
  copy: true,
)
install_data(
  awg_policy,
  install_dir: join_paths(get_option('datadir'), 'polkit-1', 'actions'),
)

awg_service_configuration = configuration_data()
awg_service_configuration.set('AWG_SERVICE_PATH', installed_awg_service_path)
awg_dbus_service = configure_file(
  input: 'data/dev.relz.NmSidebar.AmneziaWG.service.in',
  output: 'dev.relz.NmSidebar.AmneziaWG.service',
  configuration: awg_service_configuration,
)
install_data(
  awg_dbus_service,
  install_dir: join_paths(get_option('datadir'), 'dbus-1', 'system-services'),
)

install_data(
  'data/dev.relz.NmSidebar.AmneziaWG.conf',
  install_dir: join_paths(get_option('datadir'), 'dbus-1', 'system.d'),
)

install_data(
  'nm-sidebar.css',
  install_dir: join_paths(get_option('datadir'), 'nm-sidebar'),
)
