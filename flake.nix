{
  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixpkgs-unstable";
  inputs.systems.url = "github:nix-systems/default";
  inputs.parts.url = "github:hercules-ci/flake-parts";
  inputs.parts.inputs.nixpkgs-lib.follows = "nixpkgs";

  outputs = inputs: inputs.parts.lib.mkFlake { inherit inputs; } {
    systems = import inputs.systems;

    perSystem = { lib, pkgs, ... }: {
      devShells.default = pkgs.mkShell {
        hardeningDisable = [ "all" ];
        env.KDIR = with pkgs.linuxPackages_latest.kernel; "${dev}/lib/modules/${modDirVersion}/build";
        packages = with pkgs; [
          bear
          gnumake
          llvmPackages.clang-tools
          meson
          ninja
          pkg-config
        ];
      };

      formatter = pkgs.writeShellScriptBin "formatter" ''
        set -eoux pipefail
        shopt -s globstar
        root="$PWD"
        while [[ ! -f "$root/.git/index" ]]; do
          if [[ "$root" == "/" ]]; then
            exit 1
          fi
          root="$(dirname "$root")"
        done
        pushd "$root" > /dev/null
        ${lib.getExe pkgs.deno} fmt readme.md
        ${lib.getExe pkgs.findutils} . -regex '.*\.\(c\|h\)' -exec ${lib.getExe' pkgs.clang-tools "clang-format"} -i {} \;
        ${lib.getExe pkgs.nixpkgs-fmt} .
        popd
      '';
    };
  };
}
