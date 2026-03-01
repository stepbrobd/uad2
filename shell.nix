{ mkShell
, linuxPackages
, bear
, gnumake
, llvmPackages
, meson
, ninja
}:

mkShell {
  env.KDIR = with linuxPackages.kernel; "${dev}/lib/modules/${modDirVersion}/build";

  packages = [
    bear
    gnumake
    llvmPackages.bintools
    llvmPackages.clang-tools
    meson
    ninja
  ];
}
