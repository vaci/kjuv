{
  description = "kjuv";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-25.11";
  };

  outputs = { nixpkgs, ...}:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
    in
      {
        defaultPackage.x86_64-linux =
          with import nixpkgs { system = "x86_64-linux"; };
          callPackage ./package.nix { };

        devShells.x86_64-linux.default = pkgs.mkShell {
          buildInputs = [
            pkgs.cmake
            pkgs.capnproto
            pkgs.openssl
            pkgs.gcc
            pkgs.libuv
            pkgs.gtest
            pkgs.pkg-config
          ];

          shellHook = ''
            export LIBUV_SRC=${pkgs.libuv.src}
            export LIBUV_DIR=${pkgs.libuv}
            export LIBUV_INCLUDE_DIRS="${pkgs.libuv.src}/include"
          '';

        };
      };
}
