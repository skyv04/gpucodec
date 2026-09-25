# AGC-1

A video codec written from scratch to run entirely in GPU compute shaders,
built for an Adreno 840 driven by Mesa/Turnip inside a Termux PRoot container.

Every stage — forward DCT, quantisation, entropy coding, bitstream packing,
parsing, and reconstruction — runs on the GPU. The CPU reads a file, hands over
pixels, and takes back a compressed stream.

## Why this exists

On this device the hardware video block is permanently unreachable. The
container is denied `/dev/dma_heap/*`, so Codec2 cannot allocate buffers, and
the SELinux label on `app_process64` blocks the usual workarounds. Hardware
encode and decode are simply not available, no matter how the software is
configured.

The GPU, by contrast, *is* reachable: `/dev/kgsl-3d0` is world-readable and
Turnip drives it properly. So the question became whether a codec could be
built that treats a GPU as its primary target rather than as an accelerator
bolted onto a CPU design.

## Why not just port H.264 or HEVC to the GPU

Because their two central mechanisms are inherently serial:

- **CABAC** is adaptive arithmetic coding. Each symbol updates the probability
  model used by the next one, so symbol *n+1* cannot begin until *n* finishes.
  This is why hardware decoders contain a *dedicated fixed-function CABAC
  engine* — even silicon cannot parallelise it.
- **Intra prediction** chains each block onto its already-reconstructed
  neighbours, serialising decode across the whole frame.

AGC-1 drops both. There is no arithmetic coder and no cross-block prediction:
every 8×8 block is independent, so a 4K frame is 129,600 genuinely parallel
units of work. That costs compression efficiency, and the numbers below say
exactly how much.

## The interesting problem: parallel decode of a bit-packed stream

Blocks are packed at exact bit offsets with no padding, which is what makes the
stream compact — an earlier version padded every block to a 32-bit boundary and
wasted 25.9% of the file on padding alone.

But bit-exact packing means a decoder cannot find block *N* without first
measuring every block before it. The obvious fixes are both bad:

- ship a 32-bit offset per block → 518 KB of index for a 562 KB payload;
- parse serially → throws away the parallelism the codec exists for.

Instead the stream carries **one 16-bit length per tile of 32 blocks**. The
decoder prefix-sums those on the GPU to recover tile offsets, then parses all
tiles in parallel, walking the 32 blocks inside a tile in sequence — each
block's parse ends exactly where the next one begins.

That costs **1.4%** of the file and keeps decode parallel 4,050 ways at 4K. The
worst possible tile is 32 × 1,927 = 61,664 bits, so a 16-bit length can never
overflow.

## Measured results

4K (3840×2160) luma, quality 75, Adreno 840, all eight cores available.
Figures are the **range over four consecutive `make bench` runs**, not a
cherry-picked best; GPU clock scaling on a phone moves these by ~10%.

| | |
|---|---|
| encode | 19.4–21.5 ms — **~47–52 fps** |
| decode | 19.0–21.0 ms — **~48–53 fps** |
| stream | 561.9 KB payload + 7.9 KB tile index → **14.22:1** |
| quality | **48.20 dB** PSNR |
| CPU consumed | **5.1–7.2 ms/frame — 13–18% of one core** |

Both figures are the **full pipeline**: host buffer in, host buffer out,
including upload and readback. Timings that exclude those are not a codec
anyone can use, and an earlier draft of this file quoted such numbers
(57 fps decode) before the I/O was added to the measurement.

The CPU number is the point of the whole exercise. CPU MJPEG at comparable
throughput consumes **74.3 ms of CPU per frame**; AGC-1 consumes about **6 ms**,
roughly **12× less**, leaving the cores free.

Per-stage, both directions are bandwidth-bound rather than compute-bound:

```
encode 19.37 ms = upload 1.02 + dct 12.97 + scan 2.20 + pack 2.11 + read 1.06
decode 19.02 ms = upload 1.36 + clear 5.18 + scan 1.35 + parse 2.88 + idct 6.34 + read 1.91
```

`clear` and `idct` are both pure memory traffic over the int32 coefficient
buffer. Packing coefficients as int16 (two per uint, since GLSL 430 has no
int16) would roughly halve both — the largest remaining win, not yet done.

## Honest limits

This is a real codec, not a replacement for the ones you already use.

