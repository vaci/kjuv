{
  pkgs ? import <nixpkgs> {}
}:

{
  pykj = pkgs.callPackage ./package.nix {};
}
