# selinux-bridge: real hardware MediaCodec, reached from Debian/PRoot

## Why this exists

The rest of this repo (`agc.c`, `agc_core.h`, `ffmpeg/`) is a **GPU-compute**
codec that works entirely from inside the Termux/PRoot Debian container,
because OpenGL/Vulkan device nodes (`/dev/kgsl-3d0`) are world-open.

**Real hardware video encode/decode (Qualcomm Codec2, `c2.qti.*`) is a
separate story.** It was investigated in depth (see the main README's
"Hardware codec" section and `COPILOT-HANDOFF.md` §2.5l) and is blocked for
any process running as the Termux/PRoot shell, for a specific, verified
reason:

- Codec2 backs its buffers with DMA-BUF from `/dev/dma_heap/*`, which is
  **SELinux-denied** to the shell's `untrusted_app_27` domain.
- The `app_process`/scrcpy route (running a real ART/ Java runtime) also
  fails: `/system/bin/app_process64` is labeled `zygote_exec`, which
  `untrusted_app_27` cannot execute into. It dies with `SIGABRT` during ART
  startup, no diagnostics (logd is denied too).

Both of those restrictions are properties of the **shell's own app identity**,
not of the device or the codec hardware. A normal, installed APK gets a
completely different identity — a real UID and SELinux domain assigned by
Zygote/`installd` at install time — and normal apps use MediaCodec routinely
with no special permission at all.

**`selinux-bridge/` is that APK — "SELinux Hardware Bridge".** Today it's a
minimal Android app whose only job is to expose real hardware `MediaCodec`
(H.264 encode and decode) over a loopback TCP socket, so the Debian/PRoot
side can drive it directly — but the name and protocol design are meant to
generalize: any hardware the shell is SELinux-blocked from (camera capture
is the next obvious candidate) fits the same "run a real app, forward it
over a local socket" pattern, as an additional `mode=` on the same bridge
rather than a new app. The name is deliberately literal about the *reason*
this exists rather than today's one feature: if you come back to this
months later having forgotten why it's installed, "SELinux Hardware
Bridge" on the home screen (and the in-app subtitle, "Gives your
Termux/PRoot shell hardware access it can't reach alone") should be enough
to remind you.

## What's here

| File | Purpose |
|---|---|
| `AndroidManifest.xml` | One activity, one foreground service, one boot receiver. `INTERNET` only for the loopback socket; `RECEIVE_BOOT_COMPLETED` and `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` for the gap-#4 mitigations. |
| `app/src/main/java/com/selinuxbridge/app/MainActivity.java` | Launcher UI; starts the bridge service in the foreground so Android doesn't kill it, and shows the current Doze state with a one-tap battery-optimisation exemption. Everything is drawn programmatically (gradients, a small chip-logo icon, a pulsing "alive" status dot, a data-flow diagram) rather than a blank white screen — see screenshot note below. |
| `app/src/main/java/com/selinuxbridge/app/BridgeService.java` | The actual bridge: `ServerSocket` on `127.0.0.1:7878`, wire protocol below, drives `android.media.MediaCodec` synchronously, explicitly preferring hardware (`c2.qti.*`) codec components, admits sessions through a semaphore sized from the device's own codec-instance limit, one thread per client so a crashed session doesn't take down the accept loop, and logs to a file it can also stream back over the socket. |
| `app/src/main/java/com/selinuxbridge/app/BootReceiver.java` | Restarts the service after a reboot (`BOOT_COMPLETED`/`QUICKBOOT_POWERON`) and after an in-place update (`MY_PACKAGE_REPLACED`), so the two recoverable halves of gap #4 need no human. |
| `app/src/main/java/com/selinuxbridge/app/I420.java` | Conversion between the tightly packed I420 the wire protocol carries and whatever layout MediaCodec actually hands out (padded row strides, padded slice heights, semi-planar chroma). Deliberately framework-free so it can be unit tested on a normal JVM. |
| `bridge_client.c` | Debian-side client/CLI: connects over loopback, sends raw frames or a bitstream (from a file or a pipe), prints round-trip stats and the codec name actually used. Supports `encode`/`decode`/`info`/`log`, h264/hevc/vp9/av1 via `-c`, bounded socket timeouts with a distinct exit code, a streaming Annex-B splitter with O(1) memory, and v4 format records so `decode` needs no dimensions. |
| `build.sh` | Rebuilds the signed APK from source using raw SDK command-line tools (`aapt2`, `javac`, `d8`, `apksigner`) — no gradle/network dependency. Signs with a persistent key in `keystore/` so rebuilds install as in-place updates. |
| `keystore/` | Local debug signing key, gitignored. Kept outside `build/` so `build.sh`'s clean step cannot destroy it (see gap #9). |
| `../ffmpeg/selinuxbridge.c` | libavcodec wrapper registering `h264_selinuxbridge` / `hevc_selinuxbridge` as real ffmpeg **encoders and decoders**. |
| `../ffmpeg/selinuxbridge-ffmpeg.patch` | The three-file registration patch (`allcodecs.c`, `Makefile`, `configure`) that ffmpeg's build needs. |
| `tools/bridge-status` | Health-check script: reports whether the app is reachable and prints its `info` diagnostics (device, Android version, all four video codecs and which are hardware-backed, concurrency limit). `--log` also dumps the app's log. |
| `tools/hw-transcode` | Wrapper that pipes an arbitrary ffmpeg-readable input through the bridge's hardware encoder and remuxes to a normal container — the hardware-codec analogue of this repo's `agc-from`/`agc-to`. `-c hevc` selects HEVC; `-D` also decodes on the hardware block, making the transcode hardware end to end. |
| `tools/selftest` | Full regression suite. Needs no phone and no installed APK: it runs everything against the mock bridge, including fault injection that cannot be arranged reliably on real hardware. |
| `tools/mock-bridge.py` | A protocol-v4 bridge that reproduces the awkward parts of MediaCodec — codec-config packets, one packet per access unit, lookahead that swallows frames before emitting anything, decode format records — backed by a real x264/x265 subprocess so the output is a genuine stream. `MOCK_RESIZE_AT=N` forces a mid-stream resolution change, which is impractical to provoke on real hardware. |
| `tools/test-i420` | JVM unit tests for `I420.java` across planar, semi-planar, padded-stride, padded-slice-height, odd-dimension and cropped layouts. |

## Building

Requires an Android SDK with build-tools and a `platforms/android-34`
`android.jar`. On this project's dev machine that's a Termux-installed SDK at
`/opt/Android/sdk`; point `ANDROID_SDK_ROOT` elsewhere if yours differs.

```
./build.sh
```

Produces `build/apk_final/selinux-bridge.apk` (signed with a throwaway debug
key generated on first run) and `./bridge_client`.

