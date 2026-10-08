{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  wrapGAppsHook4,
  glib,
  gtk4,
  libadwaita,
  networkmanager,
  gtk4-layer-shell,
  polkit,
  json-glib,
  adwaita-icon-theme,
  networkmanagerapplet,
  amneziawg-tools,
  amneziawg-go,
  bash,
  coreutils,
  gnugrep,
  gnused,
  gawk,
  findutils,
  procps,
  iproute2,
  nftables,
  iptables,
  openresolv,
  withUserspaceBackend ? true,
  resolvconfPackage ? openresolv,
}:
let
  versionLine =
    lib.findFirst (line: builtins.match "[[:space:]]*version:.*" line != null)
      (throw "The Meson project version is missing")
      (lib.splitString "\n" (builtins.readFile ./meson.build));
  # This is also the inspection path: inspection and awg-quick must see the
  # same firewall implementations, rather than the invoking user's PATH.
  awgTools = [
    amneziawg-tools
    bash
    coreutils
    gnugrep
    gnused
    gawk
    findutils
    procps
    iproute2
    nftables
    iptables
    resolvconfPackage
  ]
  ++ lib.optional withUserspaceBackend amneziawg-go;
  awgPath = lib.makeBinPath awgTools + ":" + lib.makeSearchPath "sbin" (map lib.getBin awgTools);
  awgIntegrationFiles = [
    "share/dbus-1/system-services/dev.relz.NmSidebar.AmneziaWG.service"
    "share/dbus-1/system.d/dev.relz.NmSidebar.AmneziaWG.conf"
    "share/polkit-1/actions/dev.relz.NmSidebar.amneziawg.policy"
  ];
in
stdenv.mkDerivation {
  pname = "nm-sidebar";
  version = builtins.head (builtins.match "[[:space:]]*version: '([^']+)',[[:space:]]*" versionLine);
  outputs = [
    "out"
    "integration"
  ];

  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./meson.build
      ./meson_options.txt
      ./src
      ./data
      ./nm-sidebar.css
      ./LICENSE
    ];
  };

  strictDeps = true;
  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wrapGAppsHook4
  ];
  buildInputs = [
    glib
    gtk4
    libadwaita
    networkmanager
    gtk4-layer-shell
    polkit
    json-glib
    adwaita-icon-theme
  ];

  mesonFlags = [
    "-Dawg-store-paths=@prefix@,${lib.concatMapStringsSep "," toString awgTools}"
    "-Dawg-quick-path=${lib.getExe' amneziawg-tools "awg-quick"}"
    "-Dawg-resolvconf-path=${resolvconfPackage}/sbin/resolvconf"
    "-Dawg-tool-path=${awgPath}"
    "-Dawg-subprocess-path=${awgPath}"
    "-Dawg-systemd-service=nm-sidebar-amneziawg.service"
  ];

  # NixOS also discovers D-Bus files through environment.systemPackages.
  # Keep registration separate from the application output so the module can
  # opt in without rebuilding a different variant of the application.
  postInstall = ''
    for path in ${lib.escapeShellArgs awgIntegrationFiles}; do
      moveToOutput "$path" "$integration"
    done
  '';

  # Automatic wrapping includes every bin/libexec executable. Only the GUI
  # needs GTK variables; the CLI and both privileged programs remain native.
  dontWrapGApps = true;
  preFixup = ''
    wrapGApp "$out/libexec/nm-sidebar/nm-sidebar-gui" \
      --prefix PATH : ${lib.makeBinPath [ networkmanagerapplet ]}
  '';

  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck
    env -u DISPLAY -u WAYLAND_DISPLAY "$out/bin/nm-sidebar" --help
    runtime=$(mktemp -d)
    if XDG_RUNTIME_DIR="$runtime" "$out/bin/nm-sidebar" --hide >probe.log 2>&1; then
      echo "An IPC-only command unexpectedly succeeded without a listener" >&2
      exit 1
    fi
    grep -F "no running instance" probe.log
    rmdir "$runtime"
    test -x "$out/libexec/nm-sidebar/nm-sidebar-gui"
    test -x "$out/libexec/nm-sidebar/nm-sidebar-awg-service"
    test -x "$out/libexec/nm-sidebar/nm-sidebar-awg-helper"
    test -f "$out/share/nm-sidebar/nm-sidebar.css"
    for path in ${lib.escapeShellArgs awgIntegrationFiles}; do
      test ! -e "$out/$path"
      test -f "$integration/$path"
    done
    grep -Fx "Exec=$out/libexec/nm-sidebar/nm-sidebar-awg-service" \
      "$integration/share/dbus-1/system-services/dev.relz.NmSidebar.AmneziaWG.service"
    grep -Fx "SystemdService=nm-sidebar-amneziawg.service" \
      "$integration/share/dbus-1/system-services/dev.relz.NmSidebar.AmneziaWG.service"
    if readelf -d "$out/bin/nm-sidebar" | grep -E 'NEEDED.*(libgtk|libadwaita|libnm\.)'; then
      echo "The CLI must remain GTK/libnm-free" >&2
      exit 1
    fi
    runHook postInstallCheck
  '';

  passthru = {
    inherit withUserspaceBackend;
  };

  meta = {
    description = "GTK4/libadwaita NetworkManager sidebar for layer-shell Wayland desktops";
    homepage = "https://github.com/Relz/network-manager-sidebar";
    license = lib.licenses.gpl3Plus;
    mainProgram = "nm-sidebar";
    outputsToInstall = [ "out" ];
    platforms = lib.platforms.linux;
  };
}
