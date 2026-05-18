# Repository Instructions

## Architecture

- Native C GTK4/libadwaita NetworkManager sidebar for Wayland; keep Gtk4LayerShell unless explicitly asked otherwise.
- Root `nm-sidebar` is a checkout launcher; it execs `build/nm-sidebar` and fails until the Meson build exists.
- Meson builds two binaries: CLI `nm-sidebar` from `src/cli/` + `src/core/` with only GLib, and GUI helper `nm-sidebar-gui` from `src/gui/` plus NetworkManager/UI modules with GTK/libadwaita/libnm/gtk4-layer-shell.
- `src/core/` owns IPC protocol, socket paths, commands, and target-output selection; `src/gui/app.c` and `src/gui/main.c` own GTK/libadwaita startup and command dispatch; `src/gui/layer_shell.c` owns layer-shell anchoring.
- NetworkManager side effects go through `src/actions/network_actions.c`; read-only data helpers live under `src/data/`; UI sections stay under `src/sections/`; CSS is `nm-sidebar.css`.

## Runtime

- Running with no args defaults to `--toggle`; `--toggle`, `--show`, and `--background` may start the GUI helper after IPC probing, while `--hide` and `--quit` are IPC-only.
- Keep the CLI GTK/libnm-free: `--help` and IPC attempts must not initialize GTK or require GUI-only dependencies.
- Gtk4LayerShell may be unavailable at startup time but is required to show the sidebar; do not add a non-layer-shell fallback unless asked.
- Guard Gtk4LayerShell API compatibility: `KeyboardMode.ON_DEMAND` depends on protocol version support, and optional calls such as `gtk_layer_set_respect_close()` must stay version-guarded.
- Socket path is `$XDG_RUNTIME_DIR/nm-sidebar.sock`, falling back to `/tmp/nm-sidebar-$UID/nm-sidebar.sock`; preserve owner/type/permission/symlink checks plus startup-lock and stale-socket safety in `src/core/ipc_paths.c`, `src/core/command_socket.c`, and `src/gui/command_server.c`.
- Output targeting comes from env: `NM_SIDEBAR_OUTPUT` wins over `WAYBAR_OUTPUT_NAME`; the CLI sends it over IPC and the GUI app re-anchors via Gtk4LayerShell monitor connector lookup.

## NetworkManager

- Use `nm-connection-editor` for advanced profile create/edit/import flows instead of building full NetworkManager profile editors here.
- Ask before adding new behavior that disables networking/Wi-Fi, disconnects connections, or removes active connections unless the user explicitly requested it.
- Never hard-code Wi-Fi passwords, VPN credentials, or user-specific NetworkManager connection data.

## Local Setup

- `/home/relz/.local/bin/nm-sidebar` execs this checkout; Waybar calls it from the `network` and `custom/vpn` modules in `/home/relz/.config/waybar/config`.
- If changing `/home/relz/.config/waybar/config`, reload Waybar with `pkill -SIGUSR2 -x waybar`.

## Build And Packaging

- There is no test-suite config; do not invent `pytest`, `ruff`, or package-manager commands.
- After native edits: `meson setup build && meson compile -C build && build/nm-sidebar --help`; CLI wiring check: `/home/relz/.local/bin/nm-sidebar --help`.
- Wayland smoke test when a graphical session exists: `(/home/relz/.local/bin/nm-sidebar --show & pid=$!; sleep 2; /home/relz/.local/bin/nm-sidebar --quit; wait $pid)`.
- `.github/workflows/build-packages.yml` runs only for `v*.*.*` tag pushes; it builds Meson artifacts before nFPM builds `.deb`, `.rpm`, `.pkg.tar.zst`, and `.apk` artifacts.
- Packages use `packaging/nfpm.yaml`; `/usr/bin/nm-sidebar` is the native CLI, `/usr/libexec/nm-sidebar/nm-sidebar-gui` is the GUI helper, and CSS lives under `/usr/share/nm-sidebar/`.
- If adding packaged runtime files, update Meson install rules and `packaging/nfpm.yaml` as needed.
- If runtime dependencies change, update both `README.md` requirements and every distro dependency list in `packaging/nfpm.yaml`.
- Local package build: `rm -rf build-package package-root dist && meson setup build-package --prefix=/usr --libdir=lib --buildtype=release && meson compile -C build-package && DESTDIR="$PWD/package-root" meson install -C build-package && mkdir -p dist && PACKAGE_VERSION=0.0.0 PACKAGE_RELEASE=1 PACKAGE_ARCH=amd64 nfpm package --config packaging/nfpm.yaml --packager deb --target dist/`