**Non-obvious build detail**: this SDK ships build-tools in two flavors —
an ARM64-native one (`34.0.0`, only has `aapt2`/`aidl`/`zipalign`/`dexdump`)
and an x86-64 one (`34.0.0-2`, has `d8`/`apksigner` as well but its *native*
binaries SIGILL under Termux/PRoot on ARM64 hardware with no x86 emulation).
`d8` and `apksigner` are plain Java tools though (`d8.jar`, `apksigner.jar`),
so the script runs them via `java -cp ... com.android.tools.r8.D8` /
`com.android.apksigner.ApkSignerTool` directly, sidestepping the
architecture mismatch entirely. Only `aapt2`/`zipalign` need the native
`34.0.0` directory.

## Installing

This machine has no running `adbd` (it would need to connect to itself), so
sideload normally: the built APK is copied to `/sdcard/Download/selinux-bridge.apk`
by `build.sh`'s caller — open it with a file manager and allow install from
this source, or enable *Settings → Developer options → Wireless debugging*,
pair with `adb pair`, then:

```
adb connect <phone-ip>:<port>
adb install -r selinux-bridge/build/apk_final/selinux-bridge.apk
```

**Migration note**: this project (directory, Java package, and APK
filename) was renamed from `android-bridge`/`com.gpucodec.bridge`/
`gpucodec-bridge.apk` to `selinux-bridge`/`com.selinuxbridge.app`/
`selinux-bridge.apk` early on, while it was still safe to do a clean
rename rather than carry a legacy package name forever. Because the
Android package name changed, this installs as a **separate app**, not an
update — Android has no way to know it's "the same app" under a new
package ID. If you previously installed the old `com.gpucodec.bridge`
build, uninstall it manually (it will keep running/showing up separately
otherwise) after confirming the new one works.

Launch the app once and leave it open (or in the recent-apps list — it runs
as a foreground service with a persistent notification, so Android won't
kill it). It listens on `127.0.0.1:7878` for as long as it's alive.

The landing screen is a dark, gradient-themed dashboard rather than a blank
white page: a chip-logo header, a pulsing green "listening" status card, a
small data-flow diagram (`PRoot shell → Bridge :7878 → hardware codec`),
and a short feature list. All of it is drawn with plain Java (canvas
shapes, `GradientDrawable`) — no image assets, so it doesn't need any
network-fetched design tooling.

### Branding

- **Name**: "SELinux Hardware Bridge" (package `com.selinuxbridge.app`,
  unchanged, to avoid an unnecessary rename churn). This reflects the
  app's actual mechanism and intended scope, not just its current single
  use case: the root cause it exists to work around is always the same
  **SELinux domain restriction** on the Termux/PRoot shell (`untrusted_app_27`
  is denied things a normal installed app's domain is allowed), and a
  real installed APK is the fix regardless of *which* hardware is being
  reached. Today that's the Codec2 hardware video codec (see below); the
  same app/socket pattern is intended to be extended to other
  SELinux-gated hardware a PRoot shell can't reach directly (e.g. camera
  capture) as separate `mode=` request types on the same bridge, without
  needing a new APK per hardware class.
- **Important clarification**: this app does not itself use the GPU. Its
  current (only, so far) capability reaches Qualcomm's dedicated hardware
  *video codec* block (Codec2, `c2.qti.*`) — a separate fixed-function
  ASIC from the Adreno GPU. AGC-1 (the rest of this repo, `agc.c`/
  `agc_core.h`) is the part that actually runs on the GPU via OpenGL
  compute shaders. The two are complementary, not the same mechanism —
  see the main README's architecture overview if that distinction matters
  for your use case.
- **Icon**: a real launcher icon (adaptive icon, `res/mipmap-anydpi-v26/`),
  not the default Android placeholder. It reuses the same chip motif as
  the in-app header, with a terminal `>_` prompt glyph embedded in the
  chip's core — visually saying "a shell talking to a chip" in one glance.
  Pure hand-written vector drawables (`res/drawable/ic_launcher_foreground.xml`,
  `ic_launcher_background.xml`), no bitmap assets, no generated/downloaded
  art. `build.sh`'s `aapt2 link` step was updated to pick up all compiled
  resources (previously it only linked the one `strings.xml` file, since
  that was all that existed) — see its comments for the fix.
- **Trademark note**: neither the name nor the icon references any
  trademarked product, chip vendor, or brand (no "Qualcomm", "Android"
  logo, or similar) — "SELinux" is a plain descriptive reference to the
  Linux Security Modules framework (itself not a vendor trademark; NSA
  open-sourced it in 2000), used the same way this repo's own docs already
  refer to it throughout.

## Wire protocol (v5)

All integers are 4-byte big-endian (`DataInputStream`/`DataOutputStream`
network order). One TCP connection = one session.

1. Client → server, once:
   `mode width height fps bitrate codec`

   | field | meaning |
   |---|---|
   | `mode` | `0` encode, `1` decode, `2` info, `3` log |
   | `width`/`height` | frame size; decode may send `0 0` and learn the real size from the format record below |
   | `fps`/`bitrate` | encode only; send 0 otherwise |
   | `codec` | bits 0–7: `0` h264, `1` hevc, `2` vp9, `3` av1 — **new in v3**.<br>bits 8–15: rate control, encode only — `0` CBR, `1` VBR, `2` CQ — **new in v5**. A v3/v4 client sends a bare `0..3`, so its high bits are zero and it gets CBR, which is the fix for gap #14. In CQ the `bitrate` field carries a quality in 1..100 instead of bits/s. |

   Modes 2 and 3 ignore everything after `mode`, but the handshake shape is
   fixed, so dummy values are still sent.

2. Server → client, once: `int32 status` (`0` = OK, nonzero = failed) then
   `int32 len` + `len` UTF-8 bytes — the **name of the codec component
   actually selected** on success (e.g. `c2.qti.avc.encoder`, or `n/a` for
   modes 2/3), or a human-readable error message on failure (e.g. `no
   encoder for video/avc`, or `all 3 hardware codec slots are busy`).

3. For `info` (mode 2) and `log` (mode 3): the server sends one or more
   length-prefixed UTF-8 chunks followed by `-1` EOS — no client input is
   expected or read. `info` returns device/codec diagnostics; `log` returns
   the app's own `bridge.log`, which scoped storage otherwise hides.

4. For `encode`/`decode` (modes 0/1), then, repeated:
   - Client → server: `int32 length` then `length` bytes (one raw YUV420
     flexible frame for encode, or one Annex-B unit for decode).
     `length == -1` signals end-of-stream, no bytes follow.
   - Server → client: `int32 length` then `length` bytes of the
     corresponding output (the coded bitstream for encode, raw YUV420
     flexible for decode). `length == -1` on the final reply signals
     end-of-stream.

