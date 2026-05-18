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

glib_dep = dependency('glib-2.0')
gio_dep = dependency('gio-2.0')
gtk_dep = dependency('gtk4')
adw_dep = dependency('libadwaita-1')
nm_dep = dependency('libnm')
layer_shell_dep = dependency('gtk4-layer-shell-0')

source_root = meson.project_source_root()
installed_css_path = join_paths(get_option('prefix'), get_option('datadir'), 'nm-sidebar', 'nm-sidebar.css')
installed_gui_path = join_paths(get_option('prefix'), get_option('libexecdir'), 'nm-sidebar', 'nm-sidebar-gui')

native_args = [
  '-DNETWORK_SIDEBAR_SOURCE_DIR="@0@"'.format(source_root),
  '-DNETWORK_SIDEBAR_INSTALLED_CSS_PATH="@0@"'.format(installed_css_path),
  '-DNETWORK_SIDEBAR_INSTALLED_GUI_PATH="@0@"'.format(installed_gui_path),
]

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
  include_directories: include_directories('src'),
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
  ] + core_sources + data_sources + action_sources + section_sources,
  dependencies: [glib_dep, gio_dep, gtk_dep, adw_dep, nm_dep, layer_shell_dep],
  c_args: native_args,
  include_directories: include_directories('src'),
  install: true,
  install_dir: join_paths(get_option('libexecdir'), 'nm-sidebar'),
)

install_data(
  'nm-sidebar.css',
  install_dir: join_paths(get_option('datadir'), 'nm-sidebar'),
)
