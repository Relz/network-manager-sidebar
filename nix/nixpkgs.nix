{
  system ? builtins.currentSystem,
  pkgs ? null,
}:
let
  # Traditional Nix commands use the same pin as the flake without requiring
  # flake support or an independently maintained nixpkgs hash.
  lock = builtins.fromJSON (builtins.readFile ../flake.lock);
  input = lock.nodes.nixpkgs.locked;
  source = builtins.fetchTarball {
    url = "https://github.com/${input.owner}/${input.repo}/archive/${input.rev}.tar.gz";
    sha256 = input.narHash;
  };
  packageSet = if pkgs == null then import source { inherit system; } else pkgs;
in
if packageSet.stdenv.hostPlatform.system == "armv7l-linux" then
  # GTK's dependency closure reaches rdma-core through libpcap. Its man-page
  # generator needs Pandoc/GHC, which cannot bootstrap on ARMv7 at this pin.
  # Keep the runtime libraries intact and omit only those dependency manuals.
  packageSet.extend (
    _final: prev: {
      rdma-core = prev.rdma-core.override { withManPages = false; };
    }
  )
else
  packageSet
