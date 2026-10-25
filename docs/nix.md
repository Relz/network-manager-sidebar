# Nix reference

See the [README](../README.md#nix) for installation and NixOS setup.

## Setup

Enable flake commands in `~/.config/nix/nix.conf` if necessary:

```ini
experimental-features = nix-command flakes
```

Without flakes, build with `nix-build` and run `./result/bin/nm-sidebar`; on NixOS, import `./nix/module.nix` from a checkout. Both traditional entry points (`nix-build`, `nix-shell`) share the flake's nixpkgs pin. Update it with `nix flake update` and commit `flake.lock`. Add new source files to Git so local flake commands include them.

The host needs running NetworkManager, system D-Bus, polkit, and a layer-shell Wayland session, with normal NetworkManager permissions and an authentication agent as needed. Non-NixOS systems may need [nixGL](https://github.com/nix-community/nixGL) for GPU drivers.

## NixOS

In your system's `flake.nix`, add the `nm-sidebar` input and pass `inputs` to configuration modules through `specialArgs`. For example, adapt this to your existing flake, keeping your nixpkgs input, host name, and system architecture:

```nix
{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  inputs.nm-sidebar.url = "github:Relz/network-manager-sidebar";

  outputs = inputs@{ nixpkgs, ... }: {
    nixosConfigurations.YOUR_HOST = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      specialArgs = { inherit inputs; };
      modules = [ ./configuration.nix ];
    };
  };
}
```

Add the [README's module import and enable options](../README.md#nixos) to `configuration.nix`. The `inputs` argument used there is supplied by `specialArgs`; it is not available automatically. Apply from the directory containing your system's `flake.nix`, replacing `YOUR_HOST` with your configuration name:

```sh
sudo nixos-rebuild switch --flake .#YOUR_HOST
```

## AmneziaWG

With the [NixOS module](../README.md#nixos), add and rebuild:

```nix
programs.nm-sidebar.amneziawg = {
  enable = true;
  authorizedUsers = [ "YOUR_USER" ];
};

# For profiles with DNS=:
services.resolved.enable = true;
networking.networkmanager.dns = "systemd-resolved";
```

The module registers an on-demand privileged D-Bus service using the root-owned system Nix store. Only listed users in active local sessions receive access; an empty list denies access. Installing the package in a user profile does not register the service. Tunnels do not start at boot.

Plain `nix run` provides the NetworkManager features, but does not register the AmneziaWG service. The documented AmneziaWG setup uses the NixOS module. On other distributions, you must separately register the privileged service and the D-Bus/polkit files from the package's `integration` output with the host system. Running the application and adding the README's authorization rule alone are insufficient. Privileged helpers verify executable ownership and require a root-owned Nix store; a user-owned single-user store cannot be used for this integration.

The package includes AmneziaWG tools and an `amneziawg-go` fallback. Set `programs.nm-sidebar.amneziawg.userspaceBackend = false;` to use an independently configured compatible kernel module; the module does not install one. Active userspace tunnels survive the service's idle exit and restart.

Profiles with `DNS=` require host resolver configuration. Running `systemd-resolved` is preferred; otherwise, configure a supported resolvconf backend. `programs.nm-sidebar.amneziawg.resolvconfPackage` defaults to `openresolv` and can select another package providing `sbin/resolvconf`.

## Development

Enter `nix develop` (or `nix-shell`) for build dependencies and the GTK environment. Configure once in a separate directory:

```sh
meson setup build-nix --prefix=/usr --libdir=lib --buildtype=debugoptimized
meson compile -C build-nix
./build-nix/nm-sidebar --help
./build-nix/nm-sidebar --show
./build-nix/nm-sidebar --quit
```

Run these binaries inside the shell; no installation is needed. Development GUI builds use the installed, authorized AmneziaWG service.

## Packaging notes

- `out` contains the application, helpers, and tool dependencies; `integration` contains AmneziaWG D-Bus/polkit registration and is installed by the module when enabled. Only the GUI is GTK-wrapped; the CLI remains GLib-only.
- `package.nix` is the shared `callPackage` definition; `default.nix` and `shell.nix` accept `pkgs`. Tool paths and subprocess `PATH` are fixed at build time via the `awg-*` Meson options. Profiles and keys stay in protected system directories.
- ARMv7 disables `rdma-core` man pages and omits `nixfmt` to avoid unbuildable GHC dependencies at the locked revision. Other supported platforms provide `nix fmt`.

## Validation

```sh
nix flake check --no-build --all-systems  # Evaluate all four platforms
nix flake check                         # Build package and module checks
nix develop --command true              # Realize the development shell
```

`nix build` also runs headless CLI/artifact checks. Module checks evaluate installation, service configuration, and authorization, not runtime D-Bus activation or tunnels. Builds for other architectures require a native/remote builder or emulation.

For a Wayland smoke test, run `./result/bin/nm-sidebar --show`, then `./result/bin/nm-sidebar --quit`, with no other sidebar instance. Use `./build-nix/nm-sidebar` for development.
