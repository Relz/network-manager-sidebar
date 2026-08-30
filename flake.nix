{
  description = "Native GTK4/libadwaita NetworkManager sidebar for Wayland desktops";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
        "armv7l-linux"
        "riscv64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs supportedSystems (system: f {
        pkgs = import nixpkgs { inherit system; };
        inherit system;
      });
    in
    {
      packages = forAllSystems ({ pkgs, ... }: {
        default = self.packages.${pkgs.system}.nm-sidebar;
        nm-sidebar = pkgs.callPackage ./package.nix { };
      });

      apps = forAllSystems ({ pkgs, ... }: {
        default = {
          type = "app";
          program = "${self.packages.${pkgs.system}.default}/bin/nm-sidebar";
        };
      });

      devShells = forAllSystems ({ pkgs, ... }: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.system}.default ];
          packages = with pkgs; [
            gdb
            valgrind
          ];
        };
      });

      overlays.default = final: prev: {
        nm-sidebar = final.callPackage ./package.nix { };
      };
    };
}