5. **Format records — new in v4, decode only.** The server's output stream
   may carry `int32 -2` followed by `int32 width` and `int32 height`. One
   always precedes the first frame, and another is emitted whenever the
   picture size changes mid-stream. Every frame after a record is tightly
   packed I420 at those dimensions.

   This is what lets a decode client stop guessing: the picture size lives
   in the bitstream's parameter sets, not in anything the caller knows, and
   MediaCodec reports it (crop rectangle included) only once it has actually
   decoded something. `bridge_client decode in.h264 out.yuv` therefore takes
   no dimensions at all, and the libavcodec decoder gets what it needs to
   call `ff_set_dimensions()` at the right moment.

   Lengths of `-3` or below are reserved. A client must treat one as a
   protocol error rather than guessing — otherwise a future record type
   would be read as a frame length and desynchronise the stream silently.

### Version history

| version | change |
|---|---|
| v1 | bare `int32` status, no codec name or error message |
| v2 | status gained a length-prefixed codec-name/error-message field; added `info` (mode 2) |
| v3 | added the `codec` handshake field (hevc/vp9/av1) and `log` (mode 3) |
| v4 | added decode format records (`-2 w h`), so decode dimensions are optional and a mid-stream resolution change is a normal event |
| v5 | added rate control in the codec field's high byte (CBR/VBR/CQ), fixing a 25–34% bitrate overshoot. The header is unchanged, so this is the one version bump that *is* backward compatible |

Versions are **not** wire-compatible with each other. A v3 client talking to
a v2 server happens to work for `info` (the extra `codec` int is simply never
read before the server replies), and a v4 client talking to a v3 server works
for everything except decode (a v3 server simply never sends a format record,
so the client has no size to report). v5 is the deliberate exception: it
reuses spare bits of an existing field rather than adding one, so a v5
client at its default talks to a v4 server unchanged, and asking a v4
server for a mode it does not have fails loudly (`unknown codec id 256`)
instead of being silently ignored — both confirmed on real hardware.
Otherwise encode/decode will mis-frame — update both sides together. `bridge_client info` prints the
server's protocol version, which is the quickest way to spot a mismatch.

### Stale process after an update

Installing a new build does **not** reliably restart the service. Android
usually kills the process on replacement, but a foreground service can
survive it, and `startForegroundService()` then only delivers another
`onStartCommand()` to the classes already loaded — it never reloads code.
The bridge keeps answering, on the old protocol, with nothing to show for
it but a version number that does not change.

`bridge_client info` reports both the protocol version and the **build
timestamp of the code actually answering**, and flags the mismatch
outright:

```
protocol: v5
build: 2026-09-25 11:03:00
STALE PROCESS: the package was replaced at 11:14:22, after this process started.
  The update did not restart the service, so this is still the old
  build. Force-stop the app and reopen it (or reboot).
```

When it detects this, the app shows a banner with a **Restart now**
button. The service is `START_STICKY`, so ending the process makes
Android start it again, and *that* start loads the new code. The button
refuses while any transcode is in flight, so it cannot cut off a stream
mid-stripe.

Failing that, **force-stop the app and reopen it** — Settings → Apps →
SELinux Hardware Bridge → Force stop. A reboot works too, and
`BootReceiver` brings the service back on its own afterwards.

### Concurrency limit

Codec2 caps how many codec instances can exist at once, and **blocks** in
`configure()`/`start()` past that cap rather than failing — which is how
4 concurrent sessions used to hang the bridge outright. `BridgeService`
now queries `CodecCapabilities.getMaxSupportedInstances()` across the
`c2.qti.*` video components, keeps one instance in reserve, and admits
sessions through a fair counting semaphore. A session that finds no free
slot waits 5 seconds, then gets a `busy` error it can act on. `info`
reports both the limit and the number of free slots.

### Client exit codes and environment

`bridge_client` returns `0` ok, `1` error, `2` usage, **`3` bridge stalled**.
Exit 3 is deliberately distinct so scripts can tell "the app is being
throttled, try again" apart from a real failure.

| variable | default | meaning |
|---|---|---|
| `BRIDGE_TIMEOUT` | `30` | seconds of socket inactivity before giving up |
| `BRIDGE_RETRIES` | `1` | retries after a timeout (file input/output only) |
| `BRIDGE_PORT` | `7878` | port the bridge listens on |

Retries are automatically disabled when either side is `-` (stdin/stdout),
because a pipe cannot be rewound and retrying would silently truncate the
output.

### Hardware codec selection

