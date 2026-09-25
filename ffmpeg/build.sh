#!/bin/bash
# Builds a real ffmpeg with AGC-1 wired in as a proper libavcodec
# encoder/decoder (agc1), installed to its own prefix -- never touching the
# system ffmpeg/libavcodec. See ../README.md ("Embedding AGC-1: the ffmpeg
# codec") for the full story; this script is the reproducible version of
# everything done by hand in that section.
#
# Requires: a Debian/Ubuntu-family system where `apt-get source ffmpeg`
# matches the installed ffmpeg version (this repo was built against
# ffmpeg 7:5.1.9-0+deb12u1 on Debian 12 / bookworm), and the mesa-adreno
# GPU stack this repo already depends on for agc.c itself.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_ROOT="${AGC1_BUILD_ROOT:-$HOME/build}"
INSTALL_PREFIX="${AGC1_INSTALL_PREFIX:-$HOME/build/ffmpeg-agc1-install}"
JOBS="${AGC1_MAKE_JOBS:-2}"   # keep low: this device is RAM-constrained

echo "==> AGC-1 depends on the exact ffmpeg source matching your installed"
echo "    binary, so this must run where ffmpeg is already installed."
ffmpeg -version | head -1

if ! grep -q '^deb-src' /etc/apt/sources.list 2>/dev/null; then
    echo "==> Enabling deb-src (needed for 'apt-get source ffmpeg')"
    awk '/^deb /{print; sub(/^deb /,"deb-src "); print; next} {print}' \
        /etc/apt/sources.list > /tmp/sources.list.agc1
    sudo cp /tmp/sources.list.agc1 /etc/apt/sources.list 2>/dev/null \
        || cp /tmp/sources.list.agc1 /etc/apt/sources.list
    apt-get update
fi

mkdir -p "$BUILD_ROOT"
cd "$BUILD_ROOT"
if [ ! -d ffmpeg-5.1.9 ]; then
    echo "==> Fetching ffmpeg source matching the installed binary"
    apt-get source ffmpeg
fi
if ! (fakeroot apt-get build-dep ffmpeg -y); then
    echo "!! fakeroot apt-get build-dep failed -- install ffmpeg's build"
    echo "   dependencies yourself, then re-run this script."
    exit 1
fi

SRC="$BUILD_ROOT"/ffmpeg-5.1.9
cd "$SRC"

echo "==> Applying the AGC-1 registration patch"
patch -p1 -N --forward < "$HERE/agc1-ffmpeg.patch" || {
    echo "   (already applied, or the tree has diverged -- check by hand)"
}
cp "$HERE/../agc_core.h" libavcodec/agc_core.h
cp "$HERE/agc1enc.c"     libavcodec/agc1enc.c
cp "$HERE/agc1dec.c"     libavcodec/agc1dec.c

CONFIG_FLAGS="$(ffmpeg -version | sed -n 's/^configuration: //p')"
# Redirect the install away from /usr -- this build is a superset of the
# system one (same source, same flags, plus agc1), meant to sit alongside it
# and be picked up via LD_LIBRARY_PATH, not to replace it.
CONFIG_FLAGS="$(echo "$CONFIG_FLAGS" \
    | sed -E "s#--prefix=[^ ]+#--prefix=$INSTALL_PREFIX#" \
    | sed -E "s#--libdir=[^ ]+#--libdir=$INSTALL_PREFIX/lib#" \
    | sed -E "s#--incdir=[^ ]+#--incdir=$INSTALL_PREFIX/include#")"

echo "==> Configuring (reusing the system ffmpeg's own flags, so this build"
echo "    is a drop-in superset -- everything the system one has, plus agc1)"
# shellcheck disable=SC2086
./configure $CONFIG_FLAGS --enable-rpath \
    --enable-decoder=agc1 --enable-encoder=agc1 \
    --extra-libs="-lEGL -lGL -lpthread" \
    --extra-cflags="-I$HERE/.. -I/opt/mesa-adreno/include" \
    --extra-ldflags="-L/opt/mesa-adreno/lib"

echo "==> Building with -j$JOBS (raise AGC1_MAKE_JOBS if you have RAM to spare)"
make -j"$JOBS"
make install

echo
echo "==> Done. AGC-1-capable ffmpeg installed at: $INSTALL_PREFIX"
echo "    Verify with:"
echo "      LD_LIBRARY_PATH=$INSTALL_PREFIX/lib $INSTALL_PREFIX/bin/ffmpeg -encoders | grep agc1"
echo
echo "    To make Shotcut or Blender use it, put $INSTALL_PREFIX/lib first in"
echo "    LD_LIBRARY_PATH before launching them -- see README.md."
