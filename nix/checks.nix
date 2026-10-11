{
  nixpkgs,
  system,
}:
let
  pkgs = import ./nixpkgs.nix { pkgs = nixpkgs.legacyPackages.${system}; };
  inherit (pkgs) lib;

  evaluate =
    amneziawg:
    (nixpkgs.lib.nixosSystem {
      modules = [
        ./module.nix
        {
          nixpkgs.hostPlatform = system;
          nixpkgs.pkgs = pkgs;
          boot.isContainer = true;
          fileSystems."/" = {
            device = "none";
            fsType = "tmpfs";
          };
          system.stateVersion = "26.05";
          networking.networkmanager.enable = true;
          programs.nm-sidebar = {
            enable = true;
            inherit amneziawg;
          };
        }
      ];
    }).config;

  basic = evaluate { };
  amneziawgDefault = evaluate { enable = true; };
  authorizedUsers = [
    "alice"
    "bob"
  ];
  amneziawg = evaluate {
    enable = true;
    inherit authorizedUsers;
  };

  containsPackage = package: lib.any (entry: toString entry == toString package);
  moduleAssertions = config: lib.all (entry: entry.assertion) config.assertions;
  commonChecks = config: {
    "NixOS assertions pass" = moduleAssertions config;
    "application output is installed" =
      containsPackage config.programs.nm-sidebar.package.out config.environment.systemPackages;
    "polkit is enabled" = config.security.polkit.enable;
  };
  integrationChecks =
    config:
    let
      package = config.programs.nm-sidebar.package;
      service = config.systemd.services.nm-sidebar-amneziawg;
    in
    commonChecks config
    // {
      "integration output is installed" =
        containsPackage package.integration config.environment.systemPackages;
      "system D-Bus discovers integration" =
        containsPackage package.integration config.services.dbus.packages;
      "service uses the application executable" =
        service.serviceConfig.ExecStart == "${package.out}/libexec/nm-sidebar/nm-sidebar-awg-service";
      "service uses D-Bus activation" =
        service.serviceConfig.Type == "dbus"
        && service.serviceConfig.BusName == "dev.relz.NmSidebar.AmneziaWG";
      "service runs as root" =
        service.serviceConfig.User == "root" && service.serviceConfig.Group == "root";
      "service protects private files" = service.serviceConfig.UMask == "0077";
      "userspace backends survive service exit" = service.serviceConfig.KillMode == "process";
      "service requires the system bus" = lib.elem "dbus.service" service.requires;
      "service starts after D-Bus and polkit" = lib.all (unit: lib.elem unit service.after) [
        "dbus.service"
        "polkit.service"
      ];
      "service does not start at boot" = service.wantedBy == [ ] && service.requiredBy == [ ];
    };

  # Force every assertion before producing a derivation. This makes --no-build
  # useful, and the resulting checks do not depend on a NixOS system build.
  check =
    name: assertions:
    assert lib.all (label: lib.assertMsg assertions.${label} "${name}: ${label}") (
      builtins.attrNames assertions
    );
    pkgs.runCommand name { } ''
      touch "$out"
    '';
in
{
  nixos-module-basic = check "nm-sidebar-nixos-module-basic" (
    commonChecks basic
    // {
      "integration output is not installed" =
        !containsPackage basic.programs.nm-sidebar.package.integration basic.environment.systemPackages;
      "system D-Bus does not register integration" =
        !containsPackage basic.programs.nm-sidebar.package.integration basic.services.dbus.packages;
      "privileged service is not defined" = !(basic.systemd.services ? nm-sidebar-amneziawg);
      "no AmneziaWG authorization rule is added" =
        !lib.hasInfix "dev.relz.NmSidebar.manage-amneziawg" basic.security.polkit.extraConfig;
    }
  );
  nixos-module-amneziawg = check "nm-sidebar-nixos-module-amneziawg" (
    integrationChecks amneziawg
    // lib.mapAttrs' (label: value: lib.nameValuePair "default authorization: ${label}" value) (
      integrationChecks amneziawgDefault
    )
    // {
      "default authorized users are empty" =
        amneziawgDefault.programs.nm-sidebar.amneziawg.authorizedUsers == [ ];
      "empty authorized users add no rule" =
        !lib.hasInfix "dev.relz.NmSidebar.manage-amneziawg" amneziawgDefault.security.polkit.extraConfig;
      "authorization is scoped to the AmneziaWG action" =
        lib.hasInfix ''action.id == "dev.relz.NmSidebar.manage-amneziawg"'' amneziawg.security.polkit.extraConfig;
      "authorization checks the explicit user list" =
        lib.hasInfix "${builtins.toJSON authorizedUsers}.indexOf(subject.user) !== -1" amneziawg.security.polkit.extraConfig;
      "authorization requires an active local session" =
        lib.hasInfix "subject.local && subject.active" amneziawg.security.polkit.extraConfig;
      "matching sessions receive authorization" =
        lib.hasInfix "return polkit.Result.YES;" amneziawg.security.polkit.extraConfig;
    }
  );
}