- **JPEG still compresses better.** At matched quality on the same 4K frame,
  JPEG is 409 KB at 48.32 dB versus AGC-1's 570 KB at 48.20 dB — JPEG is
  **1.39× smaller**. Dropping CABAC and intra prediction is what costs this.
- **Intra-only.** Every frame is coded independently. There is no motion
  compensation, so it is nowhere near H.264/H.265 for video sequences. Motion
  search was also the weakest GPU result measured here (1.9× over eight CPU
  cores, bandwidth-bound), so this is not obviously worth adding.
- **It cannot decode H.264, HEVC, VP9 or AV1**, so it cannot play your existing
  files. For those, hardware/CPU decode with GPU-accelerated output is the right
  path.
- **No audio and no muxing.** A stream is frames and nothing else; the frame
  rate is the only timing information it carries. Anything needing sound or
  seeking has to go through `agc-to` into a real container.
- **Not registered with ffmpeg, GStreamer or VLC.** It is a standalone tool
  that composes through pipes.

Where it does make sense: bulk frame compression on a machine whose CPU you
want to keep free, or as a worked example of designing for a GPU from scratch.

## Build

Requires the side-by-side Mesa/Turnip stack at `/opt/mesa-adreno` and GL headers
at `~/.local/gldev`.

```sh
make                 # build ./agc
make test            # 35-case suite: round trips, padding, streaming, malformed input
make bench           # 4K benchmark with per-stage timings
make install         # installs ~/.local/bin/agc, the launcher, and the tools
```

`make install` writes a small launcher that points the process at Turnip/Zink
and a headless EGL device, so `agc` works from any directory without sourcing
anything first. The `tools/` wrappers are installed next to it and simply call
`agc` by name, so they inherit the same launcher.

## Usage

Input and output are planar raw frames: `gray8` is W×H bytes, `yuv420p` adds two
(W+1)/2 × (H+1)/2 chroma planes. Dimensions that are not a multiple of 8 are
coded padded by edge replication and cropped on decode.

```sh
ffmpeg -i clip.mp4 -frames:v 1 -pix_fmt yuv420p -f rawvideo frame.yuv
agc encode frame.yuv frame.agc -s 1920x1080 -f yuv420p -q 85
agc info   frame.agc
agc decode frame.agc out.yuv
ffmpeg -f rawvideo -pix_fmt yuv420p -s 1920x1080 -i out.yuv out.png
```

| command | |
|---|---|
| `encode <in> <out> -s WxH [-f gray8\|yuv420p] [-q 1..100] [-r fps] [-v]` | compress |
| `decode <in> <out> [-v]` | decompress |
| `info <in>` | header, tile counts, index overhead, frame count |
| `probe <in>` | geometry as shell variables, for scripts |
| `compare <a> <b> <WxH> <format>` | PSNR between two raw frames |
| `bench [frame.gray] [W] [H] [q]` | timed round trip with stage breakdown |

`-` means stdin or stdout. Any input holding more than one frame is encoded as
a sequence and decoded back as one; the GPU context and its buffers are built
once and reused for every frame.

Chroma is quantised at twice the luma scale; both sides derive this from the
stored quality, so it is not transmitted.

## Integrating it

AGC cannot decode H.264, HEVC, VP9 or AV1, and nothing else on the system can
open a `.agc` file. So it does not slot in as "the codec" anywhere. It earns
its place in exactly one situation: **you control both ends, and CPU is the
resource you are short of.**

`ffmpeg` is the adapter. Frames move through a pipe, so the raw video — which
is about 3 MB per 1080p frame — never touches the disk:

```sh
# anything ffmpeg can read -> agc
ffmpeg -v error -i clip.mp4 -pix_fmt yuv420p -f rawvideo - \
  | agc encode - clip.agc -s 1920x1080 -f yuv420p -q 80 -r 30

# agc -> anything
agc decode clip.agc - \
  | ffmpeg -v error -f rawvideo -pix_fmt yuv420p -s 1920x1080 -r 30 -i - out.mp4
```

Four wrappers under `tools/` do that for you, and are installed alongside `agc`:

| tool | |
|---|---|
| `agc-from <video> <out.agc> [q]` | anything ffmpeg reads → AGC |
| `agc-to <in.agc> <out.mp4> [ffmpeg args]` | AGC → an ordinary video file |
| `agc-play <file.agc>` | decode on the GPU and play it, nothing hits the disk |
| `agc-rec <out.agc> [-s WxH] [-r fps] [-q n]` | record the X11 screen straight to AGC |

