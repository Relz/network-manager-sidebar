# Repository Instructions

## Architecture

- Native C GTK4/libadwaita NetworkManager sidebar for Wayland; Gtk4LayerShell is required to show it.
- Meson installs four executables: GLib-only CLI `nm-sidebar`, GTK/libadwaita/libnm/gtk4-layer-shell GUI `nm-sidebar-gui`, and the privileged AmneziaWG service and helper from `src/helper/`.
- `src/core/` owns IPC, socket paths, commands, and target-output parsing; `src/gui/app.c` handles GTK application lifecycle and command dispatch; `src/gui/layer_shell.c` handles monitor anchoring.
- NetworkManager side effects belong in `src/actions/network_actions.c`; read-only data helpers live in `src/data/`; UI sections live in `src/sections/`; app CSS is `nm-sidebar.css`.

### AmneziaWG

- AmneziaWG tool adapters use `src/helper/amneziawg_subprocess.{c,h}` for fork/exec, pipes, bounded I/O, and cleanup; `src/helper/amneziawg_process.{c,h}` owns signal cancellation and group supervision. `src/helper/amneziawg_helper_session.{c,h}` reconciles journals with kernel links; `src/helper/amneziawg_helper_storage.{c,h}` owns filesystem access only. Resolver-specific commands stay private to `src/helper/amneziawg_dns.{c,h}`.
- `src/helper/amneziawg_service.c` owns D-Bus, authorization, and admission. All authorized user commands can preempt inventory through one pending-action slot, waiting for cleanup before execution. `src/helper/amneziawg_helper_runner.{c,h}` owns helper execution, supervision, deadlines, and reaping; an early inventory result does not release cleanup ownership. `src/amneziawg/profile_report.{c,h}` owns the shared bounded inventory wire codec.
- Shared AmneziaWG formats have separate owners: `src/amneziawg/config.{c,h}` parses configuration envelopes, `src/amneziawg/runtime_journal.{c,h}` owns journal encoding and pure runtime-state classification, and `src/amneziawg/dns_config.{c,h}` owns DNS values and their bounded payload codec. Tool-specific hook generation and its snapshot size reserve belong to `src/helper/amneziawg_quick_config.{c,h}`, alongside the `awg-quick` adapter in `src/helper/amneziawg_helper_quick.{c,h}`.
- `src/amneziawg/inventory.{c,h}` owns saved-profile refresh scheduling, retries, request cancellation/generations, capabilities, and inventory-result processing. `src/amneziawg/controller.{c,h}` gates inventory by authorization, visibility, and user activity, preempts the client wait when dispatching a command, and coordinates model/runtime presentation through inventory notifications. Selection/confirmation only suspends new refreshes; the authorized service command owns actual helper preemption.
- `src/amneziawg/runtime_source.{c,h}` distinguishes background polling from invalidation: polling retains monitored observations, while mutations, relevant monitor changes/loss, and failed observations invalidate them. `src/amneziawg/link_identity.{c,h}` filters link notifications by tracked names, indices, and ownership tokens; uncertain events still invalidate. Generations prevent a late poll result from restoring invalidated state.
- `src/amneziawg/admission.{c,h}` owns pure user-action eligibility and dependency exemptions. Snapshots expose action availability and blocked reasons for UI rendering; `src/amneziawg/controller.{c,h}` reuses the policy with current facts at admission and confirmation, and rechecks dependencies before dispatch.

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

- Configure the local development build once with `meson setup build --prefix=/usr --libdir=lib --buildtype=debugoptimized`.
- Focused native check after C or Meson edits: `meson compile -C build && sudo meson install -C build && nm-sidebar --help`.
- Wayland smoke test when a graphical session exists: `(nm-sidebar --show & pid=$!; sleep 2; nm-sidebar --quit; wait "$pid")`.
- `.github/workflows/build-packages.yml` runs only on `v*.*.*` tag pushes, rewrites the Meson project version from the tag, builds Meson artifacts, then uses nFPM for `.deb`, `.rpm`, `.pkg.tar.zst`, and `.apk`; AUR metadata publishes only when `AUR_SSH_PRIVATE_KEY` is configured.
- Packages install `/usr/bin/nm-sidebar`, the GUI and AmneziaWG helpers under `/usr/libexec/nm-sidebar/`, and application/D-Bus/polkit data under `/usr/share/`; packaged files must be covered by both Meson install rules and `packaging/nfpm.yaml`.
- Runtime dependency changes must update `README.md`, every distro dependency list in `packaging/nfpm.yaml`, and `packaging/aur/PKGBUILD.in`; CI/build dependency changes may also need `.github/workflows/build-packages.yml` and AUR `makedepends`.
- Local deb build: `rm -rf build-package package-root dist && meson setup build-package --prefix=/usr --libdir=lib --buildtype=release && meson compile -C build-package && DESTDIR="$PWD/package-root" meson install -C build-package && mkdir -p dist && PACKAGE_VERSION=0.0.0 PACKAGE_RELEASE=1 PACKAGE_ARCH=amd64 PACKAGE_HOMEPAGE=https://github.com/Relz/network-manager-sidebar nfpm package --config packaging/nfpm.yaml --packager deb --target dist/`
