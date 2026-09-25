# android-bridge: real hardware MediaCodec, reached from Debian/PRoot

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

**`android-bridge/` is that APK — "PRoot GPUCodec Bridge".** It's a minimal
Android app whose only job is to expose real hardware `MediaCodec` (H.264
encode and decode) over a loopback TCP socket, so the Debian/PRoot side can
drive it directly. The name is deliberately literal: if you come back to
this months later having forgotten why it's installed, "PRoot Codec
Bridge" on the home screen (and the in-app subtitle, "Gives your
Termux/PRoot shell hardware access it can't reach alone") should be enough
to remind you.

## What's here

| File | Purpose |
|---|---|
| `AndroidManifest.xml` | One activity, one foreground service. `INTERNET` permission only for the loopback socket. |
| `app/src/main/java/com/gpucodec/bridge/MainActivity.java` | Launcher UI; starts the bridge service in the foreground so Android doesn't kill it. Everything is drawn programmatically (gradients, a small chip-logo icon, a pulsing "alive" status dot, a data-flow diagram) rather than a blank white screen — see screenshot note below. |
| `app/src/main/java/com/gpucodec/bridge/BridgeService.java` | The actual bridge: `ServerSocket` on `127.0.0.1:7878`, wire protocol below, drives `android.media.MediaCodec` synchronously, explicitly preferring hardware (`c2.qti.*`) codec components, one thread per client so a crashed session doesn't take down the accept loop, and logs to a file (see below). |
| `bridge_client.c` | Debian-side test client/CLI: connects over loopback, sends raw frames or a bitstream (from a file or a pipe), prints round-trip stats and the codec name actually used; also supports the `info` diagnostic request. |
| `build.sh` | Rebuilds the signed APK from source using raw SDK command-line tools (`aapt2`, `javac`, `d8`, `apksigner`) — no gradle/network dependency. |
| `tools/bridge-status` | Health-check script: reports whether the app is reachable and prints its `info` diagnostics (device, Android version, which AVC codecs are hardware-backed). |
| `tools/hw-transcode` | Wrapper that pipes an arbitrary ffmpeg-readable input through the bridge's hardware encoder and remuxes to a normal container — the hardware-codec analogue of this repo's `agc-from`/`agc-to`. |

## Building

Requires an Android SDK with build-tools and a `platforms/android-34`
`android.jar`. On this project's dev machine that's a Termux-installed SDK at
`/opt/Android/sdk`; point `ANDROID_SDK_ROOT` elsewhere if yours differs.

```
./build.sh
```

