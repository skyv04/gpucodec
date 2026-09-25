#!/usr/bin/env bash
# build-selinuxbridge.sh — rebuilds ffmpeg with the SELinux Hardware Bridge
# encoders (h264_selinuxbridge, hevc_selinuxbridge) wired into libavcodec.
#
# Why: inside the Termux/PRoot container ffmpeg has no hardware encoder at
# all, because /dev/dma_heap/* is SELinux-denied and Codec2 cannot allocate
# buffers. The companion APK (../selinux-bridge/) runs in a normal Android
# app domain where Codec2 *is* permitted and exposes it over loopback TCP;
# this makes that reachable as an ordinary ffmpeg encoder, so anything
# driving libavcodec gets hardware offload with no pipeline surgery.
#
# This deliberately reuses the same source tree and install prefix as
# build.sh (the AGC-1 build), so you end up with ONE ffmpeg that has both
# agc1 and the bridge encoders. Run build.sh first if the tree doesn't
# exist yet; this script will bail out rather than guess.
#
# Unlike the agc1 patch, this needs no new AV_CODEC_ID / codec descriptor /
# container tag: the bridge emits standard H.264 and HEVC, so the encoders
# simply attach to the existing codec ids.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_ROOT="${AGC1_BUILD_ROOT:-$HOME/build}"
SRC="${AGC1_FFMPEG_SRC:-$BUILD_ROOT/ffmpeg-5.1.9}"
INSTALL_PREFIX="${AGC1_INSTALL_PREFIX:-$BUILD_ROOT/ffmpeg-agc1-install}"
JOBS="${AGC1_MAKE_JOBS:-4}"

if [ ! -d "$SRC" ]; then
    cat >&2 <<EOF
!! No ffmpeg source tree at $SRC

   Run ./build.sh first -- it fetches the ffmpeg source matching your
   installed binary and does the initial configure. This script only adds
   the bridge encoders to that existing tree.
EOF
    exit 1
fi

cd "$SRC"

echo "==> Installing the bridge codec into libavcodec"
cp "$HERE/selinuxbridge.c" libavcodec/selinuxbridge.c

echo "==> Applying the registration patch (allcodecs.c + Makefile)"
if grep -q selinuxbridge libavcodec/allcodecs.c; then
    echo "    already registered, skipping"
else
    patch -p1 -N --forward < "$HERE/selinuxbridge-ffmpeg.patch"
fi

# ffmpeg's configure derives its encoder list by scanning allcodecs.c for
# `extern const FFCodec ff_*_encoder;`, so the patch above is what makes
# these two names valid --enable-encoder targets. Reuse the tree's own
# recorded flags so this stays a strict superset of the existing build.
if [ ! -f ffbuild/config.mak ]; then
    echo "!! $SRC has never been configured -- run ./build.sh first" >&2
    exit 1
fi
OLD_FLAGS="$(sed -n 's/^FFMPEG_CONFIGURATION=//p' ffbuild/config.mak)"
NEW_FLAGS="$OLD_FLAGS"
for enc in h264_selinuxbridge hevc_selinuxbridge; do
    case "$NEW_FLAGS" in
        *"--enable-encoder=$enc"*) ;;
        *) NEW_FLAGS="$NEW_FLAGS --enable-encoder=$enc" ;;
    esac
done

if [ "$NEW_FLAGS" != "$OLD_FLAGS" ]; then
    echo "==> Reconfiguring to enable the bridge encoders"
    eval ./configure $NEW_FLAGS
else
    echo "==> Already configured with the bridge encoders"
fi

echo "==> Building with -j$JOBS (raise AGC1_MAKE_JOBS if you have RAM to spare)"
make -j"$JOBS"
make install

cat <<EOF

==> Done. Verify with:
      LD_LIBRARY_PATH=$INSTALL_PREFIX/lib $INSTALL_PREFIX/bin/ffmpeg -encoders | grep selinuxbridge

    Then, with the SELinux Hardware Bridge app open on-screen:
      LD_LIBRARY_PATH=$INSTALL_PREFIX/lib $INSTALL_PREFIX/bin/ffmpeg \\
          -i input.mp4 -c:v h264_selinuxbridge -b:v 4M output.mp4
EOF
