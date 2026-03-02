{ lib
, stdenv
, linuxPackages
}:

stdenv.mkDerivation {
  pname = "uad2";
  version = with lib; pipe ./uad2.c [
    readFile
    (splitString "\n")
    (filter (hasPrefix "MODULE_VERSION"))
    head
    (match ''MODULE_VERSION\("([^"]+)"\);'')
    head
  ];

  src = with lib.fileset; toSource {
    root = ./.;
    fileset = unions [
      ./uad2.c
      ./Kbuild
      ./Makefile
    ];
  };

  nativeBuildInputs = linuxPackages.kernel.moduleBuildDependencies;

  makeFlags = linuxPackages.kernel.commonMakeFlags ++ [
    "KDIR=${linuxPackages.kernel.dev}/lib/modules/${linuxPackages.kernel.modDirVersion}/build"
    "INSTALL_MOD_PATH=$(out)"
  ];
}