Produces `build/apk_final/gpucodec-bridge.apk` (signed with a throwaway debug
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
sideload normally: the built APK is copied to `/sdcard/Download/gpucodec-bridge.apk`
by `build.sh`'s caller — open it with a file manager and allow install from
this source, or enable *Settings → Developer options → Wireless debugging*,
pair with `adb pair`, then:

```
adb connect <phone-ip>:<port>
adb install -r android-bridge/build/apk_final/gpucodec-bridge.apk
```

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

- **Name**: "PRoot GPUCodec Bridge" (package `com.gpucodec.bridge`,
  unchanged, to avoid an unnecessary rename churn). Chosen to be
  self-explanatory on the home screen/app switcher without needing the
  README open: it names what it's a companion to (**PRoot**, i.e. this
  repo's Termux/PRoot Debian container), which project family it belongs
  to (**GPUCodec**, this repo's name/package), and its role (**Bridge**).
  Plain "GPUCodec Bridge" (the original placeholder name) explained the
  *what* but not the *why it's needed*, hence the "PRoot" prefix.
- **Important clarification**: despite the "GPUCodec" name (kept for
  project-family recognizability, matching this repo's name and the
  app's own package `com.gpucodec.bridge`), this specific app does **not**
  use the GPU. It reaches Qualcomm's dedicated hardware *video codec*
  block (Codec2, `c2.qti.*`) — a separate fixed-function ASIC from the
  Adreno GPU. AGC-1 (the rest of this repo, `agc.c`/`agc_core.h`) is the
  part that actually runs on the GPU via OpenGL compute shaders. The two
  are complementary, not the same mechanism — see the main README's
  architecture overview if that distinction matters for your use case.
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
  logo, or similar) — "PRoot" here is used only in the plain descriptive
  sense of the open-source `proot`/Termux tool this app is a companion to,
  the same way this repo's own README and docs already use that term
  throughout.

## Wire protocol (v2)

All integers are 4-byte big-endian (`DataInputStream`/`DataOutputStream`
network order). One TCP connection = one encode/decode/info session.

1. Client → server, once: `mode(0=encode,1=decode,2=info) width height fps bitrate`
   (`fps`/`bitrate` only matter for encode; send 0 for the others; `info`
   ignores all four but the handshake shape is fixed, so dummy values are
   still sent)
2. Server → client, once: `int32 status` (`0` = OK, nonzero = failed) then
   `int32 len` + `len` UTF-8 bytes — the **name of the hardware/software
   codec component actually selected** on success (e.g. `c2.qti.avc.encoder`),
   or a human-readable error message on failure (e.g. "no encoder for
   video/avc"). This is new in v2; v1 sent only a bare status int with no
   name/message, so v1 and v2 clients/servers are not wire-compatible.
3. For `info` (mode 2): the server then sends one length-prefixed UTF-8 text
   chunk (device model, Android version, and every AVC `MediaCodecInfo` on
   the device with `[hardware]`/`[software]` tags) followed by `-1` EOS —
   no client input is expected or read.
4. For `encode`/`decode` (modes 0/1), then, repeated:
   - Client → server: `int32 length` then `length` bytes (one raw YUV420
     flexible frame for encode, or one H.264 Annex-B unit for decode).
     `length == -1` signals end-of-stream, no bytes follow.
   - Server → client: `int32 length` then `length` bytes of the
     corresponding output (H.264 Annex-B for encode, raw YUV420 flexible
     for decode). `length == -1` on the final reply signals end-of-stream.

### Hardware codec selection

`BridgeService` enumerates `MediaCodecList(REGULAR_CODECS)` and prefers the
first AVC encoder/decoder component whose name starts with `c2.qti.`
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
`/sdcard/Android/data/com.gpucodec.bridge/files/bridge.log`), readable
directly from Debian:

```
cat /sdcard/Android/data/com.gpucodec.bridge/files/bridge.log
```

and the `info` mode / `tools/bridge-status` gives a live summary without
needing to read the log file at all for the common case.

**Caveat, found while verifying this on-device**: on Android 11+ (this
device is Android 17), `Android/data/<package>/` is scoped-storage-sandboxed
and denies directory listing/reads from *other* apps/shells, including this
one, even under `/sdcard` — `find /sdcard/Android/data/com.gpucodec.bridge`
returns "Permission denied" from the Debian side. So `bridge.log` exists
and is useful if you have a way to read it (a root shell, `adb shell run-as`,
or the device's own Files app with "show system files"), but it is **not**
actually readable from the Termux/PRoot shell as originally assumed. The
`info` mode over the socket (verified working, see below) is the reliable
adb-free diagnostic path in practice.

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

Installed via sideload (`/sdcard/Download/gpucodec-bridge.apk`, after fixing
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
reference decoders return tightly-packed frames). Fixed one real client-side
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
   `android-bridge` APK can; it would need to actually be merged and shipped
   in an official release before anyone could use it.
2. **New-API PRs there don't appear to land quickly.** Checking the repo's
   recent closed PRs (as of this writing): a "feat: add calendar" addition
   sat open for about three weeks and was closed unmerged; a USB
   vendor/product-ID feature PR was closed unmerged too. No issue or PR has
   ever mentioned MediaCodec or hardware video.

So for anyone else in this situation, `android-bridge/` here is the more
useful thing to fork: it's standalone, buildable and sideloadable with
`build.sh` alone, and doesn't depend on anyone else's release cadence or
signing key.

## What this does *not* solve

- This still requires the app to be **open and on-screen** (a foreground
  service survives backgrounding but not a force-stop); it is not a daemon
  in the Linux sense.
- 4K NV12 frames are ~12.4 MB each — do not send raw frames over the socket
  at high framerate without back-pressure; this is a validation harness,
  not a production streaming pipeline.
- It does not (yet) wire into `ffmpeg` as an external codec the way AGC-1
  does. If the hardware-codec route is validated as reliable, an
  `agc1`-style `FFCodec` wrapper that shells out to `bridge_client`'s logic
  would be the natural next step.
