#!/usr/bin/env python3
"""
Mock SELinux Hardware Bridge speaking protocol v5.

Lets bridge_client, the ffmpeg codecs and any other consumer be tested on a
machine with no phone attached, and lets the awkward parts of MediaCodec's
behaviour be reproduced deliberately instead of waited for.  It mimics the
things a client has to cope with:

  * SPS/PPS (and VPS for HEVC) arrive as a standalone codec-config packet
    before any picture data, exactly like BUFFER_FLAG_CODEC_CONFIG;
  * one output packet per access unit, not per NAL unit;
  * several input frames are swallowed before the first output appears, which
    is what makes a naive write-then-read client deadlock;
  * decode output is preceded by a v4 format record announcing the real
    picture size, and another one whenever that size changes -- which
    MOCK_RESIZE_AT forces on demand, because provoking a genuine mid-stream
    resolution change on a real device is impractical.

Frames go through a real x264/x265 subprocess, so the output is a genuine
stream that corresponds to the input rather than a canned replay.

Usage:  mock-bridge.py [port]           (default 9401)
Env:    MOCK_RESIZE_AT=N   halve the picture size from decoded frame N on
        MOCK_UNSUPPORTED_RC=cq[,vbr]  refuse those rate-control modes
        MOCK_DENY_CAMERA=1 / MOCK_DENY_MIC=1  refuse capture the way an
                            ungranted runtime permission does
        MOCK_CAMERAS=N     how many cameras to pretend exist (default 2)
        MOCK_CAMERA_EXACT=1  honour the requested size instead of rounding
"""
import os
import math
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9401
RESIZE_AT = int(os.environ.get("MOCK_RESIZE_AT", "0"))
# Rate-control modes the pretend component refuses, as "cq" or "vbr,cq".
# Real Qualcomm parts split these across components (CQ lives on a separate
# c2.qti.*.encoder.cq), so the refusal path needs covering without a device.
UNSUPPORTED_RC = {m for m in os.environ.get("MOCK_UNSUPPORTED_RC", "").split(",") if m}

# Capture knobs. The deny switches exist because a missing runtime grant is
# the single likeliest reason a capture session fails on a real phone, and
# the refusal has to be a readable error rather than an empty stream.
DENY_CAMERA = os.environ.get("MOCK_DENY_CAMERA", "") not in ("", "0")
DENY_MIC = os.environ.get("MOCK_DENY_MIC", "") not in ("", "0")
MOCK_CAMERAS = int(os.environ.get("MOCK_CAMERAS", "2"))
# Off by default so the mock rounds the requested size like a real HAL does.
CAMERA_EXACT = os.environ.get("MOCK_CAMERA_EXACT", "") not in ("", "0")

# Wire codec id -> (ffmpeg encoder, decoder demuxer, component name)
CODECS = {
    0: ("libx264", "h264", b"c2.mock.avc"),
    1: ("libx265", "hevc", b"c2.mock.hevc"),
}

# v5 rate-control modes, carried in the codec field's high byte.
RC_NAMES = {0: "cbr", 1: "vbr", 2: "cq"}


def split_nals(buf):
    starts, i, n = [], 0, len(buf)
    while i + 3 <= n:
        if buf[i] == 0 and buf[i + 1] == 0:
            if i + 4 <= n and buf[i + 2] == 0 and buf[i + 3] == 1:
                starts.append(i)
                i += 4
                continue
            if buf[i + 2] == 1:
                starts.append(i)
                i += 3
                continue
        i += 1
    return [buf[s:(starts[k + 1] if k + 1 < len(starts) else n)]
            for k, s in enumerate(starts)]


def classify(nal, hevc):
    """Return (is_parameter_set, is_vcl) for one Annex-B NAL unit."""
    off = 4 if nal[:4] == b"\x00\x00\x00\x01" else 3
    if hevc:
        t = (nal[off] >> 1) & 0x3F
        return t in (32, 33, 34), t <= 31
    t = nal[off] & 0x1F
    return t in (7, 8), t in (1, 5)


