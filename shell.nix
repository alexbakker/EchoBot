with import <nixpkgs> {};
mkShell {
  buildInputs = [
    gcc
    gnumake
    libtoxcore
    libmsgpack
    libsodium
  ];
}