`BridgeService` enumerates `MediaCodecList(REGULAR_CODECS)` and prefers the
first encoder/decoder component for the requested MIME type whose name starts with `c2.qti.`
(Qualcomm's hardware Codec2 tier) over generic `createEncoderByType()`/
`createDecoderByType()`, which make no such guarantee and can silently hand
back a software (`c2.android.*`/`OMX.google.*`) component instead. If no
`c2.qti.*` component exists (e.g. on a non-Qualcomm device), it falls back
to the default factory method so the bridge still works, just without the
hardware guarantee — and the selected name is always reported back to the
client either way, so this is verifiable from the Debian side without ever
touching Android Studio.

### Diagnostics without adb

`logcat`/`dumpsys` are denied to the Termux/PRoot shell's SELinux domain, so
there was previously no way to see the app's internal state or errors from
the Debian side. `BridgeService` now also writes a timestamped log to
`getExternalFilesDir(null)/bridge.log` (typically
`/sdcard/Android/data/com.selinuxbridge.app/files/bridge.log`), readable
directly from Debian:

```
cat /sdcard/Android/data/com.selinuxbridge.app/files/bridge.log
```

**Caveat, found while verifying this on-device**: on Android 11+ (this
device is Android 17), `Android/data/<package>/` is scoped-storage-sandboxed
and denies directory listing/reads from *other* apps/shells, including this
one, even under `/sdcard` — `find /sdcard/Android/data/com.selinuxbridge.app`
returns "Permission denied" from the Debian side. So reading the file
*directly* needs a root shell, `adb shell run-as`, or the device's own Files
app with "show system files".

That is why the log is also served **over the socket**, which has no such
restriction:

```
./bridge_client log          # the app's bridge.log, straight to stdout
tools/bridge-status --log    # diagnostics followed by the log
```

Between that and `info`, everything the app knows about itself is reachable
from Debian with no adb and no root.

## Testing without a device

Most of the bridge can be exercised with no phone attached, no APK installed
and no hardware codec, which matters for two reasons: sideloading here needs
a physical install tap, so nothing could otherwise be checked before that
happens; and the interesting failure modes — a bridge that accepts a
connection and then goes quiet, a bridge that is out of codec slots, a
semi-planar chroma layout — cannot be arranged reliably on real hardware.

```sh
./tools/selftest          # 30 checks, ~3 min
./tools/test-i420         # 10 plane-layout unit tests on a plain JVM
```

`tools/selftest` builds `bridge_client`, starts `tools/mock-bridge.py`
alongside two scripted fault-injection servers, and checks:

- encode, decode and HEVC round trips, and streaming over stdin/stdout;
- every documented exit code (0/1/2/3), including that a `busy` refusal
  fails immediately instead of pointlessly retrying;
- that peak RSS stays flat when the input grows 8x (the O(1) splitter);
- that a v4 decode works with **no dimensions supplied**, that the format
  record reports the true picture size, that a mid-stream resolution change
  is announced and produces exactly the right number of bytes, and that an
  unknown record type is refused rather than mistaken for a length;
- that both ffmpeg encoders **and both decoders** produce valid, decodable
  output with surviving timestamps;
- that Annex-B output keeps its parameter sets inline;
- that a stalled bridge fails the encode *or the decode* in seconds rather
  than hanging.

It skips the ffmpeg sections cleanly if no build with the `selinuxbridge`
codecs is present; point it at one with `FFMPEG=/path/to/ffmpeg`.

## Testing the bridge

Once the app is installed and open on-screen:

```
# health check + hardware codec inventory
./tools/bridge-status

# encode: 320x180 raw NV12/I420 frames -> H.264 Annex B
./bridge_client encode 320 180 30 2000000 in.yuv420 out.h264

# decode: an H.264 elementary stream -> raw YUV420 frames (concatenated)
./bridge_client decode 320 180 in.h264 out.yuv420

# transcode an arbitrary file through the hardware encoder end-to-end
./tools/hw-transcode input.mp4 output.mp4
```

`bridge_client encode`/`decode` now print the codec name the server
actually selected, e.g.:

```
handshake ok, codec selected on-device: c2.qti.avc.encoder
```

If the codec name starts with `c2.qti.`, that's Qualcomm's real hardware
Codec2 component doing the work — not software (`c2.android.*`) and not
AGC-1. This is now reported directly by the protocol; no logcat access is
needed to confirm it, unlike in the original v1 test below.

### Verified result (this device, this session, protocol v1)

Installed via sideload (`/sdcard/Download/selinux-bridge.apk`, after fixing
a Play Protect "unsafe app" block caused by a missing `targetSdkVersion` —
see git history), launched, left open. From the Debian/PRoot side:

```
$ ./bridge_client encode 320 180 30 2000000 test.yuv420 out.h264
handshake ok, codec configured on-device
done: sent=30 units, received=31 units       # 30 frames + EOS marker

$ ffprobe out.h264
codec_name=h264, width=320, height=180        # a real, valid H.264 stream

$ ./bridge_client decode 320 180 out.h264 redecoded.yuv420
handshake ok, codec configured on-device
done: sent=32 units, received=30 units       # SPS+PPS+30 slices in, 30 frames out
```

Full round trip: raw frames → real hardware H.264 encode → real hardware
decode → 30 frames back out, over a loopback socket from a normal Debian
process. The decoded output arrived as 110,528 bytes/frame rather than the
tightly-packed 86,400 (`320*180*1.5`) — that's stride/slice-height padding,
a hallmark of Qualcomm's actual hardware decode output layout (software
reference decoders return tightly-packed frames). That observation turned
out to be the visible edge of gap #10: the same padded, semi-planar layout
was silently corrupting the *encode* path's chroma too. Both directions now
go through `I420.java`, so the wire always carries tightly packed I420. Fixed one real client-side
bug along the way (see `bridge_client.c` comments): a naive
write-frame-then-wait-for-its-reply loop deadlocks against MediaCodec's
lookahead latency, and sending a whole elementary stream as a single decode
input unit only returns a fraction of the frames — both fixed by splitting
Annex-B NALs individually and decoupling send/receive onto a writer thread.

**Conclusion: confirmed.** A real, installed, signed APK gets a normal
Zygote-forked app identity that unlocks the hardware codec the Termux/PRoot
shell is denied. This is not inference — it is a directly observed,
reproducible encode+decode round trip on this exact device.

### Production hardening (protocol v2)

Building on the confirmed v1 result above, the service and client were
hardened for everyday use rather than one-off validation:

- Explicit hardware-codec selection (see above) instead of hoping the
  default factory method picks `c2.qti.*`.
- The selected codec name (or a real error message) is now part of the
  wire protocol instead of requiring logcat to confirm.
- Per-client exception isolation: one crashed/misbehaving session logs and
  closes cleanly instead of taking the whole app down.
- File-based logging (`bridge.log`) readable from Debian without adb.
- `bridge_client` gained stdin/stdout (`-`) support, a connection-refused
  message that explains *why* (app not open) instead of a bare `ECONNREFUSED`,
  and a clean error+exit instead of a silent hang if the app is killed
  mid-transfer.
- Two new tools, `tools/bridge-status` and `tools/hw-transcode`, so this
  isn't just a raw protocol but something pastable into a normal workflow.

This is a genuine breaking wire-protocol change (v1 servers/clients cannot
talk to v2 clients/servers) — see "Wire protocol (v2)" above.

### Verified result (this device, this session, protocol v2)

APK reinstalled (physical sideload tap, same install step as v1), reopened,
then re-ran the full test matrix from the Debian/PRoot side:

```
$ ./bridge_client info
connected (bridge is alive)
GPUCodec Bridge diagnostics
device: SM-F971U1 (qcom)
android: 17 (sdk 37)

AVC (H.264) codecs available:
  decoder: c2.qti.avc.decoder [hardware]
  decoder: c2.qti.avc.decoder.low_latency [hardware]
  encoder: c2.qti.avc.encoder [hardware]
  decoder: c2.android.avc.decoder
  encoder: c2.android.avc.encoder
  ... (software OMX.google.*/OMX.qcom.* entries omitted here)

$ ./bridge_client encode 320 180 30 2000000 test.yuv420 out.h264
handshake ok, codec selected on-device: c2.qti.avc.encoder
done: sent=30 units, received=31 units

$ ./bridge_client decode 320 180 out.h264 redecoded.yuv420
handshake ok, codec selected on-device: c2.qti.avc.decoder
done: sent=32 units, received=30 units

$ ./tools/bridge-status         # health check, same info output as above
$ ./tools/hw-transcode testsrc.mp4 hwout.mp4
hw-transcode: 320x180 @ 30fps, bitrate=4000000 -> hardware encoder
handshake ok, codec selected on-device: c2.qti.avc.encoder
done: sent=30 units, received=31 units
hw-transcode: wrote hwout.mp4      # a normal, playable .mp4
```

**Confirmed**: the v2 protocol correctly reports the real hardware codec
component by name for both encode and decode, `bridge-status` and
`hw-transcode` both work end-to-end against the live app, and the codec
selection logic picked `c2.qti.*` over the available software alternatives
exactly as designed.

## Why this isn't a Termux:API contribution

Termux:API's plugin architecture (`TermuxApiReceiver` dispatching to per-feature
classes in `apis/`, streaming binary data over anonymous-socket file
descriptors from `am broadcast`) is actually a good structural fit for this —
a `MediaCodecAPI.java` following that same pattern would look natural there.
Two things make it impractical to pursue as a PR, though:

