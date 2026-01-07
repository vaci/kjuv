{
  description = "kjuv";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-25.11";
  };

  outputs = { nixpkgs, ...}:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      ccache = pkgs.lib.getExe pkgs.ccache;
    in
      {
        defaultPackage.${system} =
          pkgs.callPackage ./package.nix { };

        devShells.x86_64-linux.default = pkgs.mkShell {
          buildInputs = [
            pkgs.cmake
            pkgs.capnproto
            pkgs.ccache
            pkgs.openssl
            pkgs.gcc
            pkgs.libuv.dev
            pkgs.gtest
            pkgs.ninja
            pkgs.pkg-config
          ];

          shellHook = ''
            export CMAKE_C_COMPILER_LAUNCHER=${ccache}
            export CMAKE_CXX_COMPILER_LAUNCHER=${ccache}
          '';
        };
      };
}
