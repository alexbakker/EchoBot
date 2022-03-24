with import <nixpkgs> {
  overlays = [ (import ./overlay) ];
};

mkShell {
  buildInputs = [
    gcc
    gnumake
    libtoxcore
    libmsgpack
    libsodium
  ];
}