1. **Termux:API must be signed with Termux's own release key** for its
   permission model to work at all (see its README) — a fork or PR from
   outside that org can't be self-installed as a drop-in the way this repo's
   `selinux-bridge` APK can; it would need to actually be merged and shipped
   in an official release before anyone could use it.
2. **New-API PRs there don't appear to land quickly.** Checking the repo's
   recent closed PRs (as of this writing): a "feat: add calendar" addition
   sat open for about three weeks and was closed unmerged; a USB
   vendor/product-ID feature PR was closed unmerged too. No issue or PR has
   ever mentioned MediaCodec or hardware video.

So for anyone else in this situation, `selinux-bridge/` here is the more
useful thing to fork: it's standalone, buildable and sideloadable with
`build.sh` alone, and doesn't depend on anyone else's release cadence or
signing key.

## Using it from ffmpeg

`bridge_client` and `hw-transcode` are fine for scripting, but anything that
drives libavcodec directly (Shotcut, Blender, an ffmpeg one-liner) cannot
call them. So the bridge is also packaged as real libavcodec codecs — both
directions:

| name | codec id | components it drives |
|---|---|---|
| `h264_selinuxbridge` | `AV_CODEC_ID_H264` | `c2.qti.avc.encoder` / `c2.qti.avc.decoder` |
| `hevc_selinuxbridge` | `AV_CODEC_ID_HEVC` | `c2.qti.hevc.encoder` / `c2.qti.hevc.decoder` |

These attach to the **existing** H.264/HEVC codec ids rather than inventing
a new one (the same convention as `h264_v4l2m2m`, `h264_nvenc` and friends),
because the bridge emits bit-exact standard streams. That means no new codec
descriptor, no container tag, and every muxer already knows what to do with
the output — unlike the `agc1` codec in this repo, which is a genuinely new
format and needs all of those.

```sh
cd ffmpeg && ./build-selinuxbridge.sh      # patches + rebuilds ffmpeg

FF=~/build/ffmpeg-agc1-install
LD_LIBRARY_PATH=$FF/lib $FF/bin/ffmpeg -codecs | grep selinuxbridge

# hardware encode
LD_LIBRARY_PATH=$FF/lib $FF/bin/ffmpeg \
    -i input.mp4 -c:v h264_selinuxbridge -b:v 4M output.mp4

# hardware decode (the option goes before -i, like any decoder option)
LD_LIBRARY_PATH=$FF/lib $FF/bin/ffmpeg \
    -c:v h264_selinuxbridge -i input.mp4 -f rawvideo output.yuv

# both at once -- hardware end to end
FFMPEG=$FF/bin/ffmpeg ./tools/hw-transcode -D input.mp4 output.mp4
```

Private options: `-bridge_port` (default 7878, also read from `$BRIDGE_PORT`
so it matches `bridge_client`) and `-bridge_timeout` (seconds, default 30).
As with `bridge_client`, a stalled bridge becomes an encoder error rather
than a hung ffmpeg process.

Implementation notes worth knowing:

- **Threading.** MediaCodec has algorithmic lookahead and will swallow
  several frames before emitting anything, so a strict write-then-read loop
  deadlocks against it. A reader thread drains the socket into a packet
  queue while `encode2()` feeds frames in — the same decoupling already
  proven in `bridge_client.c`. `encode2` only ever blocks while ffmpeg is
  draining (`frame == NULL`), never while frames are still arriving.
- **Extradata has to exist before the first frame.** `mp4` writes its
  `avcC`/`hvcC` box when the header is written, which happens before
  anything has been encoded, and libavformat's late-extradata side-data path
  covers AAC/FLAC/AV1 but *not* H.264 or HEVC — so parameter sets arriving
  with the first packet are simply too late and the file comes out
  undecodable ("No start code is found"). When the caller asks for a global
  header the encoder therefore runs a throwaway probe session at init: one
  grey frame in, parameter sets out, session closed. It costs one extra
  codec open/close and is skipped entirely for Annex-B output.
- **Parameter sets, wherever they turn up.** MediaCodec normally emits
  SPS/PPS (VPS/SPS/PPS for HEVC) once as a standalone codec-config packet,
  but some components prepend them to every IRAP access unit instead. Both
  are handled: the leading run of parameter-set NALs is harvested into
  `avctx->extradata`, and a packet that is *nothing but* parameter sets is
  never emitted as a picture. For Annex-B output such a packet is held and
  prepended to the next real access unit, so the stream stays decodable on
  its own.
- **Timestamps.** The wire protocol carries no timestamps in either
  direction. Codec2 is configured without B-frames, so output order equals
  input order: each input pts is queued and one is popped per emitted
  packet, which restores them exactly. Without this the muxer warns
  "Timestamps are unset" and invents its own.
- **Decoding needs Annex-B, and gets it for free.** The decoders declare
  `FFCodec.bsfs = "h264_mp4toannexb"` / `"hevc_mp4toannexb"` exactly as
  `h264_mediacodec` does, so a length-prefixed `mp4`/`mov` source is
  converted before it ever reaches the bridge and `-c:v h264_selinuxbridge
  -i whatever.mp4` simply works.
- **Decoded dimensions come from the wire, not the container.** The decoder
  never parses the bitstream: it sizes its frames from the v4 format records
  and calls `ff_set_dimensions()` when one changes, which is also what makes
  a mid-stream resolution change survivable.
- **Decoded timestamps are reordered, not replayed.** Packets go in in
  decode order and frames come back in display order, so popping input
  timestamps FIFO would permute them on any stream with B-frames. The
  decoder instead pops the **smallest outstanding** pts per frame, which is
  by definition the next one to be displayed; for a stream without B-frames
  it degenerates to plain FIFO. (The encoder can use FIFO safely because it
  configures Codec2 without B-frames in the first place.)

## Stress test results (verified on this device)

Closing validation run against the installed `com.selinuxbridge.app` build.
All numbers are 60 frames of `testsrc` unless noted, measured end-to-end
from the Debian/PRoot side (`date`-delimited wall clock, bash `time` for
CPU).

### Throughput and scaling

| Resolution | Encode | Decode | Output | Frames verified |
|---|---|---|---|---|
| 640x360 | 0.15 s (393 fps) | 0.17 s (356 fps) | 145 KB | 60/60 |
| 1280x720 | 0.24 s (252 fps) | 0.27 s (220 fps) | 158 KB | 60/60 |
| 1920x1080 | 0.41 s (147 fps) | 0.28 s (218 fps) | 187 KB | 60/60 |
| 3840x2160 (30 fr) | 0.53 s (56 fps) | — | 353 KB | 30/30 |

Every output was re-probed with `ffprobe -count_frames`: correct
resolution, full frame count, no truncation. Decoded output runs 1.00–1.05x
the tightly-packed size, i.e. stride/slice-height padding — the expected
signature of genuine hardware output.