They read geometry with `agc probe`, which prints shell variables rather than
prose, so nothing has to scrape human-readable output.

### Playing a stream

`agc-play` decodes on the GPU and feeds a player through a pipe. Two details
of this machine shaped how it does that, and both were measured rather than
guessed:

* **It uses VLC, not ffplay.** `ffplay` asks X for a GLX context and
  termux-x11 refuses it (`X_GLXCreateContext`, `BadValue`), so it displays
  nothing at all. VLC is tried first, through `start-vlc-gpu` when that is
  present, which also puts VLC's scaling and colour conversion on the Adreno.
* **The frames are wrapped in Matroska on the way.** Fed raw, VLC has no
  timebase, starves its own picture pool and drops frames, logging `buffer
  deadlock prevented`. Over an identical 14 s playback: raw = 12 such errors,
  Matroska = 0. This is not a re-encode — `-c:v rawvideo` copies the pixels
  and only adds framing.

VLC keeps its window open when the clip ends; close it to return to the shell.

### Stopping a capture

`agc-rec` is meant to be stopped with Ctrl-C, so that has to leave a usable
file. `agc encode` and `agc decode` catch `SIGINT` and `SIGTERM`, finish the
frame already in flight, flush it and stop between frames:

```
agc: interrupted -- 47 complete frames written
```

This matters more than it sounds. The container is deliberately strict, so a
frame torn in half would cost the reader the whole tail of the capture, not
just the last frame. Interrupting the encoder is a supported way to stop it;
truncating a file afterwards is still reported as damage.

### Does it actually help? Measured, not assumed

The claim is CPU offload, so the honest test is what happens to **other work**
while encoding. A fixed CPU workload saturating every available core, timed
alone and then again with an encoder running *continuously for the whole
workload* — so this is a steady-state rate, not a one-shot overlap.

On the default 4 throttled cores (`/background`):

| | workload wall time | slowdown |
|---|---|---|
| nothing else running | 9.69 s | — |
| while AGC-1 encodes (GPU) | 9.95 s | **+2.7%** |
| while x264 `veryfast` encodes (CPU) | 13.81 s | **+42%** |

Head to head on the same 100 frames of 1080p, both encoders reading the
identical raw file so no decoder cost is smuggled into either side:

| | AGC-1 | x264 veryfast |
|---|---|---|
| wall | **1.49 s** | 1.90 s |
| CPU consumed | **0.60 s** | 4.50 s |
| output size | 19.6 MB | **0.7 MB** |

**AGC costs 7.5× less CPU.** That is the whole argument, and everything else
is a cost: the file is ~28× larger and nothing else can read it.

Note the wall-time result **depends on the cpuset**, so state which you mean.
Measured earlier on `/top-app` with all 8 cores, x264 was the faster of the
two (0.59 s vs 0.88 s over 60 frames) because it had twice the parallelism to
exploit; on the 4 throttled cores this container gets by default, that
advantage disappears and AGC is faster as well as cheaper. Assert
`/proc/self/cpuset` in any benchmark here.

Expressed the way you would actually use it — capturing 1080p30, i.e. 100
frames every 3.33 s — AGC costs **0.18 of one core** and x264 `veryfast`
costs **1.35 cores**. On a 4-core container that is 4.5% of the machine
versus 34%.

Use AGC when the CPU is the scarce resource and the file is transient — screen
capture while the machine is busy, editing proxies, frame caches, scratch
intermediates. It matters more here than it would on a desktop because this
container is capped at 4 of 8 cores unless Termux holds focus.

**Do not use it** for anything you want to keep, send elsewhere, or play in a
normal player: it is several times larger than H.264 and no other software can
read it. Run `agc-to` first.

**And do not reach for it to make playback cheaper — it does not.** Twenty
seconds of 4K footage, measured end to end, each run screenshotted to prove a
picture was actually on the screen while the CPU was being sampled:

| | H.264 in VLC | AGC-1 via `agc-play` |
|---|---|---|
| size | **18.1 MB** | 438 MB (24x) |
| CPU while playing | **107–111%** of a core | 117–118% of a core |
| dropped frames | 0 | 0 |

