#!/usr/bin/env python3
"""
Mock SELinux Hardware Bridge speaking protocol v3.

Lets bridge_client, the ffmpeg encoders and any other consumer be tested on a
machine with no phone attached, and lets the awkward parts of MediaCodec's
behaviour be reproduced deliberately instead of waited for.  It mimics the
three things a client has to cope with:

  * SPS/PPS (and VPS for HEVC) arrive as a standalone codec-config packet
    before any picture data, exactly like BUFFER_FLAG_CODEC_CONFIG;
  * one output packet per access unit, not per NAL unit;
  * several input frames are swallowed before the first output appears, which
    is what makes a naive write-then-read client deadlock.

Frames go through a real x264/x265 subprocess, so the output is a genuine
stream that corresponds to the input rather than a canned replay.

Usage:  mock-bridge.py [port]           (default 9401)
"""
import socket
import struct
import subprocess
import sys
import threading

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9401

# Wire codec id -> (ffmpeg encoder, component name reported to the client)
CODECS = {
    0: ("libx264", b"c2.mock.avc.encoder"),
    1: ("libx265", b"c2.mock.hevc.encoder"),
}


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


def handle(conn):
    hdr = conn.recv(24)
    if len(hdr) < 24:
        conn.close()
        return
    mode, w, h, fps, br, codec = struct.unpack(">6i", hdr)
    print(f"[mock] mode={mode} {w}x{h} fps={fps} br={br} codec={codec}",
          flush=True)

    if codec not in CODECS:
        msg = b"unsupported codec id"
        conn.sendall(struct.pack(">ii", 1, len(msg)) + msg)
        conn.close()
        return

    encoder, name = CODECS[codec]
    hevc = codec == 1
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

    f = conn.makefile("rb")
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
    conn.close()


def main():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", PORT))
    s.listen(8)
    print(f"[mock] v3 bridge listening on 127.0.0.1:{PORT}", flush=True)
    while True:
        c, _ = s.accept()
        threading.Thread(target=handle, args=(c,), daemon=True).start()


if __name__ == "__main__":
    main()