**4K works fine.** An earlier note in this file warned that ~12.4 MB
frames would need back-pressure before 4K was viable; measured, 4K encode
completes in 0.53 s for 30 frames with no special handling. That warning
was too cautious and has been corrected.

### Cost comparison, 1080p x 60 frames

| Path | Wall | CPU (user+sys) | Output | Notes |
|---|---|---|---|---|
| **Hardware bridge (Codec2)** | **0.47 s** | **0.12 s** | **187 KB** | inter-coded H.264 |
| AGC-1 (Adreno GPU compute) | 0.99 s | 0.49 s | 7.9 MB | intra-only, own format |
| x264 `veryfast` (CPU) | 1.06 s | 2.38 s | 291 KB | inter-coded H.264 |
| x264 `medium` (CPU) | 2.05 s | — | 260 KB | inter-coded H.264 |

The hardware path costs **~19x less local CPU than x264 `veryfast`** and
**~4x less than AGC-1**. Caveat, stated plainly: the bridge's CPU figure
counts only the *container* side. The real encode work happens inside the
APK's own Android process, which this measurement cannot see (and which
`/proc`-based sampling can't reach either, see the main README). The point
is not that the work is free — it's that it leaves the PRoot container's
CPU entirely, which is the whole reason to want it.

AGC-1's much larger output is expected and not a defect: it is an
**intra-only** codec (every frame coded independently, no motion
compensation), so on a near-static synthetic clip H.264's inter-frame
prediction wins enormously. That gap narrows sharply on
high-motion/scene-cut content, and AGC-1 buys properties H.264 can't
offer here (runs fully inside the container, no APK, no SELinux
dependency, deterministic bit-exact round trip).

### Does this help AGC-1? Measured, not assumed

The two do **not** compete for the same silicon, and this was verified
rather than argued: AGC-1 was timed at 1080p with the hardware codec idle,
then again with the codec saturated by a 40-session background loop.

| AGC-1 1080p | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| Hardware codec **idle** | 0.940 s | 0.912 s | 0.986 s |
| Hardware codec **saturated** | 0.939 s | 0.927 s | 0.929 s |

**No measurable contention** — the saturated numbers actually land inside
the idle run-to-run spread. So the honest framing is: the bridge does not
give AGC-1 *better* GPU access (AGC-1 already has unrestricted GPU access
via `/dev/kgsl-3d0`, and never needed help there). What it does is let
H.264 work run on a completely separate fixed-function block, in parallel,
leaving the GPU entirely free for AGC-1 or any other compute workload.
That's a scheduling win, not an access win.

### Robustness

| Test | Result |
|---|---|
| 25 sequential 720p sessions | 25/25 passed, 0.17 s → 0.23 s, no leak or drift |
| Client `SIGKILL`ed mid-stream | Service survived; `info` and a full encode both worked immediately after |
| 2 concurrent sessions | pass |
| 3 concurrent sessions | pass, 60/60 frames each |
| **4 concurrent sessions** | **FAILS** — see gap table below |

## Known gaps and potential fixes

Everything below was either observed during the stress run above or is a
known structural limitation. Severity is relative to the intended use
(a personal hardware-offload bridge for a PRoot container), not to a
hypothetical production service.

Gaps #1-#9 came out of the stress run. Gaps #10-#12 were found *while
verifying the fixes for the others*, which is the more interesting half of
the story: #10 in particular had been present since the first working build
and every signal available at the time — frame counts, bitrate, file size,
decodability, luma PSNR — said the bridge was working perfectly.

Status column: **fixed** entries have been implemented and verified (see
"Verification of the fixes" below). Only gap #4 is still open, and only
partly: its recoverable halves now heal themselves, but the residue is a
deliberate Android platform behaviour that no unprivileged app can change.

