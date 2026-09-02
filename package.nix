{
  stdenv,
  lib,
  cmake,
  pkg-config,
  rvmi,
  dtc,
  zlib,
  boost,
  autoreconfHook,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "whisper";
  version = "1.0.0";

  # FIXME: I am too lazy to enumerate all files from root dir in lib.fileset.toSource
  src = ./.;

  outputs = [
    "out"
  ];

  cmakeFlags = [
    (lib.cmakeBool "BUILD_TESTING" finalAttrs.finalPackage.doCheck)
    (lib.cmakeFeature "MODEL_LAUNCHER_TESTS_DIR" "${rvmi.out}/share/testgen-model-interface/rvm")
    (lib.cmakeFeature "CMAKE_MODULE_PATH" "${rvmi.dev}/lib/cmake/testgen-model-interface")
  ];

  nativeBuildInputs = [
    cmake
    dtc
    pkg-config
    rvmi
  ];

  strictDeps = true;

  buildInputs = [
    rvmi
    boost
    zlib
  ];
  installPhase = ''
    runHook preInstall
    mkdir -p $out/lib
    install -Dm644 whisper-model-build/*.so $out/lib/
    runHook postInstall
  '';

  # TODO: Can't run tests for now because we need cross clang/gcc stdenvs for
  # freestanding riscv32 and riscv64.
  doCheck = false;

  meta.mainProgram = "whisper";
})
