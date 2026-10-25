{
  pkgs ? null,
  package ? import ./default.nix { inherit pkgs; },
}:
let
  packageSet = import ./nix/nixpkgs.nix { inherit pkgs; };
in
packageSet.mkShell {
  inputsFrom = [ package ];
  packages = [
    packageSet.networkmanagerapplet
    packageSet.gdb
  ]
  # Match the flake's formatter platforms: the locked nixpkgs cannot
  # bootstrap nixfmt's GHC on ARMv7.
  ++ packageSet.lib.optional (builtins.elem packageSet.stdenv.hostPlatform.system [
    "x86_64-linux"
    "aarch64-linux"
    "riscv64-linux"
  ]) packageSet.nixfmt;

  # wrapGAppsHook configures installed executables during fixup, not programs
  # launched straight from a Meson development directory.
  shellHook = ''
    export XDG_DATA_DIRS="${
      packageSet.lib.makeSearchPath "share" [
        packageSet.gtk4
        packageSet.libadwaita
        packageSet.adwaita-icon-theme
      ]
    }:''${GSETTINGS_SCHEMAS_PATH:+$GSETTINGS_SCHEMAS_PATH:}''${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
  '';
}
