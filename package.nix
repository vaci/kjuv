{
  capnproto
    , cmake
  , ekam
  , gtest
  , libuv
  , stdenv
  , which

  , debug ? false
}:

let
  create-ekam-rules-link = ''
    ln --symbolic --force --target-directory=src \
      "${ekam.src}/src/ekam/rules"
  '';


in stdenv.mkDerivation {

  name = "kjuv";
  src = ./.;

  buildInputs = [
    capnproto
    libuv
  ];

  nativeBuildInputs = [
    cmake
    ekam
    gtest 
  ];

  shellHook = create-ekam-rules-link;

  buildPhase = ''
    ${create-ekam-rules-link}
    make ${if debug then "debug" else "release"}

    
  '';

  installPhase = ''
    mkdir --parents $out/lib $out/include
    cp tmp/kjuv. $out/bin
  '';
}