def group_access_units(raw, hevc):
    """Parameter sets form their own packet; each VCL NAL closes an AU."""
    packets, pending, au = [], [], []
    for nal in split_nals(raw):
        is_ps, is_vcl = classify(nal, hevc)
        if is_ps:
            pending.append(nal)
            continue
        if pending:
            packets.append(b"".join(pending))
            pending = []
        au.append(nal)
        if is_vcl:
            packets.append(b"".join(au))
            au = []
    if pending:
        packets.append(b"".join(pending))
    if au:
        packets.append(b"".join(au))
    return packets


def frame_size(w, h):
    cw, ch = (w + 1) // 2, (h + 1) // 2
    return w * h + 2 * cw * ch


def halve_i420(buf, w, h):
    """2:1 subsample a tightly packed I420 frame, for the resize test."""
    nw, nh = w // 2, h // 2
    cw, ch = (w + 1) // 2, (h + 1) // 2
    ncw, nch = (nw + 1) // 2, (nh + 1) // 2
    out = bytearray()
    for y in range(nh):
        row = buf[2 * y * w: 2 * y * w + w]
        out += bytes(row[2 * x] for x in range(nw))
    for base in (w * h, w * h + cw * ch):
        for y in range(nch):
            row = buf[base + 2 * y * cw: base + 2 * y * cw + cw]
            out += bytes(row[2 * x] for x in range(ncw))
    return bytes(out), nw, nh


def probe_dimensions(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height", "-of", "csv=p=0", path],
        capture_output=True, text=True).stdout.strip()
    w, h = out.split(",")[:2]
    return int(w), int(h)


INFO_REPORT = (
    "SELinux Hardware Bridge diagnostics (mock)\n"
    "device: mock (mock)\n"
    "protocol: v5\n"
    "concurrent codec slots: 3 (3 free)\n"
)


def handle_encode(conn, f, w, h, fps, codec):
    encoder, _, name = CODECS[codec]
    hevc = codec == 1
    name = name + b".encoder"
    conn.sendall(struct.pack(">ii", 0, len(name)) + name)

    args = ["ffmpeg", "-v", "error", "-f", "rawvideo", "-pix_fmt", "yuv420p",
            "-s", f"{w}x{h}", "-r", str(max(fps, 1)), "-i", "-",
            "-c:v", encoder, "-preset", "ultrafast", "-g", "1"]
    if hevc:
        args += ["-x265-params", "log-level=none", "-tag:v", "hvc1",
                 "-f", "hevc", "-"]
    else:
        args += ["-f", "h264", "-"]

    enc = subprocess.Popen(args, stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE)
    nframes = [0]

    def feeder():
        while True:
            b = f.read(4)
            if len(b) < 4:
                break
            (length,) = struct.unpack(">i", b)
            if length < 0:
                break
            try:
                enc.stdin.write(f.read(length))
            except BrokenPipeError:
                break
            nframes[0] += 1
        try:
            enc.stdin.close()
        except OSError:
            pass

    t = threading.Thread(target=feeder, daemon=True)
    t.start()

    raw = enc.stdout.read()      # lookahead: nothing goes out until input ends
    t.join()
    enc.wait()

    packets = group_access_units(raw, hevc)
    for p in packets:
        conn.sendall(struct.pack(">i", len(p)) + p)
    conn.sendall(struct.pack(">i", -1))
    print(f"[mock] in={nframes[0]} frames, out={len(packets)} packets "
          f"({len(raw)} bytes)", flush=True)


