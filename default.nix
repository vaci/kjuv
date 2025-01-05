{
  pkgs ? import <nixpkgs> {}
}:

{
  kjuv = pkgs.callPackage ./package.nix {};
}
