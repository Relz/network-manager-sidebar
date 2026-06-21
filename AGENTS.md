# Repository Instructions

## Architecture

- Native C GTK4/libadwaita NetworkManager sidebar for Wayland; Gtk4LayerShell is required to show it.
- Root `nm-sidebar` is a checkout launcher for `build/nm-sidebar`; it fails until `meson setup build && meson compile -C build` has run.
- Meson builds two executables: CLI `nm-sidebar` from `src/cli/` + `src/core/` with only GLib, and GUI helper `nm-sidebar-gui` from `src/gui/` plus GTK/libadwaita/libnm/gtk4-layer-shell UI modules.
- `src/core/` owns IPC, socket paths, commands, and target-output parsing; `src/gui/app.c` handles GTK application lifecycle and command dispatch; `src/gui/layer_shell.c` handles monitor anchoring.
- NetworkManager side effects belong in `src/actions/network_actions.c`; read-only data helpers live in `src/data/`; UI sections live in `src/sections/`; app CSS is `nm-sidebar.css`.

## Runtime

- Running with no args defaults to `--toggle`; `--toggle`, `--show`, and `--background` may start the GUI helper, while `--hide`, `--quit`, and `--reload-css` require an existing IPC listener.
- Keep the CLI GTK/libnm-free: `--help` and failed IPC probes must not initialize GTK or require GUI-only dependencies.
- Do not add a non-layer-shell fallback unless explicitly requested; keep Gtk4LayerShell compatibility guards such as protocol-gated keyboard mode and version-gated `gtk_layer_set_respect_close()`.
- Command socket path is `$XDG_RUNTIME_DIR/nm-sidebar.sock`, falling back to `/tmp/nm-sidebar-$UID/nm-sidebar.sock`; preserve owner/type/permission/symlink checks, startup locks, stale-socket probing, and safe unlink behavior.
- Output targeting comes from env: `NM_SIDEBAR_OUTPUT` wins over `WAYBAR_OUTPUT_NAME`; the CLI forwards it over IPC and the GUI re-anchors by GDK monitor connector.
- CSS loads from the source-tree `nm-sidebar.css` in dev or installed `/usr/share/nm-sidebar/nm-sidebar.css`; user overrides use `$XDG_CONFIG_HOME/nm-sidebar/nm-sidebar.css` via `--reload-css`.

## NetworkManager

- Use `nm-connection-editor` for advanced profile create/edit/import flows instead of building full NetworkManager profile editors here.
- Ask before adding new behavior that disables networking/Wi-Fi, disconnects connections, or removes active connections unless the user explicitly requested it.
- Never hard-code Wi-Fi passwords, VPN credentials, or user-specific NetworkManager connection data.

## Build And Packaging

- There is no test-suite config; do not invent `pytest`, `ruff`, or package-manager commands.
- Focused native check after C or Meson edits: `meson setup build && meson compile -C build && build/nm-sidebar --help`.
- Wayland smoke test when a graphical session exists: `(./nm-sidebar --show & pid=$!; sleep 2; ./nm-sidebar --quit; wait $pid)`.
- `.github/workflows/build-packages.yml` runs only on `v*.*.*` tag pushes, rewrites the Meson project version from the tag, builds Meson artifacts, then uses nFPM for `.deb`, `.rpm`, `.pkg.tar.zst`, and `.apk`; AUR metadata publishes only when `AUR_SSH_PRIVATE_KEY` is configured.
- Packages install `/usr/bin/nm-sidebar`, `/usr/libexec/nm-sidebar/nm-sidebar-gui`, and `/usr/share/nm-sidebar/nm-sidebar.css`; packaged files must be covered by both Meson install rules and `packaging/nfpm.yaml`.
- Runtime dependency changes must update `README.md`, every distro dependency list in `packaging/nfpm.yaml`, and `packaging/aur/PKGBUILD.in`; CI/build dependency changes may also need `.github/workflows/build-packages.yml` and AUR `makedepends`.
- Local deb build: `rm -rf build-package package-root dist && meson setup build-package --prefix=/usr --libdir=lib --buildtype=release && meson compile -C build-package && DESTDIR="$PWD/package-root" meson install -C build-package && mkdir -p dist && PACKAGE_VERSION=0.0.0 PACKAGE_RELEASE=1 PACKAGE_ARCH=amd64 PACKAGE_HOMEPAGE=https://github.com/Relz/network-manager-sidebar nfpm package --config packaging/nfpm.yaml --packager deb --target dist/`
