{
  description = "Network Manager Sidebar: native GTK4/libadwaita sidebar for Wayland";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
        "armv7l-linux"
        "riscv64-linux"
      ];
      # The locked nixpkgs cannot bootstrap nixfmt's GHC on ARMv7.
      formatterSystems = [
        "x86_64-linux"
        "aarch64-linux"
        "riscv64-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
      packagesFor =
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          package = import ./default.nix { inherit pkgs; };
        in
        {
          default = package;
          nm-sidebar = package;
        };
    in
    {
      packages = forAllSystems packagesFor;
      apps = forAllSystems (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/nm-sidebar";
          meta.description = "Launch or control the Network Manager Sidebar";
        };
      });
      devShells = forAllSystems (system: {
        default = import ./shell.nix {
          pkgs = nixpkgs.legacyPackages.${system};
          package = self.packages.${system}.default;
        };
      });
      checks = forAllSystems (
        system:
        {
          package = self.packages.${system}.default;
        }
        // import ./nix/checks.nix { inherit nixpkgs system; }
      );
      formatter = nixpkgs.lib.genAttrs formatterSystems (system: nixpkgs.legacyPackages.${system}.nixfmt);
      overlays.default = final: _prev: {
        nm-sidebar = import ./default.nix { pkgs = final; };
      };
      nixosModules.default = import ./nix/module.nix;
    };
}