def handle_decode(conn, f, codec):
    """
    Decode side, protocol v4 format records.

    Real MediaCodec discovers the picture size from the stream's parameter
    sets, not from the client, so this deliberately ignores the handshake's
    width/height and announces what it finds. Input is buffered whole before
    anything is decoded, which also reproduces the lookahead a client must
    tolerate.
    """
    _, demuxer, name = CODECS[codec]
    name = name + b".decoder"
    conn.sendall(struct.pack(">ii", 0, len(name)) + name)

    units, nunits = bytearray(), 0
    while True:
        b = f.read(4)
        if len(b) < 4:
            break
        (length,) = struct.unpack(">i", b)
        if length < 0:
            break
        units += f.read(length)
        nunits += 1

    with tempfile.NamedTemporaryFile(suffix="." + demuxer) as tf:
        tf.write(units)
        tf.flush()
        try:
            w, h = probe_dimensions(tf.name)
        except Exception:
            conn.sendall(struct.pack(">i", -1))
            print("[mock] decode: unprobeable stream", flush=True)
            return
        raw = subprocess.run(
            ["ffmpeg", "-v", "error", "-f", demuxer, "-i", tf.name,
             "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"],
            capture_output=True).stdout

    fsz = frame_size(w, h)
    nframes = len(raw) // fsz
    announced = None
    for i in range(nframes):
        frame = raw[i * fsz:(i + 1) * fsz]
        fw, fh = w, h
        if RESIZE_AT and i >= RESIZE_AT:
            frame, fw, fh = halve_i420(frame, w, h)
        if (fw, fh) != announced:
            conn.sendall(struct.pack(">iii", -2, fw, fh))
            announced = (fw, fh)
        conn.sendall(struct.pack(">i", len(frame)) + frame)
    conn.sendall(struct.pack(">i", -1))
    print(f"[mock] decode: in={nunits} units, out={nframes} frames at {w}x{h}"
          + (f" (resized at {RESIZE_AT})" if RESIZE_AT else ""), flush=True)


