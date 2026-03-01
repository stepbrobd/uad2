{ config, pkgs, ... }:

{
  boot = {
    kernelModules = [ "uad2" ];
    extraModulePackages = [
      (pkgs.callPackage ./default.nix {
        linuxPackages = config.boot.kernelPackages;
      })
    ];
  };
}