Decoding 4K AGC costs 28.9 ms/frame, comfortably inside a 30 fps budget, and
that part really is on the GPU. It buys nothing, because playback is not
limited by decoding. The control says so: the same clip with `--vout=none`
decodes for 51% and never draws, so over half of VLC's 107% is simply getting
pixels onto the display — a cost both codecs pay in full. AGC then adds to it,
because every frame leaves the GPU as 12.4 MB of raw pixels and is copied
across two pipe boundaries, roughly 370 MB/s of pure memcpy, before VLC even
looks at it. That is the ~10 points it ends up behind by.

This is the same bus limit described under *Streaming throughput*, and it is
why the offload argument is about **encoding**, where the GPU replaces work the
CPU would otherwise do and only the compressed stream travels back. A playback
path that kept frames GPU-resident to the display would be a different design,
not a wrapper.

### Streaming throughput

Real sustained rates, with no instrumentation in the way:

| | encode | decode |
|---|---|---|
| 640×480 yuv420p | 4.1 ms (245 fps) | 9.0 ms (112 fps) |
| 1920×1080 yuv420p | 13.2 ms (76 fps) | — |
| 3840×2160 gray8 | 29.9 ms (34 fps) | 21.3 ms (47 fps) |

At 4K the limit is no longer the codec but the pipe: 8 MB per gray frame, 12.4
MB per colour frame, all of it crossing the process boundary as raw bytes. A
deeper integration would keep frames on the GPU instead of round-tripping them
through host memory.

> `make bench` reports noticeably slower figures than these. That is expected:
> bench drains the GPU pipeline between every stage so it can attribute time to
> each one, and each drain is a full stall. The per-stage numbers are useful for
> finding bottlenecks; the totals are not the throughput. Only `bench` pays this
> cost — it was originally paid on every frame, which is why a 640×480 frame
> once cost as much as a 4K one.

## Embedding AGC-1: the ffmpeg codec (Shotcut, Blender, and anything else)

Everything above talks to AGC through the CLI: raw frames over a pipe, in and
out of `agc`. That is enough for a wrapper script, but it is invisible to any
application that talks to video through `libavcodec` directly — which is
almost everything, including both video editors on this system. Shotcut and
Blender don't know what `agc` is and never will, no matter how good the
wrapper script is.

So AGC-1 is also a real, registered `libavcodec` codec: `agc1`, with its own
`AVCodecID`, its own encoder and decoder, and a real FourCC (`AGC1`) so AVI
containers can carry it. `ffmpeg -c:v agc1 out.avi` works exactly like `-c:v
libx264` does. This is not a wrapper around the CLI tool — it's the same GPU
pipeline (`agc_core.h`, extracted from `agc.c`) called directly from inside
`libavcodec`.

**Why this benefits Shotcut and Blender specifically, and why for free:**
both dynamically link the *same* system `libavcodec.so.59` (verified with
`ldd`; for Shotcut it's one level down, through MLT's `libmltavformat.so`
module). `libavcodec` has no plugin ABI — a new codec has to be compiled in —
but that also means **one rebuilt shared library is enough for both apps at
once**, decode side, with no changes to either application.

### What's in `ffmpeg/`

