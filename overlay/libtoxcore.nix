{ lib, stdenv, fetchFromGitHub, cmake, libsodium, ncurses, libopus, msgpack
, libvpx, check, libconfig, pkg-config }:

stdenv.mkDerivation {
  pname = "libtoxcore";
  version = "0.2.17.1337";

  src = fetchFromGitHub {
    owner  = "jfreegman";
    repo   = "toxcore";
    rev    = "8fe3bbd37256c2193d1d2479116d5856b9317f52";
    sha256 = "sha256-rnMZF90jUzOsYRwckLqUA9qpbfYm+JzjGNw6qraz0tM=";
  };

  cmakeFlags = [
    "-DBUILD_NTOX=ON"
    "-DDHT_BOOTSTRAP=ON"
    "-DBOOTSTRAP_DAEMON=ON"
  ];

  buildInputs = [
    libsodium msgpack ncurses libconfig
  ] ++ lib.optionals (!stdenv.isAarch32) [
    libopus libvpx
  ];

  nativeBuildInputs = [ cmake pkg-config ];

  doCheck = false; # hangs, tries to access the net?
  checkInputs = [ check ];

  postFixup =''
    sed -i $out/lib/pkgconfig/*.pc \
      -e "s|^libdir=.*|libdir=$out/lib|" \
      -e "s|^includedir=.*|includedir=$out/include|"
  '';

  meta = with lib; {
    description = "P2P FOSS instant messaging application aimed to replace Skype";
    homepage = "https://tox.chat";
    license = licenses.gpl3Plus;
    maintainers = with maintainers; [ peterhoeg ];
    platforms = platforms.all;
  };
}
