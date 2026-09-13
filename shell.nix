{ pkgs ? import <nixpkgs> { } }:

let
  pkg = pkgs.callPackage ./package.nix { };
in
pkgs.mkShell {
  inputsFrom = [ pkg ];
  packages = with pkgs; [
    gdb
    valgrind
  ];
}