| file | |
|---|---|
| `ffmpeg/agc1-ffmpeg.patch` | registers `AV_CODEC_ID_AGC1` in `codec_id.h`, `codec_desc.c`, `allcodecs.c`, the `libavcodec/Makefile`, and the `AGC1` FourCC in `libavformat/riff.c` |
| `ffmpeg/agc1enc.c` | the `libavcodec` encoder: one `AVFrame` → one `AVPacket`, per-plane bitstreams concatenated with a tiny length header (no need for `agc.c`'s own file container — `AVCodecContext` already carries width/height/format) |
| `ffmpeg/agc1dec.c` | the matching decoder |
| `ffmpeg/build.sh` | fetches the matching Debian ffmpeg source, applies the patch, builds, installs to its own prefix |
| `agc_core.h` | the reusable GPU pipeline itself — see below |

### `agc_core.h`: why a second copy of the algorithm exists

`agc_core.h` has the same tables, shaders, and `encode_plane`/`decode_plane`
pipeline as `agc.c`. It is a deliberate, header-only duplication, not a typo:

* **`agc.c` is a short-lived CLI process; a codec is a guest in someone
  else's.** `agc.c`'s GL bootstrap (`glboot.h`) calls `exit(1)` on a shader
  compile failure or missing EGL display, because if that happens the whole
  program was going to fail anyway. That is fatal if it happens inside
  Shotcut or Blender instead — a GPU hiccup would take the whole editor down.
  Every failure path in `agc_core.h` returns an error code instead.
* **No signal handlers.** `agc.c` installs `SIGINT`/`SIGTERM` handlers so
  Ctrl-C during a capture still leaves a valid file (see *Stopping a
  capture*, above). A shared library must never do this — it would steal
  signal delivery from the host application.
* **The EGL context is acquired and released around each call.** `agc_gl_lock()`
  boots a process-wide headless context once (guarded by a mutex — safe if
  Shotcut or Blender call the codec from multiple threads) and makes it
  current on whichever thread calls in; `agc_gl_unlock()` releases it
  immediately after. Shotcut and Blender have their own GL/Qt rendering
  elsewhere in the same process, so the codec must never leave a foreign EGL
  context current on a thread the host app might use for its own GL work
  next.

Given those constraints, duplicating the ~700 lines of shader/pipeline code
into a header — rather than refactoring `agc.c` itself to share it — was the
lower-risk choice: `agc.c`'s own test suite (`make test`) stays untouched and
still passes, and the hardened, embeddable version lives entirely in
`agc_core.h`, exercised independently by a small self-test before it was ever
wired into ffmpeg (build it yourself with `gcc -I. your_test.c -lEGL -lGL
-lpthread`, calling `agc_gl_lock`/`agc_progs_init`/`agc_plane_init`/
`agc_encode_plane`/`agc_decode_plane`/`agc_gl_unlock` in that order).
De-duplicating the two is future work, tracked as a known limitation, not
done here because agc.c already has a working, tested implementation and
touching it wasn't necessary to reach Shotcut/Blender.

### Building it

```sh
cd gpucodec/ffmpeg
./build.sh
```

This fetches the exact Debian ffmpeg source matching your installed `ffmpeg`
(`apt-get source ffmpeg` — requires `deb-src` lines in
`/etc/apt/sources.list`; the script adds them if missing), reuses the
*system* ffmpeg's own `./configure` flags verbatim so the result is a drop-in
superset (everything the system build has, plus `agc1`), and installs to
`~/build/ffmpeg-agc1-install` by default — **never touching
`/lib/aarch64-linux-gnu/libavcodec.so.59`**. Build with `AGC1_MAKE_JOBS=2`
(the default) on a RAM-constrained device; a full rebuild with every optional
library enabled took a little under 20 minutes at `-j2` here.

Verify it built and registered correctly:

```sh
LD_LIBRARY_PATH=~/build/ffmpeg-agc1-install/lib \
  ~/build/ffmpeg-agc1-install/bin/ffmpeg -encoders | grep agc1
```

### Wiring it into Shotcut and Blender

Overwriting the system `libavcodec.so.59` was deliberately avoided — it's a
shared library every application on the system depends on, replacing it is
hard to reverse cleanly, and this device doesn't have RAM to spare for
troubleshooting a mistake there. Instead, both `start-shotcut-gpu` and
`start-blender-gpu` (the existing GPU launch wrappers) put the new build
**first** in `LD_LIBRARY_PATH` before `exec`ing the real application:

```sh
export LD_LIBRARY_PATH="$HOME/build/ffmpeg-agc1-install/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

Order matters: it has to come before the Mesa/Adreno GPU library path these
wrappers already set up, or the dynamic linker finds the *system*
`libavcodec.so.59` first (it's also on that path) and `agc1` is invisible
again. Both wrapper scripts already get this right — if you're writing your
own launcher, put the AGC1 lib dir first.

With that env var set, both apps pick up `agc1` decode automatically, no
application changes needed:

* **Shotcut**: verified with `melt` (the MLT engine Shotcut embeds) — it
  correctly reads an `agc1`-encoded `.avi`'s geometry and frame count, and
  rendering a frame with `melt file.avi -consumer avformat:frame.png`
  produces real, correct pixel data.
* **Blender**: verified headlessly — `bpy` can add an `agc1`-encoded `.avi`
  as a VSE movie strip and reports the correct frame count and resolution
  (rather than the `0x0`/"not an anim" failure you get if the codec's GPU
  init fails, e.g. `LD_LIBRARY_PATH` ordering wrong or the Adreno/Turnip+Zink
  stack not active).

**Encode side is asymmetric.** Shotcut can already export `agc1` today —
`ffmpeg -c:v agc1` works from any app that lets you type an arbitrary codec
name into MLT's avformat consumer, which Shotcut's "Export File" does via its
advanced/custom profile options. **Blender cannot**: its FFmpeg export
codec dropdown is a fixed enum compiled into Blender itself
(`BKE_writeffmpeg.c`), not something a shared library swap can extend — that
would need a Blender source patch and a full Blender rebuild, which was
judged disproportionate to AGC-1's actual benefit (transient CPU offload
during encoding) given this device's RAM headroom. The practical path for
Blender is a proxy step after render, not a native menu option:

```sh
# Render normally to an image sequence, then compress the sequence with AGC
# for fast transient storage/transfer, the same way agc-from does for video:
ffmpeg -framerate 30 -i render_%04d.png -pix_fmt yuv420p -f rawvideo - \
  | agc encode - render.agc -s 1920x1080 -f yuv420p -q 80 -r 30
```

Decoding that back into Blender's VSE, or into Shotcut, works today via the
`agc1` codec above — only Blender's own *export* dropdown is out of reach
without patching Blender itself.

### Packet layout

An `agc1` `AVPacket` is a plain concatenation of each plane's compressed
bitstream (Y, then U, then V for `yuv420p`; just Y for `gray8`), with a small
length header per plane — no need to reuse `agc.c`'s own on-disk container
(magic/version/quality/tile-log/etc.), since `AVCodecContext` already carries
width, height and pixel format from the surrounding container:

```
repeated per plane:
    uint32_t nTiles        (little-endian)
    uint16_t tileBits[nTiles]
    uint32_t nWords
    uint32_t words[nWords]  -- the packed bitstream itself
```


## Bitstream

Per block, bit-exact, nothing padded between blocks:

| field | bits | |
|---|---|---|
| coded | 1 | 0 means the block is entirely zero and nothing follows |
| lastPos | 6 | zigzag index of the last significant coefficient |
| mask | lastPos+1 | significance flags in zigzag order |
| magnitudes | variable | one per significant coefficient |

Each magnitude is a unary size prefix — (s−1) ones then a zero — followed by s
bits holding the sign in the top bit and the low s−1 magnitude bits, with the
leading 1 implicit. A ±1 coefficient therefore costs 2 bits rather than a fixed
width. Together with removing padding, these changes cut the stream by **60%**
at identical PSNR.

## Container

All multi-byte fields little-endian.

```
magic   4  "AGC1"
version 1
format  1  0 = gray8, 1 = yuv420p
quality 1  1..100
tileLog 1  log2(blocks per tile) = 5
width   4  real luma width
height  4  real luma height
planes  4
fps     4  frames per second x1000, 0 if unknown
per plane:  nTiles(4) nWords(4) crc32(4)
            tileBits(2*nTiles, padded to 4)  payload(4*nWords)
```

Each plane carries a CRC32 over its index and payload. A stored frame that
silently decodes to garbage is worse than one that refuses to decode, so a
single flipped bit is reported rather than rendered.

**A sequence is simply these frames written one after another.** Every frame
repeats the full header, which costs 24 bytes but means each one is completely
self-contained: a stream damaged in the middle still decodes everything before
the damage, and then reports it rather than pretending the clip was shorter.
Because a frame is self-delimiting, no index or length prefix is needed to find
the next one.

Self-containment alone is not enough to survive a Ctrl-C, though — it bounds
the loss to the frame being written, but that frame is still torn. So the
encoder also stops *between* frames rather than wherever the signal lands,
which is what makes an interrupted capture a complete stream and not a damaged
one.

Nothing in the stream can be located by seeking, so validation cannot rely on
the file size — a pipe has none. Instead both counts are checked for exact
agreement: `nTiles` must equal what the frame dimensions imply, and `nWords`
must equal what the tile index sums to. That is stricter than a size check and
works identically on a file and on stdin.

## Files

| | |
|---|---|
| `agc.c` | the codec: shaders, container, CLI |
| `glboot.h` | headless EGL + GL 4.3 compute bootstrap |
| `test.sh` | test suite |
| `tools/agc-from`, `tools/agc-to` | conversion to and from ordinary video |
| `tools/agc-play`, `tools/agc-rec` | playback and X11 screen capture |
| `bench.c` | early viability benchmark, kept for its CPU-reference DCT check |
| `cpu_sad.c` | CPU motion-search baseline used to decide against inter coding |
| `agc_core.h` | the reusable, hardened GPU pipeline — see "Embedding AGC-1" |
| `ffmpeg/` | `agc1`: AGC-1 as a real `libavcodec` codec for Shotcut/Blender |

## Notes for anyone extending this

- `GL_MAX_COMPUTE_WORK_GROUP_COUNT` is 65535 per dimension. A 1D dispatch of
  129,600 silently does nothing and reports no error; only a CPU-reference
  check catches it.
- **Do not leave `glFinish` in the per-frame path.** The stage timers originally
  drained the pipeline four or five times per plane, which is fifteen full
  stalls for a colour frame. It is invisible on a single large frame and
  crippling on a stream: a 640×480 frame cost as much as a 4K one. Making the
  drains conditional on bench mode made encode 2.6× and decode 2.2× faster with
  a byte-identical bitstream. Barriers order dependent dispatches;
  `glGetBufferSubData` synchronises on its own when the result is read back.
- Readback here runs at ~0.4–1.2 GB/s against ~17–27 GB/s upload, which is why
  entropy coding must happen on the GPU: only the compressed stream should ever
  cross the bus.
- In-shader zeroing of the coefficient buffer is far slower than
  `glClearBufferData`, because adjacent threads write 64 ints apart and the
  writes never coalesce.
- Assert `/proc/self/cpuset` when benchmarking. Backgrounded, this container
  gets 4 throttled cores; focused, it gets all 8.
- **Do not hand a player raw frames.** VLC's `rawvideo` demuxer supplies no
  timestamps, so VLC starves its own picture pool and drops frames with
  `buffer deadlock prevented`. Wrapping the identical bytes in Matroska first
  (`-c:v rawvideo`, a copy, not a re-encode) took the same playback from 12
  such errors to 0.
- **ffplay is useless on this display.** It requests a GLX context and
  termux-x11 answers `BadValue` to `X_GLXCreateContext`, so it shows nothing
  while appearing to run normally. `SDL_VIDEODRIVER`/`SDL_RENDER_DRIVER` do not
  help. VLC works.
- **VLC's GL output is silently black here unless you pass `--gl=glx`.** It
  defaults to the `egl_x11` provider, EGL cannot get a DRI3 device, and VLC
  reports no error at all: the window opens, the seek bar advances, the clip
  "finishes", and every pixel is black. Over 20 s of 4K, with the window
  captured during each sample: `gl`+`glx` = picture at 103%; `gl`+`egl_x11`
  (the default) = BLACK at 82%; `gles2` = BLACK at 87%; `xcb_x11` = picture
  but 157%. `start-vlc-gpu` now forces `--gl=glx`.
- **Verify the picture and the CPU in the same breath.** The broken video
  outputs look *better* on CPU precisely because they draw nothing, so a
  playback benchmark that only reads a CPU counter will quietly rank the
  blank ones first. There is no `xdotool` here; screenshot VLC's own window
  instead — `xwininfo -root -tree | grep 'VLC media player'` for the id, then
  `import -window <id>`, then `identify -format "%[fx:mean] %[fx:standard_deviation]"`.
  **Test the mean first:** below ~0.15 is black, full stop. Two earlier
  versions of this check got it wrong — testing stddev first calls a black
  video area with a visible menu bar and seek bar a picture (mean 0.05 but
  stddev 0.21), and a *high* mean with stddev < 0.12 is the white playlist
  screen, i.e. not playing yet. File size is the quickest sanity check of all:
  a black 4K capture is ~18 KB, a real one ~2 MB.
- **Neither `/proc/stat` nor `times` can measure playback here.** `/proc/stat`
  is frozen inside this PRoot container — two reads seconds apart are
  byte-identical — so system-wide accounting reads 0%. `times` fails
  differently: killing a pipeline wrapper mid-`wait` means it never reaps
  `agc`/`ffmpeg`/`vlc`, so their time never reaches `cutime` and a busy
  pipeline scores 0%. Walk the live process tree in `/proc/<pid>/stat`
  (`utime+stime+cutime+cstime`) and diff two samples while it still runs.
- A codec that is stopped with Ctrl-C has to stop *between* frames. Install a
  handler with `signal()`, not bare `sigaction` without `SA_RESTART`: an
  interrupted `fread` returns a partial frame, which is indistinguishable from
  a truncated input and defeats the very check that makes truncation visible.
