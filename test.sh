#!/usr/bin/env bash
# AGC-1 test suite.
#
# Covers the round trip at a range of sizes and qualities, the padding paths
# for dimensions that are not a multiple of 8, colour, and -- importantly --
# that malformed input is rejected rather than crashing or hanging.
#
# Usage:  ./test.sh [path-to-agc]
set -u

AGC=${1:-./agc}
TMP=$(mktemp -d /tmp/agctest.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

if [ ! -x "$AGC" ]; then echo "no agc binary at $AGC" >&2; exit 1; fi
if ! command -v ffmpeg >/dev/null; then echo "ffmpeg is required" >&2; exit 1; fi

pass=0; fail=0
ok()   { pass=$((pass+1)); printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }

expected_bytes() {
    if [ "$3" = yuv420p ]; then echo $(( $1*$2 + 2 * ((($1+1)/2) * (($2+1)/2)) ))
    else echo $(( $1*$2 )); fi
}

# ffmpeg refuses some geometries (it will not emit a 1x1 testsrc, and forces
# even dimensions for yuv420p), so fall back to a synthetic plane. Otherwise a
# source-generation failure would masquerade as a codec failure.
gen_source() {
    local w=$1 h=$2 fmt=$3 out=$4 pix=gray want
    [ "$fmt" = yuv420p ] && pix=yuv420p
    want=$(expected_bytes "$w" "$h" "$fmt")
    if ffmpeg -v error -f lavfi -i "testsrc2=size=${w}x${h}" -frames:v 1 \
            -vf "scale=${w}:${h}" -pix_fmt "$pix" -f rawvideo -y "$out" 2>/dev/null \
       && [ "$(stat -c%s "$out")" = "$want" ]; then
        return 0
    fi
    python3 - "$out" "$w" "$h" "$fmt" <<'PY'
import sys
path, w, h, fmt = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
def plane(pw, ph, phase):
    b = bytearray()
    for y in range(ph):
        for x in range(pw):
            v = (x * 255 // max(pw, 1) + y * 255 // max(ph, 1)) // 2 + phase
            if ((x >> 3) + (y >> 3)) & 1:
                v = 255 - v
            b.append(max(0, min(255, v)))
    return bytes(b)
data = plane(w, h, 0)
if fmt == 'yuv420p':
    cw, ch = (w + 1) // 2, (h + 1) // 2
    data += plane(cw, ch, 40) + plane(cw, ch, 80)
open(path, 'wb').write(data)
PY
    [ "$(stat -c%s "$out")" = "$want" ]
}

# A round trip is only a pass if it survives the file, keeps the exact byte
# count, and lands above a PSNR floor -- a codec that writes a blank frame
# would otherwise pass silently.
roundtrip() {
    local w=$1 h=$2 fmt=$3 q=$4 floor=$5
    local name="${w}x${h} ${fmt} q${q}"
    local src="$TMP/s.raw" enc="$TMP/s.agc" dec="$TMP/d.raw"

    if ! gen_source "$w" "$h" "$fmt" "$src"; then
        bad "$name (could not build source)"; return
    fi
    if ! "$AGC" encode "$src" "$enc" -s "${w}x${h}" -f "$fmt" -q "$q" >/dev/null 2>"$TMP/e.log"; then
        bad "$name encode: $(head -1 "$TMP/e.log")"; return
    fi
    if ! "$AGC" decode "$enc" "$dec" >/dev/null 2>"$TMP/d.log"; then
        bad "$name decode: $(head -1 "$TMP/d.log")"; return
    fi
    local a b
    a=$(stat -c%s "$src"); b=$(stat -c%s "$dec")
    if [ "$a" != "$b" ]; then bad "$name size $b != $a"; return; fi
    local out psnr
    out=$("$AGC" compare "$src" "$dec" "${w}x${h}" "$fmt")
    if [ "$out" = "identical (PSNR infinite)" ]; then ok "$name lossless"; return; fi
    psnr=$(echo "$out" | awk '{print $2}')
    if awk -v p="$psnr" -v f="$floor" 'BEGIN{exit !(p>=f)}'; then
        ok "$name PSNR ${psnr} dB"
    else
        bad "$name PSNR ${psnr} dB below floor ${floor}"
    fi
}

# Malformed input must produce a non-zero exit and a message, never a crash
# (>=128 means a signal) and never a hang.
reject() {
    local name=$1 file=$2
    timeout 60 "$AGC" decode "$file" "$TMP/x.raw" >/dev/null 2>"$TMP/r.log"
    local rc=$?
    if [ $rc -eq 0 ];        then bad "$name was accepted"
    elif [ $rc -ge 128 ];    then bad "$name crashed or hung (rc=$rc)"
    elif [ ! -s "$TMP/r.log" ]; then bad "$name rejected with no message"
    else ok "$name rejected: $(head -c 70 "$TMP/r.log" | tr -d '\n')"
    fi
}

echo
echo "AGC-1 test suite  ($AGC)"
echo
echo "round trip, sizes and qualities"
roundtrip 3840 2160 gray8   75 40
roundtrip 1920 1080 yuv420p 85 38
roundtrip 1280  720 yuv420p 50 30
roundtrip  640  480 gray8   95 40
roundtrip  320  240 yuv420p 75 32

echo
echo "dimensions that are not a multiple of 8"
roundtrip 1237  699 gray8   80 35
roundtrip 1236  698 yuv420p 80 35
roundtrip   17   11 gray8   80 25
roundtrip    8    8 gray8   80 25
roundtrip    1    1 gray8   80 25

echo
echo "streaming, pipes and sequences"

# A sequence is just AGC frames back to back, so the source is n frames of raw
# video concatenated. Byte counts are asserted for the whole stream, which is
# what catches a decoder that silently stops after the first frame.
gen_seq() {
    local w=$1 h=$2 fmt=$3 n=$4 out=$5 pix=gray want
    [ "$fmt" = yuv420p ] && pix=yuv420p
    want=$(( $(expected_bytes "$w" "$h" "$fmt") * n ))
    ffmpeg -v error -f lavfi -i "testsrc2=size=${w}x${h}:rate=25" -frames:v "$n" \
        -vf "scale=${w}:${h}" -pix_fmt "$pix" -f rawvideo -y "$out" 2>/dev/null
    [ "$(stat -c%s "$out")" = "$want" ]
}

seq_roundtrip() {
    local w=$1 h=$2 fmt=$3 q=$4 n=$5 floor=$6
    local name="sequence ${n} frames ${w}x${h} ${fmt}"
    local src="$TMP/q.raw" enc="$TMP/q.agc" dec="$TMP/qd.raw"
    if ! gen_seq "$w" "$h" "$fmt" "$n" "$src"; then bad "$name (could not build source)"; return; fi
    if ! "$AGC" encode "$src" "$enc" -s "${w}x${h}" -f "$fmt" -q "$q" -r 25 \
            >/dev/null 2>"$TMP/e.log"; then
        bad "$name encode: $(head -1 "$TMP/e.log")"; return
    fi
    if ! "$AGC" decode "$enc" "$dec" >/dev/null 2>"$TMP/d.log"; then
        bad "$name decode: $(head -1 "$TMP/d.log")"; return
    fi
    local a b
    a=$(stat -c%s "$src"); b=$(stat -c%s "$dec")
    if [ "$a" != "$b" ]; then bad "$name size $b != $a"; return; fi
    local got
    got=$("$AGC" info "$enc" 2>/dev/null | awk '/^  frames/{print $2}')
    if [ "$got" != "$n" ]; then bad "$name info reported '${got}' frames, not $n"; return; fi
    local psnr
    psnr=$("$AGC" compare "$src" "$dec" "${w}x${h}" "$fmt" | awk '{print $2}')
    if awk -v p="$psnr" -v f="$floor" 'BEGIN{exit !(p>=f)}'; then
        ok "$name, first frame ${psnr} dB"
    else
        bad "$name first frame ${psnr} dB below floor ${floor}"
    fi
}

seq_roundtrip 320 240 yuv420p 80 12 30
seq_roundtrip 128  96 gray8   80  5 28

# Through pipes end to end, which also proves the status lines go to stderr:
# any of them landing on stdout would corrupt the bitstream and the byte count
# or the checksum would fail.
pipe_name="pipe round trip 320x240 yuv420p"
if gen_seq 320 240 yuv420p 6 "$TMP/p.raw"; then
    cat "$TMP/p.raw" \
      | "$AGC" encode - - -s 320x240 -f yuv420p -q 80 -r 25 2>/dev/null \
      | "$AGC" decode - - > "$TMP/pd.raw" 2>/dev/null
    pa=$(stat -c%s "$TMP/p.raw"); pb=$(stat -c%s "$TMP/pd.raw")
    if [ "$pa" = "$pb" ]; then ok "$pipe_name ($pb bytes)"
    else bad "$pipe_name size $pb != $pa"; fi
else
    bad "$pipe_name (could not build source)"
fi

# Writing to stdout must produce exactly the same bytes as writing to a file.
if "$AGC" encode "$TMP/p.raw" "$TMP/tofile.agc" -s 320x240 -f yuv420p -q 80 -r 25 \
        >/dev/null 2>&1 \
   && "$AGC" encode "$TMP/p.raw" - -s 320x240 -f yuv420p -q 80 -r 25 \
        > "$TMP/topipe.agc" 2>/dev/null; then
    if cmp -s "$TMP/tofile.agc" "$TMP/topipe.agc"; then
        ok "stdout stream identical to file stream"
    else
        bad "stdout stream differs from file stream"
    fi
else
    bad "stdout stream could not be produced"
fi

# The frame rate is carried in the stream, so probe must read back what
# encode was told.
eval "$("$AGC" probe "$TMP/tofile.agc" 2>/dev/null)" 2>/dev/null || true
if [ "${AGC_WIDTH:-}" = 320 ] && [ "${AGC_HEIGHT:-}" = 240 ] \
   && [ "${AGC_FORMAT:-}" = yuv420p ] && [ "${AGC_FPS:-}" = 25.000 ]; then
    ok "probe reports 320x240 yuv420p @ 25.000 fps"
else
    bad "probe reported ${AGC_WIDTH:-?}x${AGC_HEIGHT:-?} ${AGC_FORMAT:-?} @ ${AGC_FPS:-?}"
fi

echo
echo "quality extremes"
roundtrip  640  480 gray8    1 15
roundtrip  640  480 gray8  100 45

echo
echo "malformed input"
: > "$TMP/empty.agc";                                   reject "empty file"        "$TMP/empty.agc"
printf 'xy' > "$TMP/tiny.agc";                          reject "2-byte file"       "$TMP/tiny.agc"
head -c 40000 /dev/urandom > "$TMP/rand.agc";           reject "random bytes"      "$TMP/rand.agc"

gen_source 640 480 gray8 "$TMP/g.raw"
"$AGC" encode "$TMP/g.raw" "$TMP/good.agc" -s 640x480 -q 80 >/dev/null 2>&1

head -c 100 "$TMP/good.agc" > "$TMP/trunc.agc";         reject "truncated file"    "$TMP/trunc.agc"
cp "$TMP/good.agc" "$TMP/magic.agc"
printf 'ZZZZ' | dd of="$TMP/magic.agc" bs=1 seek=0 conv=notrunc 2>/dev/null
reject "bad magic" "$TMP/magic.agc"
cp "$TMP/good.agc" "$TMP/ver.agc"
printf '\x63' | dd of="$TMP/ver.agc" bs=1 seek=4 conv=notrunc 2>/dev/null
reject "future version" "$TMP/ver.agc"
cp "$TMP/good.agc" "$TMP/bit.agc"
python3 - "$TMP/bit.agc" <<'PY'
import sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
d[len(d)//2] ^= 1
open(p, 'wb').write(d)
PY
reject "single flipped bit" "$TMP/bit.agc"

# A sequence cut partway through a later frame must be reported, not silently
# treated as a shorter clip.
if gen_seq 128 96 gray8 3 "$TMP/s3.raw" \
   && "$AGC" encode "$TMP/s3.raw" "$TMP/s3.agc" -s 128x96 -q 80 -r 25 >/dev/null 2>&1; then
    full=$(stat -c%s "$TMP/s3.agc")
    head -c $(( full - 200 )) "$TMP/s3.agc" > "$TMP/scut.agc"
    reject "sequence truncated mid-frame" "$TMP/scut.agc"
else
    bad "sequence truncated mid-frame (could not build source)"
fi

# Raw input that is not a whole number of frames is a mistake worth reporting,
# because it usually means the geometry is wrong.
if gen_seq 64 64 gray8 3 "$TMP/pf.raw"; then
    head -c $(( 64*64*2 + 1000 )) "$TMP/pf.raw" > "$TMP/pf25.raw"
    if "$AGC" encode "$TMP/pf25.raw" "$TMP/pf.agc" -s 64x64 -q 80 >/dev/null 2>&1; then
        bad "partial trailing frame was accepted"
    else
        ok "partial trailing frame rejected"
    fi
else
    bad "partial trailing frame (could not build source)"
fi

# Ctrl-C is the documented way to stop agc-rec, so an interrupted encode has
# to leave a stream that still reads back cleanly. The frame in flight is
# finished and flushed rather than torn in half, which would otherwise cost
# the reader the entire capture, not just the last frame.
if gen_seq 96 64 gray8 1 "$TMP/one.raw"; then
    # timeout signals the whole process group, so the producer and the encoder
    # both see SIGINT -- exactly what happens on a terminal.
    timeout -s INT 3 sh -c "
        i=0
        while [ \$i -lt 500 ]; do cat '$TMP/one.raw'; sleep 0.02; i=\$((i+1)); done \
          | '$AGC' encode - '$TMP/int.agc' -s 96x64 -f gray8 -q 80 -r 25
    " >/dev/null 2>&1
    if [ ! -s "$TMP/int.agc" ]; then
        bad "interrupted encode wrote nothing"
    elif ! "$AGC" info "$TMP/int.agc" >/dev/null 2>&1; then
        bad "interrupted encode left a torn frame"
    elif ! "$AGC" decode "$TMP/int.agc" /dev/null >/dev/null 2>&1; then
        bad "interrupted encode does not decode"
    else
        ok "interrupted encode stays readable"
    fi
else
    bad "interrupted encode (could not build source)"
fi

echo
echo "argument handling"
argfail() {
    local name=$1; shift
    if "$AGC" "$@" >/dev/null 2>&1; then bad "$name was accepted"; else ok "$name rejected"; fi
}
argfail "encode without -s"      encode "$TMP/g.raw" "$TMP/o.agc"
argfail "encode with bad -s"     encode "$TMP/g.raw" "$TMP/o.agc" -s 640
argfail "encode with bad format" encode "$TMP/g.raw" "$TMP/o.agc" -s 640x480 -f rgb
argfail "encode with q=0"        encode "$TMP/g.raw" "$TMP/o.agc" -s 640x480 -q 0
argfail "encode with q=101"      encode "$TMP/g.raw" "$TMP/o.agc" -s 640x480 -q 101
argfail "wrong input size"       encode "$TMP/g.raw" "$TMP/o.agc" -s 1920x1080
argfail "missing input file"     encode "$TMP/nope.raw" "$TMP/o.agc" -s 640x480
argfail "unknown subcommand"     frobnicate
argfail "unknown option"         encode "$TMP/g.raw" "$TMP/o.agc" -s 640x480 --wat

echo
echo "  $pass passed, $fail failed"
echo
[ "$fail" -eq 0 ]
