{
  pkgs ? null,
}:
let
  packageSet = import ./nix/nixpkgs.nix { inherit pkgs; };
in
packageSet.callPackage ./package.nix { }
