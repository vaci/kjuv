{
    capnproto
  , ekam
  , gtest
  , libuv
  , stdenv
  , which

  , debug ? true
}:

let
  create-ekam-rules-link = ''
    ln --symbolic --force --target-directory=src \
      "${ekam.src}/src/ekam/rules"
  '';


in stdenv.mkDerivation {

  name = "kjuv";
  src = ./.;

  CXXFLAGS = "-ggdb --std=c++20 -DCAPNP_INCLUDE_PATH=${capnproto}/include";

 
  buildInputs = [
    capnproto
    libuv
  ];

  nativeBuildInputs = [
    ekam
    gtest 
    which
  ];

  shellHook = create-ekam-rules-link;

  buildPhase = ''
    ${create-ekam-rules-link}
    make ${if debug then "debug" else "release"}
  '';
}