def handle_camera(conn, w, h, fps, max_frames, cam_index):
    """
    Synthetic camera. Emits a format record then I420 frames.

    The size is deliberately *not* the one requested unless MOCK_CAMERA_EXACT
    is set: a real camera rounds the request to a size it actually offers,
    and a client that assumes it got what it asked for is broken in a way
    that only shows up on hardware. Making the mock round too means the
    selftest catches it here instead.
    """
    if DENY_CAMERA:
        msg = (b"the bridge app has not been granted CAMERA."
               b" Open SELinux Hardware Bridge on the phone and allow camera"
               b" access, then retry.")
        conn.sendall(struct.pack(">ii", 1, len(msg)) + msg)
        return
    if cam_index >= MOCK_CAMERAS:
        msg = ("camera index %d out of range (this device has %d, so 0..%d;"
               " 'bridge_client info' lists them)"
               % (cam_index, MOCK_CAMERAS, MOCK_CAMERAS - 1)).encode()
        conn.sendall(struct.pack(">ii", 1, len(msg)) + msg)
        return

    rw, rh = (w, h) if CAMERA_EXACT else round_to_camera_size(w, h)
    name = ("camera:%d" % cam_index).encode()
    conn.sendall(struct.pack(">ii", 0, len(name)) + name)
    conn.sendall(struct.pack(">iii", -2, rw, rh))

    n = max_frames if max_frames > 0 else 0
    delay = 1.0 / fps if fps > 0 else 0.0
    i = 0
    while n == 0 or i < n:
        # A moving luma ramp: a static frame would pass a test that only
        # checks sizes even if every frame were identical.
        y = bytes([(i * 7 + (p % 251)) & 0xff for p in range(rw)]) * rh
        uv = b"\x80" * (rw * rh // 4)
        frame = y + uv + uv
        conn.sendall(struct.pack(">i", len(frame)) + frame)
        i += 1
        if delay:
            time.sleep(delay)
    conn.sendall(struct.pack(">i", -1))


def round_to_camera_size(w, h):
    """Nearest of a plausible fixed size list, mimicking a real HAL."""
    sizes = [(176, 144), (320, 240), (640, 480), (1280, 720),
             (1920, 1080), (3840, 2160)]
    want = w * h
    return min(sizes, key=lambda s: abs(s[0] * s[1] - want))


def handle_mic(conn, rate, channels, max_seconds, source):
    """Synthetic microphone: a format record then S16LE chunks."""
    if DENY_MIC:
        msg = (b"the bridge app has not been granted RECORD_AUDIO."
               b" Open SELinux Hardware Bridge on the phone and allow microphone"
               b" access, then retry.")
        conn.sendall(struct.pack(">ii", 1, len(msg)) + msg)
        return
    if source > 3:
        msg = ("unknown audio source %d (expected 0=mic, 1=voice_communication,"
               " 2=camcorder, 3=unprocessed)" % source).encode()
        conn.sendall(struct.pack(">ii", 1, len(msg)) + msg)
        return

    rate = rate if rate > 0 else 48000
    channels = channels if channels > 0 else 1
    names = {0: "MIC", 1: "VOICE_COMMUNICATION", 2: "CAMCORDER", 3: "UNPROCESSED"}
    name = ("mic:%s" % names[source]).encode()
    conn.sendall(struct.pack(">ii", 0, len(name)) + name)
    conn.sendall(struct.pack(">iii", -2, rate, channels))

    seconds = max_seconds if max_seconds > 0 else 0
    chunk_samples = rate // 10
    i = 0
    while seconds == 0 or i < seconds * 10:
        # A 440 Hz sine, so a listener can tell real audio from zeros.
        buf = bytearray()
        for k in range(chunk_samples):
            t = (i * chunk_samples + k) / rate
            v = int(12000 * math.sin(2 * math.pi * 440 * t))
            buf += struct.pack("<h", v) * channels
        conn.sendall(struct.pack(">i", len(buf)) + bytes(buf))
        i += 1
        # Unlimited capture must run in real time, or it floods the pipe
        # far faster than a microphone ever would.
        if seconds == 0:
            time.sleep(0.1)
    conn.sendall(struct.pack(">i", -1))


def handle(conn):
    f = conn.makefile("rb")
    hdr = f.read(24)
    if not hdr or len(hdr) < 24:
        conn.close()
        return
    mode, w, h, fps, br, codec_field = struct.unpack(">6i", hdr)
    # v5 packs the rate-control mode into the high byte of the codec field.
    codec = codec_field & 0xff
    rc = (codec_field >> 8) & 0xff
    print(f"[mock] mode={mode} {w}x{h} fps={fps} br={br} codec={codec} "
          f"rc={RC_NAMES.get(rc, rc)}", flush=True)

    try:
        if mode in (2, 3):
            payload = (INFO_REPORT if mode == 2 else "(mock log)\n").encode()
            conn.sendall(struct.pack(">ii", 0, 3) + b"n/a")
            conn.sendall(struct.pack(">i", len(payload)) + payload)
            conn.sendall(struct.pack(">i", -1))
        elif mode == 4:
            handle_camera(conn, w, h, fps, br, codec)
        elif mode == 5:
            handle_mic(conn, w, h, br, codec)
        elif codec not in CODECS:
            msg = b"unsupported codec id"
            conn.sendall(struct.pack(">ii", 1, len(msg)) + msg)
        elif rc > 2:
            msg = b"unknown rate-control mode"
            conn.sendall(struct.pack(">ii", 1, len(msg)) + msg)
        elif mode == 0 and RC_NAMES.get(rc) in UNSUPPORTED_RC:
            msg = ("rate control '%s' is not supported by mock.encoder"
                   % RC_NAMES.get(rc)).encode()
            conn.sendall(struct.pack(">ii", 1, len(msg)) + msg)
        elif mode == 0:
            handle_encode(conn, f, w, h, fps, codec)
        elif mode == 1:
            handle_decode(conn, f, codec)
        else:
            msg = b"unknown mode"
            conn.sendall(struct.pack(">ii", 1, len(msg)) + msg)
    except (BrokenPipeError, ConnectionResetError, OSError):
        pass
    finally:
        conn.close()


def main():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", PORT))
    s.listen(8)
    print(f"[mock] v6 bridge listening on 127.0.0.1:{PORT}", flush=True)
    while True:
        c, _ = s.accept()
        threading.Thread(target=handle, args=(c,), daemon=True).start()


if __name__ == "__main__":
    main()
