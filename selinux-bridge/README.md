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
`/sdcard/Android/data/com.selinuxbridge.app/files/bridge.log`), readable
directly from Debian:

```
cat /sdcard/Android/data/com.selinuxbridge.app/files/bridge.log
```

and the `info` mode / `tools/bridge-status` gives a live summary without
needing to read the log file at all for the common case.

**Caveat, found while verifying this on-device**: on Android 11+ (this
device is Android 17), `Android/data/<package>/` is scoped-storage-sandboxed
and denies directory listing/reads from *other* apps/shells, including this
one, even under `/sdcard` — `find /sdcard/Android/data/com.selinuxbridge.app`
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

| # | Gap | Evidence | Severity | Potential fix |
|---|---|---|---|---|
| 1 | **4+ concurrent sessions hang** | 3 concurrent passes cleanly; 4 reproducibly hangs, 2–3 jobs hitting a 45 s timeout and emitting truncated streams (59/60 frames, `bytestream -7` errors) | **High** | Qualcomm's Codec2 advertises a `max-concurrent-instances` limit; the service currently ignores it and blocks in `configure()`/`start()`. Fix: read that capability, keep a counting semaphore in `BridgeService`, and *reject* a session over the limit with a clear v2 error string (the protocol already carries one) rather than blocking forever. |
| 2 | **Client hangs forever on a stalled server** | Same test — client had no timeout and sat until the external `timeout(1)` killed it | **High** | Add `SO_RCVTIMEO`/`SO_SNDTIMEO` to `bridge_client.c` (e.g. 30 s) and exit with a diagnostic instead of hanging. Pairs naturally with fix #1. |
| 3 | **Transient first-run stall** | The very first sweep hung >120 s at 640x360, then the identical command ran in 0.15 s and never reproduced across ~80 later sessions | Medium | Most likely Android backgrounding/throttling the APK (it was off-screen). Mitigations: keep the app foregrounded, and implement #2 so a throttled app surfaces as a timeout rather than an indefinite hang. Worth re-testing deliberately with the screen off. |
| 4 | **App must stay open** | Foreground service survives backgrounding but not force-stop/swipe-away; not a Linux daemon | Medium | Inherent to Android. Partial mitigations: `START_STICKY` (already set), battery-optimisation exemption, and a boot-completed receiver to auto-restart. Cannot be made fully headless without root. |
| 5 | **`bridge.log` unreadable from Debian** | `find /sdcard/Android/data/com.selinuxbridge.app` → Permission denied (Android 11+ scoped storage) | Low | Already worked around by the `mode=2` `info` request over the socket. Could be closed fully by adding a `mode` that streams the log back over TCP. |
| 6 | **Only H.264 / only the codec** | No HEVC/AV1 mode; no camera or other SELinux-gated hardware despite the app's name/scope | Low (roadmap) | The hardware exposes `c2.qti.hevc.*`, `c2.qti.vp9.*`, `c2.qti.av1.*` (confirmed in `info` output). Each is a new `mode=N` + a `MediaFormat` MIME change; camera would be a `Camera2`/`CameraX` branch. The socket/threading plumbing needs no change. |
| 7 | **No `ffmpeg` integration** | AGC-1 ships an `FFCodec` (`ffmpeg/`); the bridge does not | Low | Wrap `bridge_client`'s protocol logic as an `agc1`-style `libavcodec` codec, so Shotcut/Blender could target the hardware encoder the same way they target AGC-1 today. |
| 8 | **No back-pressure / unbounded buffering** | `bridge_client`'s writer thread streams as fast as the socket accepts; decode path reads the entire elementary stream into RAM | Low | Not observed to fail (4K/373 MB passed), but a long clip would balloon memory. Fix: bound the writer with a credit/window scheme, and stream-parse NALs instead of slurping the whole file. |
| 9 | **Throwaway debug signing key** | `build.sh` generates `build/debug.keystore` on first run | Low | Fine for sideloading; would need a real, retained keystore before distributing builds anyone else installs (and a stable key is required for in-place updates). |

## What this does *not* solve

- This still requires the app to be **open and on-screen** (a foreground
  service survives backgrounding but not a force-stop); it is not a daemon
  in the Linux sense.
- It does not (yet) wire into `ffmpeg` as an external codec the way AGC-1
  does. If the hardware-codec route is validated as reliable, an
  `agc1`-style `FFCodec` wrapper that shells out to `bridge_client`'s logic
  would be the natural next step.
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