| # | Gap | Severity | Status | Fix that shipped |
|---|---|---|---|---|
| 1 | **4+ concurrent sessions hang.** 3 concurrent passed cleanly; 4 reproducibly hung, jobs hitting a 45 s timeout and emitting truncated streams (59/60 frames, `bytestream -7` errors). Codec2 blocks in `configure()`/`start()` past its instance cap instead of failing. | **High** | ✅ fixed | `BridgeService` now reads `CodecCapabilities.getMaxSupportedInstances()` across the `c2.qti.*` video components, keeps one instance spare, and gates every encode/decode session behind a fair counting semaphore. Overflow waits 5 s for a slot, then is **rejected** with a `busy` error over the existing error channel instead of blocking. `info` mode reports the limit and how many slots are free. |
| 2 | **Client hangs forever on a stalled server.** `bridge_client` had no socket timeout and sat until an external `timeout(1)` killed it. | **High** | ✅ fixed | `SO_RCVTIMEO`/`SO_SNDTIMEO` (default 30 s, `BRIDGE_TIMEOUT` to override) on every connection, a distinct **exit code 3** for "bridge stalled", and an actionable diagnostic. The reader also `shutdown()`s the socket before joining the writer thread, so a timeout can't be re-introduced by the join. |
| 3 | **Transient first-run stall.** The very first sweep hung >120 s at 640x360; the identical command then ran in 0.15 s and never reproduced across ~80 later sessions. Most likely Android throttling the off-screen app. | Medium | ✅ mitigated | Now surfaces as a bounded timeout (fix #2) rather than an indefinite hang, and `bridge_client` **auto-retries once** on timeout (`BRIDGE_RETRIES`). Retry is disabled when either side is `-` (stdin/stdout can't be rewound, so retrying would silently truncate output); `hw-transcode` therefore propagates exit 3 with an explanation instead. |
| 4 | **App must stay open.** Foreground service survives backgrounding but not force-stop/swipe-away; it is not a Linux daemon. | Medium | ⚠️ mitigated | The two recoverable halves are now automatic: a `BootReceiver` restarts the service on `BOOT_COMPLETED`/`QUICKBOOT_POWERON` (so a reboot no longer leaves a dead port) and on `MY_PACKAGE_REPLACED` (so installing a new build doesn't), and the landing screen shows the Doze state with a one-tap battery-optimisation exemption. The notification is `setOngoing` with a content intent, so a backgrounded bridge is one tap from the foreground. **A force-stop still needs a manual launch** — Android deliberately blocks every receiver of a force-stopped package until the user launches it, and there is no way around that without root. |
| 5 | **`bridge.log` unreadable from Debian.** Android 11+ scoped storage denies `/sdcard/Android/data/com.selinuxbridge.app` to every other app and to the PRoot shell. | Low | ✅ fixed | New `mode=3` streams the log file back over the same loopback socket: `bridge_client log`, or `tools/bridge-status --log`. |
| 6 | **Only H.264, and only the codec.** No HEVC/VP9/AV1; no camera or other SELinux-gated hardware despite the app's name. | Low | ✅ fixed (codecs) | Protocol v3 adds a `codec` field: `0=h264 1=hevc 2=vp9 3=av1`, selected with `bridge_client -c hevc …` or `hw-transcode -c hevc`. `info` now enumerates every one of the four and flags which are `[hardware]`. Camera remains future work — it needs a new `mode=`, not a protocol change. |
| 7 | **No `ffmpeg` integration.** AGC-1 ships an `FFCodec`; the bridge did not. | Low | ✅ fixed | `ffmpeg/selinuxbridge.c` registers `h264_selinuxbridge` and `hevc_selinuxbridge` as real libavcodec **encoders and decoders** on the existing `AV_CODEC_ID_H264`/`AV_CODEC_ID_HEVC` ids, so both `-c:v h264_selinuxbridge` (encode) and `-c:v h264_selinuxbridge -i in.mp4` (decode) just work. See "Using it from ffmpeg" below. |
| 8 | **No back-pressure / unbounded buffering.** The decode path read the entire elementary stream into RAM before sending anything. | Low | ✅ fixed | The Annex-B splitter is now streaming: it holds at most one NAL unit plus a read chunk. Memory is O(1) in clip length instead of O(n), and units start flowing immediately instead of after the whole input is read. Framing is byte-identical to the old splitter. |
| 9 | **Throwaway debug signing key.** `build.sh` wrote `build/debug.keystore`, which its own `rm -rf build` then destroyed, so every rebuild changed the app's signing identity and Android refused to update in place. | Low | ✅ fixed | The key moved to `selinux-bridge/keystore/` (gitignored), outside the wipe. An existing `build/debug.keystore` is migrated automatically before the wipe so already-installed copies keep updating. Two consecutive builds now produce the same certificate digest. |
| 10 | **Colour was silently destroyed.** Every encode produced a perfect luma plane and garbage chroma (Y PSNR 38 dB, U/V **6.7 dB**) at every resolution. `BridgeService` requested `COLOR_FormatYUV420Flexible` and then blitted the wire bytes straight into the input buffer — but "flexible" does not mean planar I420. On Qualcomm the chroma comes back **semi-planar**, U and V aliasing one region with a pixel stride of 2, and rows padded to the component's own alignment. Found only because a PSNR check happened to print U and V separately; frame counts, bitrate, decodability and luma quality all looked perfectly healthy. | **High** | ✅ fixed | New `I420.java` copies plane by plane through `getInputImage()`/`getOutputImage()`, honouring `getRowStride()` and `getPixelStride()`, so planar, semi-planar and padded layouts are all correct. The same path repacks decoder output into tightly packed I420. Covered by 10 JVM unit tests (`tools/test-i420`). |
| 11 | **Rate control was inoperative.** Every frame was queued with `presentationTimeUs = 0`, so the encoder believed the whole clip was instantaneous. A 6 Mbps request delivered **2.98 Mbps**. | Medium | ✅ fixed | Frames are now queued at `frameIndex * 1_000_000 / fps` in both directions. |
| 12 | **No tests, and the ones that mattered were untestable.** Everything was verified by hand against a live phone, so nothing could be checked before an install tap, and failure modes (a bridge that stalls, a bridge that is out of slots, a semi-planar chroma layout) could not be reproduced on demand at all. | Medium | ✅ fixed | `tools/selftest` runs 30 checks with no device attached — client round trips, exit codes, fault injection, flat-memory proof, both ffmpeg encoders **and both decoders**, protocol-v4 format records and mid-stream resolution changes, Annex-B parameter sets, timestamps — plus `tools/test-i420`'s 10 layout cases. `tools/mock-bridge.py` reproduces MediaCodec's awkward behaviour deliberately. |
| 13 | **Decode could not be wired into libavcodec.** The protocol never carried the decoded picture size, so a libavcodec decoder had no way to size its frames or to notice a resolution change. Callers had to know the dimensions up front and pass them in. | Medium | ✅ fixed | Protocol v4 adds decode format records (`-2 w h`), taken from the output `Image`'s own crop rectangle so they are the display size rather than the macroblock-padded coded size. `bridge_client decode` no longer takes dimensions at all, and `h264_selinuxbridge`/`hevc_selinuxbridge` now exist as decoders. |
| 14 | **Rate control was inaccurate.** `KEY_BITRATE_MODE` was never set, so Codec2 picked its own default -- VBR on this device's `c2.qti.*.encoder` components, where the requested bitrate is only an average the encoder may exceed freely. An explicit `-b:v` came back **+25% at 2 Mbps, +31% at 6 Mbps and +34% at 12 Mbps**, measured on ordinary content rather than a synthetic worst case. (Distinct from gap #11: that was zero timestamps making the encoder think the clip was instantaneous, which *under*-shot; this is the mode itself.) | Medium | ✅ fixed | Protocol v5 carries a rate-control mode in the **high byte of the codec field**, so the 24-byte header is unchanged and a v3/v4 client -- which sends a bare 0..3 -- lands on the new CBR default automatically. `bridge_client -r cbr\|vbr\|cq`, `hw-transcode -r`, and `ffmpeg -rc_mode cbr\|vbr\|cq`. CQ reinterprets the bitrate field as a quality in 1..100. An unsupported mode falls back to plain `KEY_BIT_RATE` rather than failing the session, and `info` now lists which modes each encoder advertises. |
| 15 | **A stale process was invisible.** Android normally kills an app's process when its package is replaced, so the next start runs the new code. A long-lived foreground service makes surviving that much more likely, and nothing then reloads it: `BootReceiver`'s `MY_PACKAGE_REPLACED` handler calls `startForegroundService()`, but that only delivers another `onStartCommand()` to the **already loaded** classes. The bridge kept serving the old protocol while the user was looking at a successful install, with no symptom at all beyond a version number that never changed. Hit for real on the v4 → v5 update. | Medium | ✅ fixed | `info` now reports the **build timestamp of the code actually answering**, and the service compares the package's `lastUpdateTime` against the value this process saw at startup. `lastUpdateTime` moves only on replacement, so a difference means the package changed *while this process was already running* — exactly the stale case, with no timing heuristic to get wrong. The warning appears in `info`, in `bridge.log`, and as a banner at the top of the app carrying a **Restart now** button, which ends the process so `START_STICKY` restarts the service on the new code — refusing while any transcode is in flight. |

## Verification of the fixes

Measured on this device after the changes. Fault injection uses scripted
stand-in bridges so the failure modes can be reproduced deterministically
rather than waited for; all of it is now checked in as `tools/selftest` and
`tools/test-i420` and runs on every change.

| Fix | Test | Result |
|---|---|---|
| #2 timeout | Server accepts then goes silent, `BRIDGE_TIMEOUT=5 BRIDGE_RETRIES=1` | exit **3** after **10 s** (2 attempts x 5 s) — previously hung indefinitely |
| #2 timeout | Server stalls *mid-stream* after a successful handshake | exit **3** after **5 s**, writer thread unblocked cleanly, no leak |
| #1/#2 busy | Server replies with the `busy` error string | exit **1** immediately, **no** pointless retry (retry is timeout-only) |
| #3 retry | Same stall, `BRIDGE_RETRIES=1` | retry attempted and logged, then a clean failure |
| #8 framing | 4 s / 137-NAL stream through old vs new splitter, hashing every unit | **137 units, byte-identical** sizes and SHA-256s |
| #8 memory | 5.2 MB stream | old **6128 KB** peak RSS → new **2096 KB** |
| #8 memory | 20.7 MB stream (4x the above) | old **21288 KB** (grows with input) → new **2100 KB** (**flat**) — O(n) → O(1) |
| #9 keystore | Two consecutive `./build.sh` runs, compare signer certificate | same digest `7a229262…5d07a1` both times |
| #6 codecs | `bridge_client info` | enumerates h264/hevc/vp9/av1, marking `c2.qti.*` entries `[hardware]` |
| #7 ffmpeg | `ffmpeg -c:v h264_selinuxbridge` → mp4, 120 frames of 1080p on the real device | valid mp4, **120/120 frames**, decodes clean, no timestamp warnings |
| #7 ffmpeg | same for `hevc_selinuxbridge` | valid mp4, 30/30 frames, decodes clean |
| #7 ffmpeg | `-f h264` (no global header) | stream starts with an inline SPS (`00 00 00 01 67`) and decodes standalone |
| #7 ffmpeg | bridge stalls mid-encode, `-bridge_timeout 3` | ffmpeg **fails in 6 s** instead of hanging |
| #10 chroma | 10 JVM unit tests over planar / semi-planar / padded-stride / padded-slice / odd-size / cropped layouts | every case round-trips **byte-exactly** |
| #10 chroma | U and V written to a symmetric planar image | land in their own planes (no swap) |
| #13 v4 | `bridge_client decode` with **no dimensions given** | format record read off the wire, `detected frame size: 192x128`, 24/24 frames, output exactly **884736 B** |
| #13 v4 | Mock forced to change resolution at frame 12 (`MOCK_RESIZE_AT`) | announced as `new frame size: 96x64`; output exactly **552960 B** = 12 full + 12 quarter-area frames |
| #13 v4 | Bridge sends a record type the client has never seen (`-7`) | exit **1** with `protocol error`, *not* a desynchronised stream |
| #13 v4 | Bridge hangs up mid-stream while the client is still writing | exit **1** with the real diagnostic — previously died of **SIGPIPE** (rc 141) before the reader could report anything |
| #7 decode | `ffmpeg -c:v h264_selinuxbridge -i in.mp4` | **24/24 frames**; same for `hevc_selinuxbridge` |
| #7 decode | Bridge output vs the **software** decoder, 320x240x24, SHA-256 of the raw I420 | **byte-identical** (`38c33b71c5143c01…`) — transport, framing, sizing and packing are all lossless |
| #7 decode | Decoded frame timestamps | survive the round trip (min-pts reorder, so B-frames can't permute them) |
| #7 decode | Mid-stream resolution change under libavfilter | decoder reconfigures, transcode completes |
| #7 decode | Bridge stalls, `-bridge_timeout` | fails in **4 s** instead of hanging |
| #4 restart | `aapt2 dump badging` on the shipped APK | `RECEIVE_BOOT_COMPLETED` + `MY_PACKAGE_REPLACED` receiver present, so reboots and in-place updates self-heal |
| #14 rate | `-b:v` at 2 / 6 / 12 Mbps through the **VBR** default, real hardware | **+25% / +31% / +34%** over target — the bug, measured |
| #14 rate | v5 client at its CBR default against a **v4** bridge, real hardware | encodes normally — the high byte is zero, so the header is wire-identical |
| #14 rate | v5 client with `-r vbr` against a **v4** bridge, real hardware | rejected as `unknown codec id 256` — fails loudly rather than silently ignoring the mode |
| #14 rate | `-r cbr/vbr/cq` packing, and that an old client defaults to cbr | mode survives the wire without disturbing the codec id |
| #14 rate | The app's own log, real hardware, before the fix | MediaCodec replied `bitrate-mode=1` — `BITRATE_MODE_VBR`, the device confirming the diagnosis in its own words |
| #15 stale | v4 → v5 in-place update, real device | reproduced: APK on disk reported v5, the running bridge still reported v4, and `info` showed none of the v5 markers |
| #12 tests | `./tools/selftest` with no device attached | **30 passed, 0 failed** |

The server-side halves of #1, #10 and #11 live in the APK, and sideloading
on this device needs a physical install tap that cannot be scripted (`pm
install` from the PRoot shell is denied). They are verified by unit test and
by inspection; the on-device confirmation — `info` reporting `protocol: v4`
and its slot count, a 4-concurrent run returning a clean `busy` instead of
hanging, and chroma PSNR coming back in the 35-45 dB range rather than 6.7 —
is pending that tap on `/sdcard/Download/selinux-bridge.apk`. The same
applies to the server half of protocol v4: the format records are emitted
from `Image.getCropRect()` in `runPump()`, and every client-side consequence
of them is covered above against `tools/mock-bridge.py`, but the crop values
themselves come from a real `MediaCodec` and are verified by inspection. The signing
key is now stable (gap #9), so it installs as an in-place update.

## What this does *not* solve

- This still needs the app to be **installed and not force-stopped**. It is
  not a daemon in the Linux sense. A reboot and an in-place update now
  recover on their own (`BootReceiver`), backgrounding is fine, and the
  landing screen offers a Doze exemption — but a force-stop (swipe away
  from recents, or Settings → Force stop) still requires a manual launch,
  because Android blocks every receiver of a force-stopped package until
  the user opens it. That residue of gap #4 is not fixable without root.
- **Only the video codec is wired up so far.** The app/protocol is named
  and structured to generalize to other SELinux-blocked hardware (camera
  capture being the most obvious next target — same DMA-BUF-style access
  pattern, same "shell denied, real app allowed" root cause), but no
  camera (or other) `mode=` has been implemented yet. Adding one means:
  a new `mode=N` in the handshake, a matching branch in
  `BridgeService.java` using the relevant Android API (`CameraX`/
  `Camera2` for camera), and a corresponding `bridge_client` subcommand —
  the loopback-socket plumbing and per-client threading already in place
  would not need to change.
- **Only H.264 and HEVC reach `ffmpeg`.** VP9 and AV1 are selectable over
  the wire (`bridge_client -c vp9`) but have no libavcodec wrapper, because
  neither has an `mp4toannexb`-style filter to normalise input framing and
  VP9/AV1 hardware encode is not advertised on every device that advertises
  decode. `bridge_client` remains the way to reach them.
