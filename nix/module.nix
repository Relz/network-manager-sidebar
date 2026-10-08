{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.programs.nm-sidebar;
  packageSet = import ./nixpkgs.nix { inherit pkgs; };
  applicationPackage = cfg.package.out or cfg.package;
  integrationPackage = cfg.package.integration or cfg.package;
in
{
  options.programs.nm-sidebar = {
    enable = lib.mkEnableOption "Network Manager Sidebar";
    package = lib.mkOption {
      type = lib.types.package;
      default = packageSet.callPackage ../package.nix {
        withUserspaceBackend = cfg.amneziawg.userspaceBackend;
        resolvconfPackage = cfg.amneziawg.resolvconfPackage;
      };
      description = "Sidebar package with out and integration outputs; uses the host's nixpkgs and can be overridden.";
    };
    amneziawg = {
      enable = lib.mkEnableOption "privileged AmneziaWG management through system D-Bus";
      authorizedUsers = lib.mkOption {
        type = lib.types.listOf lib.types.str;
        default = [ ];
        example = [ "alice" ];
        description = "Users allowed to manage AmneziaWG from an active local session. Empty denies access by default.";
      };
      userspaceBackend = lib.mkOption {
        type = lib.types.bool;
        default = true;
        description = "Include amneziawg-go as a fallback when the kernel module is unavailable.";
      };
      resolvconfPackage = lib.mkOption {
        type = lib.types.package;
        default = pkgs.openresolv;
        description = "Fallback package providing sbin/resolvconf, which must match the host resolver configuration. Running systemd-resolved is preferred automatically.";
      };
    };
  };

  config = lib.mkIf cfg.enable (
    lib.mkMerge [
      {
        assertions = [
          {
            assertion = config.networking.networkmanager.enable;
            message = "nm-sidebar requires the existing NixOS NetworkManager service (networking.networkmanager.enable).";
          }
        ];
        environment.systemPackages = [ applicationPackage ];
        security.polkit.enable = true;
      }
      (lib.mkIf cfg.amneziawg.enable {
        assertions = [
          {
            assertion = cfg.package ? integration;
            message = "nm-sidebar AmneziaWG integration requires a package with an integration output.";
          }
        ];
        # D-Bus reads the dedicated output directly; polkit discovers its
        # action through the system profile's share/polkit-1 directory.
        environment.systemPackages = [ integrationPackage ];
        services.dbus.packages = [ integrationPackage ];
        systemd.services.nm-sidebar-amneziawg = {
          description = "Network Manager Sidebar AmneziaWG service";
          requires = [ "dbus.service" ];
          after = [
            "dbus.service"
            "polkit.service"
          ];
          serviceConfig = {
            Type = "dbus";
            BusName = "dev.relz.NmSidebar.AmneziaWG";
            ExecStart = "${applicationPackage}/libexec/nm-sidebar/nm-sidebar-awg-service";
            User = "root";
            Group = "root";
            UMask = "0077";
            # The runner cleans up in-flight helpers, but released userspace
            # backends must survive the D-Bus service's idle exit and restart.
            KillMode = "process";
          };
        };
        security.polkit.extraConfig = lib.mkIf (cfg.amneziawg.authorizedUsers != [ ]) ''
          polkit.addRule(function(action, subject) {
            if (action.id == "dev.relz.NmSidebar.manage-amneziawg" &&
                ${builtins.toJSON cfg.amneziawg.authorizedUsers}.indexOf(subject.user) !== -1 &&
                subject.local && subject.active) {
              return polkit.Result.YES;
            }
          });
        '';
      })
    ]
  );
}
