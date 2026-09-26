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
| `app/src/main/java/com/selinuxbridge/app/CameraSource.java` | Camera2 capture for `mode=4`, repacked to tightly packed I420 through `I420.java`. Enumerates back camera first, picks the nearest supported size to the request (and announces what it got), and holds a bounded queue that **drops the oldest** frame under pressure — for a live camera, latency matters and a dropped frame does not. |
| `app/src/main/java/com/selinuxbridge/app/MicSource.java` | `AudioRecord` capture for `mode=5`, emitting little-endian S16 in ~100 ms units, with a buffer 4× the reported minimum so a scheduling hiccup doesn't overrun it. |
| `bridge_client.c` | Debian-side client/CLI: connects over loopback, sends raw frames or a bitstream (from a file or a pipe), prints round-trip stats and the codec name actually used. Supports `encode`/`decode`/`info`/`log`/`camera`/`mic`, h264/hevc/vp9/av1 via `-c`, bounded socket timeouts with a distinct exit code, a streaming Annex-B splitter with O(1) memory, and v4 format records so `decode` needs no dimensions. |
| `build.sh` | Rebuilds the signed APK from source using raw SDK command-line tools (`aapt2`, `javac`, `d8`, `apksigner`) — no gradle/network dependency. Signs with a persistent key in `keystore/` so rebuilds install as in-place updates. |
| `keystore/` | Local debug signing key, gitignored. Kept outside `build/` so `build.sh`'s clean step cannot destroy it (see gap #9). |
| `../ffmpeg/selinuxbridge.c` | libavcodec wrapper registering `h264_selinuxbridge` / `hevc_selinuxbridge` as real ffmpeg **encoders and decoders**. |
| `../ffmpeg/selinuxbridge-ffmpeg.patch` | The three-file registration patch (`allcodecs.c`, `Makefile`, `configure`) that ffmpeg's build needs. |
| `tools/bridge-status` | Health-check script: reports whether the app is reachable and prints its `info` diagnostics (device, Android version, all four video codecs and which are hardware-backed, concurrency limit). `--log` also dumps the app's log. |
| `tools/hw-transcode` | Wrapper that pipes an arbitrary ffmpeg-readable input through the bridge's hardware encoder and remuxes to a normal container — the hardware-codec analogue of this repo's `agc-from`/`agc-to`. `-c hevc` selects HEVC; `-D` also decodes on the hardware block, making the transcode hardware end to end. |
| `tools/selftest` | Full regression suite. Needs no phone and no installed APK: it runs everything against the mock bridge, including fault injection that cannot be arranged reliably on real hardware. |
| `native/v4l2-shim.c` | Puts the phone camera on **`/dev/video0`** as a real V4L2 capture node, by interposing the dozen libc calls a V4L2 client actually makes. No kernel module, no `v4l2loopback`, no root: an application never talks to a driver, it talks to libc. Stock `ffmpeg`, VLC, Chromium and anything else that opens the node work unmodified and with no flags. |
| `native/pci-shim.c` | Answers the one unreadable `/proc/bus/pci/devices` that `libpci` insists on, without which **every** Chromium- or Electron-based application aborts during start-up. |
| `native/netlink-shim.c` | Hands out a working substitute for the `NETLINK_KOBJECT_UEVENT` socket this container may not open. `libudev` gives up when that socket fails and reports an empty device list, which is why Chromium saw no camera *and* no microphone. |
| `native/sysfs-shim.c` | Overlays a readable `/sys/class/video4linux` entry for the camera. Enumeration reads sysfs, never `/dev`, so without this the device list is empty however well `/dev/video0` behaves. The overlay is additive — a path is redirected only where the synthetic tree has an entry — because glibc itself reads `/sys/devices/system/cpu/online`. |
| `native/hw-enable` | One entry point that installs, removes, reports and **proves** all of the above: `install`, `uninstall`, `status`, `doctor`. `doctor` captures real frames, resolves the camera through `libudev`, records real audio, starts a real Chromium and checks the codecs, rather than reporting that files exist. |
| `native/run-tests` + `native/tests/` | Fast regression suite for the shim. Every bug that mattered was only reproducible through a browser, and a Chromium run costs two to four minutes here and can be killed by memory pressure half way through; these reproduce the same failures in seconds. Covers stock `ffmpeg`, concurrent handles, acquire/release/re-acquire with a blocked `DQBUF` racing teardown, the `libv4l2` raw-syscall path via VLC, and `libudev` enumeration. |
| `native/hw-session` | Keeps the microphone published for the life of the desktop session, restarting the pipeline with backoff when the phone link drops and reaping clients left over from earlier runs. |
| `tools/bridge-webcam` | The earlier, LD_PRELOAD-free route: publishes the phone camera as a **FIFO** carrying Y4M, which native applications (`ffplay`, `ffmpeg`, VLC, OBS…) read like any other source. Superseded by `native/v4l2-shim.c` for general use, and kept because a pipe is still the simplest thing to hand to a one-off `ffmpeg`. A FIFO makes the camera on-demand for free: opening one for write blocks until a reader attaches, so the camera is not opened — and its indicator not lit — until something actually wants frames, and it is released the moment the reader goes away. |
| `tools/bridge-mic` | Publishes the phone microphone as a **real PulseAudio device** (`phone_mic`, backed by a null sink whose monitor is republished through `module-remap-source` — a great many applications, Chromium and everything built on it included, refuse to list monitor sources at all, so the remap is what makes the microphone visible where calls are actually made) using a null sink fed by `pacat`, and makes it the default. No kernel module and no root: PulseAudio is a userspace daemon, so a virtual microphone is something an unprivileged user can simply create. Every audio application, browsers included, then finds it the ordinary way. It deliberately does **not** use `module-pipe-source`, which has no flow control and inflates the audio by 20–45%; see "Why a null sink" for the measurements. |
| `tools/bridge-browser` | Predates the shims: launches Chromium wired to the bridged microphone and, optionally, a short recording from the bridged camera. Superseded by `native/hw-enable install`, after which an ordinary Chromium gets the live camera *and* the live microphone together — see gap #17. |
| `tools/bridge-demo` + `bridge-demo.html` | An interactive page for seeing the browser situation first-hand rather than taking it on faith. It starts the camera into a growing file, then serves a page showing the browser's picture next to a signature of its pixels, the live frame count of the file on disk, a microphone meter and a ticking clock. Every frame carries the time it was captured, burned in, so the delay is readable against the page's own clock. **Record 10 s** captures a clip through `MediaRecorder` for offline analysis. |
| `tools/mock-bridge.py` | A protocol-v6 bridge that reproduces the awkward parts of MediaCodec — codec-config packets, one packet per access unit, lookahead that swallows frames before emitting anything, decode format records — backed by a real x264/x265 subprocess so the output is a genuine stream. It also fakes capture, including the HAL's habit of rounding a requested camera size to one it actually offers, and can be told to refuse a permission (`MOCK_DENY_CAMERA`, `MOCK_DENY_MIC`). `MOCK_RESIZE_AT=N` forces a mid-stream resolution change, which is impractical to provoke on real hardware. |
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

### Making the hardware native to the whole container

With the app running, one command wires the phone's hardware into the
standard Linux interfaces, so that applications already installed — and any
installed later — find it without knowing this project exists:

```sh
selinux-bridge/native/hw-enable install   # camera, mic, GPU, codecs
selinux-bridge/native/hw-enable doctor    # prove each path end to end
```

After that, in any new shell:

| You want | You run | What it uses |
|---|---|---|
| The camera | `ffmpeg -f v4l2 -i /dev/video0 …`, `vlc v4l2:///dev/video0`, `gst-launch-1.0 v4l2src`, `getUserMedia` in a browser | `/dev/video0` |
| The microphone | anything that records | the default PulseAudio source, `phone_mic` |
| The GPU | anything OpenGL; Chromium and Electron apps just start | Turnip + Zink; `pci-shim` for browsers |
| Hardware codecs | `ffmpeg -c:v h264_selinuxbridge …` | `ffmpeg` on `PATH` |

Applications started from the XFCE menu or a panel need the same treatment
as ones started from a terminal, and that turns out to be a trap worth
writing down. **`startxfce4` never reads `~/.xprofile` on this system.** It
execs `$XDG_CONFIG_HOME/xfce4/xinitrc` if that exists and
`/etc/xdg/xfce4/xinitrc` otherwise — so a `.xprofile` hook *looks* installed
and silently does nothing, and the failure it produces is the confusing kind:
the camera works in a terminal and is missing from the same application
launched from the menu. `hw-enable install` therefore writes both, generating
`~/.config/xfce4/xinitrc` (or inserting a marked block into an existing one)
so the whole session inherits the environment. Because a session that cannot
start is a far worse outcome than one without a camera, that wrapper does not
assume the system file is executable, and `hw-enable status` reports whether
the *currently running* session has the shims — a fresh install does not
reach a desktop that was already up, which needs a session restart.

`hw-enable uninstall` puts the machine back exactly as it was: a generated
`xinitrc` is removed outright, while one that already existed has only its
marked block cut out and is restored byte for byte. Every shim
can also be switched off on its own without rebuilding, which is the first
thing to try if some application misbehaves:
`BRIDGE_V4L2_DISABLE=1`, `BRIDGE_PCI_DISABLE=1`, `BRIDGE_NETLINK_DISABLE=1`,
`BRIDGE_SYSFS_DISABLE=1`. `BRIDGE_V4L2_DEBUG=1` (or `2`) traces the camera
path.

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

## Wire protocol (v6)

All integers are 4-byte big-endian (`DataInputStream`/`DataOutputStream`
network order). One TCP connection = one session.

1. Client → server, once:
   `mode width height fps bitrate codec`

   | field | meaning |
   |---|---|
   | `mode` | `0` encode, `1` decode, `2` info, `3` log, `4` camera, `5` microphone — **4 and 5 new in v6** |
   | `width`/`height` | frame size; decode may send `0 0` and learn the real size from the format record below. For camera it is the *requested* size, which the HAL may round — the format record reports what was actually opened. For microphone it carries `sample_rate` and `channels` instead. |
   | `fps`/`bitrate` | encode only; send 0 otherwise. Camera uses `fps` for the frame rate and `bitrate` for a frame budget (`0` = until the client disconnects); microphone uses `bitrate` for a duration in milliseconds. |
   | `codec` | bits 0–7: `0` h264, `1` hevc, `2` vp9, `3` av1 — **new in v3**.<br>bits 8–15: rate control, encode only — `0` CBR, `1` VBR, `2` CQ — **new in v5**. A v3/v4 client sends a bare `0..3`, so its high bits are zero and it gets CBR, which is the fix for gap #14. In CQ the `bitrate` field carries a quality in 1..100 instead of bits/s.<br>For camera it is the camera index; for microphone the audio source. |

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

6. **Capture — new in v6 (modes 4 and 5).** These are one-way: the server
   sends, the client only reads, and there is no per-unit request. The
   stream opens with the same `-2 a b` format record used by decode, and it
   is mandatory rather than advisory, because in both cases the client
   asked for something the hardware is entitled to refuse:

   - **Camera (mode 4)** — `-2 width height` is the size the HAL actually
     opened, which is frequently not the size requested; Camera2 offers a
     fixed menu of stream configurations and picks the nearest. Every unit
     after the record is one tightly packed I420 frame at that size, via
     the same `I420.java` path the codec uses, so padded row strides and
     semi-planar chroma are already dealt with.
   - **Microphone (mode 5)** — `-2 sample_rate channels`, followed by
     little-endian signed 16-bit PCM in roughly 100 ms units.

   `-1` ends the stream: the frame budget ran out, the duration elapsed, or
   the capture stopped. The client hanging up is the normal way to stop an
   open-ended session, and the server treats it as such rather than as an
   error.

   Both modes require an Android runtime permission the rest of the bridge
   does not. If it is missing the session is **refused** at the handshake
   with a message naming the permission and how to grant it, rather than
   accepted and left to produce a silent or black stream — the failure mode
   that made gap #10 invisible for so long.

### Version history

| version | change |
|---|---|
| v1 | bare `int32` status, no codec name or error message |
| v2 | status gained a length-prefixed codec-name/error-message field; added `info` (mode 2) |
| v3 | added the `codec` handshake field (hevc/vp9/av1) and `log` (mode 3) |
| v4 | added decode format records (`-2 w h`), so decode dimensions are optional and a mid-stream resolution change is a normal event |
| v5 | added rate control in the codec field's high byte (CBR/VBR/CQ), fixing a 25–34% bitrate overshoot. The header is unchanged, so this is the one version bump that *is* backward compatible |
| v6 | added capture: `mode=4` camera and `mode=5` microphone, reusing the v4 format record to announce the size the HAL actually opened (camera) or the rate and channel count (microphone). The header is unchanged again, and no existing mode behaves differently, so a v5 client talks to a v6 bridge unmodified |

Versions are **not** wire-compatible with each other. A v3 client talking to
a v2 server happens to work for `info` (the extra `codec` int is simply never
read before the server replies), and a v4 client talking to a v3 server works
for everything except decode (a v3 server simply never sends a format record,
so the client has no size to report). v5 and v6 are the deliberate
exceptions: v5 reuses spare bits of an existing field rather than adding
one, and v6 only adds new `mode=` values, so in both cases a newer
client at its default talks to an older server unchanged, and asking an
older server for something it does not have fails loudly (`unknown codec
id 256`, or a rejected mode) instead of being silently ignored — both
confirmed on real hardware.
Otherwise encode/decode will mis-frame — update both sides together. `bridge_client info` prints the
server's protocol version, which is the quickest way to spot a mismatch.

### Stale process after an update

Installing a new build does **not** reliably restart the service. Android
usually kills the process on replacement, but a foreground service can
survive it, and `startForegroundService()` then only delivers another
`onStartCommand()` to the classes already loaded — it never reloads code.
The bridge keeps answering, on the old protocol, with nothing to show for
it but a version number that does not change.

The app's footer carries the same two facts, so the version can be read
off the screen without a shell:

```
protocol v6  ·  build 2026-09-25 11:34:41
github.com/skyv04/selinux-hardware-bridge
```

`bridge_client info` reports both the protocol version and the **build
timestamp of the code actually answering** in the identical format, so
the two can be compared directly, and flags the mismatch outright:

```
protocol: v6
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

## Camera and microphone (v6)

Both need a runtime permission the codec modes do not. Grant them once from
the app's landing screen — there is a capture card that asks for exactly
these two and nothing else — then:

```
tools/bridge-webcam start            # phone camera -> a FIFO carrying Y4M
tools/bridge-webcam -s 1280x720 -f 30 start
tools/bridge-webcam status           # idle, or streaming and to whom
tools/bridge-webcam stop

tools/bridge-mic start               # phone mic -> a PulseAudio source
tools/bridge-mic status
tools/bridge-mic stop
```

The camera is a FIFO on purpose. Opening a FIFO for write blocks until a
reader attaches, so the camera is not opened — and the phone's camera
indicator not lit — until something actually asks for frames, and it is
released again the moment the reader goes away. Any native application can
consume it:

```
ffplay -f yuv4mpegpipe -i "$(tools/bridge-webcam path)"
ffmpeg -f yuv4mpegpipe -i "$(tools/bridge-webcam path)" -t 10 clip.mp4
```

### The picture comes out upright

A phone's camera sensor is mounted to suit the industrial design, not the
screen, so the frames the HAL hands over are on their side — on this device
the back sensors read 90° and the front ones 270°, which is typical. The
bridge applies that angle for you, so a capture is upright without anyone
downstream having to know. `info` prints each sensor's angle, and a quarter
turn swaps the *announced* dimensions, which is why reading the format
record rather than assuming your request was honoured is not optional:

```
camera 0 [hal id 0, back,  sensor 90deg]  max 4080x3060, 24 sizes
camera 2 [hal id 1, front, sensor 270deg] max 3648x2736, 21 sizes
```

```
./bridge_client camera -i 2 640 480 15 2 out.y4m   # -> 480x640, upright
./bridge_client camera -i 2 --rotate none  ...     # exactly what the sensor saw
./bridge_client camera -i 2 --rotate 180   ...     # or pick the angle yourself
```

The size you ask for still selects the **sensor** mode, so a quarter turn
returns that many pixels with the dimensions swapped rather than quietly
picking a worse mode to hit the number you typed. Rotation is a blocked
transpose rather than the obvious nested loop, because the obvious one
misses the cache on every write; measured at 1280x720 it costs nothing
worth reporting — **24.8 fps rotated against 25.0 fps raw**, byte counts
identical.

Or straight from the client, without the supervisor:

```
./bridge_client camera -i 0 1280 720 30 0 -   # Y4M on stdout, 0 = no limit
./bridge_client mic 48000 1 0 - > mic.s16     # raw S16LE
```

The microphone becomes a real PulseAudio device. Internally it is a null
sink called `bridge_mic`, but what applications record from is
**`phone_mic`**, its monitor republished through `module-remap-source`, and
`bridge-mic` makes that the default source.

Both of those details were learned the hard way. The default matters because
`getUserMedia({audio:true})` takes the *default* source, and the default here
is otherwise a silent monitor — a working virtual microphone that nothing is
routed to is indistinguishable from a broken one. The remap matters because a
great many applications refuse to list monitor sources at all: Chromium and
everything built on it, Zoom, Discord, OBS. A monitor is normally loopback of
the speakers, and offering it as a microphone causes feedback, so hiding it is
correct behaviour — it just meant the microphone worked everywhere except in
the programs people actually make calls with. `module-remap-source`
republishes the identical stream as an ordinary input device, which those
filters accept. If the remap ever fails, `bridge-mic` falls back to
`bridge_mic.monitor` and says so.

For a call, consider `bridge-mic -s voice`, which selects
`VOICE_COMMUNICATION` and so gets the platform echo canceller and noise
suppressor. Be aware that it will look *much* quieter on an idle room —
measured here at **−72.7 dBFS** against `mic`'s −34.0 — because
suppressing steady ambient noise is exactly what it is for. That is the
feature working, not a broken source; speech still comes through.

#### Why a null sink and not `module-pipe-source`

`module-pipe-source` is the obvious way to turn a FIFO into a microphone,
and it silently destroys audio/video sync. It has no flow control: when the
pipe is empty at poll time it manufactures silence to keep the source
running, then still emits the real samples when they arrive. The source
therefore produces **more** audio than went into it, and the excess
accumulates for as long as the call lasts.

The phone was never at fault. Measured on this device:

| Path | Audio produced | Wall clock | Ratio |
|---|---|---|---|
| Bridge → file, continuous | 14.90 s | 15.02 s | **0.992×** |
| `module-pipe-source` → `parec` | 24.10 s | 20.02 s | **1.204×** |
| `module-pipe-source` → `ffmpeg` | 44.30 s | 30.64 s | **1.445×** |
| **null sink + `pacat` → `parec`** | **19.95 s** | **20.03 s** | **0.996×** |

So `bridge-mic` now loads `module-null-sink` and feeds it with `pacat`.
`pacat` is an ordinary PulseAudio playback client, so the server
rate-matches it the way it does any other stream — an underrun becomes
silence *in place of* real audio rather than *in addition to* it. End to
end, a 25 s camera-plus-microphone capture drifted **0.128 s**, against
**14.3 s** over 30 s through the pipe source.

Two things fall out of the change. The FIFO becomes container-local,
because only `pacat` reads it and `pacat` runs on this side — the old
arrangement needed a path visible identically to both this container and
Termux, and failed with nothing but `Module initialization failed` when it
was not. And `device.description` must contain no spaces or parentheses:
`pactl` joins module arguments into one string and re-splits on
whitespace, so a quoted value with a space fails the same opaque way.

### Browsers

Run `native/hw-enable install` once and an ordinary browser has the phone's
camera and microphone together, with no switches, no wrapper and no
launcher. `getUserMedia({video: true, audio: true})` returns
`Phone Camera (SELinux Bridge)` and `phone_mic`, both live, and the camera
can be released and re-acquired as many times as a call needs.

This used to be the project's flagship limitation (gap #17), and it is worth
saying plainly why it stood for so long: the reasoning was sound but the
premise was wrong. The premise was that a browser could only be given a
*fake* camera, because a real one needs a device node, which needs
`v4l2loopback`, which needs `CAP_SYS_MODULE`. Every conclusion after that
followed correctly and every one of them was useless, because an application
never talks to a kernel module. It talks to libc. Once the device is
answered for at that level, the browser has nothing to work around.

`tools/bridge-demo` and `tools/bridge-browser` predate all this and still
work; they are no longer the recommended route.

### Interposing libc: what actually goes wrong

The idea — answer the calls a V4L2 client makes — is a paragraph. Making it
survive contact with real applications took twelve distinct bugs, and they are
recorded here because every one of them presented as something other than
what it was.

| # | Symptom | Cause |
|---|---------|-------|
| 1 | Segfault before `main()` | `dlsym` calls `malloc`, `malloc` calls `mmap`, and `mmap` is interposed — so it ran with `real_mmap` still NULL. Every entry point needs a raw-`syscall()` fallback and an `inited` flag. |
| 2 | Camera helper never started, no error anywhere | Between `fork()` and `exec()` only async-signal-safe calls are legal, and `execl` allocates. `posix_spawn` instead. |
| 3 | `EACCES` opening a device that was clearly there | Anything built with `_FILE_OFFSET_BITS=64` — which is most things, ffmpeg included — calls `mmap64`/`stat64`, not the short names. Interposing only the short ones leaves those callers on the real syscall, and a permissions error is what that looks like. |
| 4 | Full-rate capture that still produced a 3-frame file | `VIDIOC_DQBUF` returned buffers with no `timestamp`, so ffmpeg deduplicated them. |
| 5 | Chromium opened the device, asked one question, gave up | V4L2 request codes have the top bit set (`VIDIOC_QUERYCAP` is `0x80685600`). A caller holding one in a signed `int` — Chromium does — passes `0xFFFFFFFF80685600`. `_IOC_NR()` masks the rest away, so it *decodes* correctly while matching no `case` label. Truncating to 32 bits is not a workaround; it is what the kernel does. |
| 6 | `posix_spawn` reported "Exec format error" for a valid ELF | `getenv()` returns a pointer *into* the environment block, and Chromium rewrites that block in place after the constructor runs. The saved path had become `""`. Copy the string; also compile the default in. |
| 7 | Microphone worked everywhere except in the apps people call with | Chromium, Zoom, Discord and OBS all hide monitor sources, since a monitor is normally speaker loopback. `module-remap-source` republishes the same stream as an ordinary input. |
| 8 | Chromium reported `NotFoundError` and dropped the camera | Returning `EBUSY` to a *second* `open()` looks honest and is wrong: the kernel allows many opens and makes only streaming exclusive, and Chromium probes capabilities while other tabs capture. |
| 9 | A live capture was torn down by an unrelated `close(-1)` | Dropping an `fd >= 0` guard makes `close(-1)` compare equal to `C.fd == -1`. |
| 10 | Heap corruption, then total silence | `stop_stream()` was gated on `C.streaming` alone, so a second caller returned early while the pump thread was still alive — and the next line freed the buffers it was still `memcpy()`ing into. Gate on the pump, not the flag, and make the second caller *wait* rather than join a thread that is already being joined. |
| 11 | GStreamer set capture up perfectly, then failed at `STREAMON` with `EACCES` | A `dup()` was not interposed, so the copy was the bare pipe underneath. `v4l2src` allocates buffers on the original fd and streams on a duplicate, so everything up to the last step worked. Unlike a second `open()`, a `dup` shares one file description and must therefore share the capture — the opposite of the rule in #8. |

The twelfth is the one worth reading twice, because no amount of care
inside the shim would have prevented it.

**Chromium's camera could be acquired once and never again.** The second
`getUserMedia({video:true})` in a page hung forever. Tracing showed the shim
going completely silent inside `VIDIOC_REQBUFS`, with the mutex recorded as
*free*. Chromium's own `--vmodule` logs showed `GetDeviceInfosAsync` never
returning and `VideoCaptureThr` parked in a futex. Rebuilding the previous
version of the shim as a control cleared the obvious suspects; running
Chromium's built-in fake device proved the fault was ours.

It was `free()`. Chromium replaces the global allocator with PartitionAlloc,
and PartitionAlloc returns memory through the **public** `munmap` — which is
us. So `free()`, called from `REQBUFS` while we held our own lock, re-entered
our own `munmap` wrapper, which tried to take that lock again, and a
non-recursive mutex did exactly what it should. glibc's malloc calls a hidden
`__munmap` alias that cannot be interposed, which is precisely why `ffmpeg`
never showed this and Chromium always did.

The fix is not a special case for `munmap`. It is the rule that removes the
whole class: **while a thread is already inside the shim, it is not an
application making a call, so every entry point behaves as a plain
passthrough.** One thread-local depth counter, checked at each entry point.

Two general lessons, both of which cost real time here:

- *An interposing library cannot assume the code beneath it stays out of the
  symbols it interposes.* Allocators, the dynamic linker and the C library
  all call the things you replaced.
- *Make the harness distinguish "failed" from "hung" before debugging
  anything asynchronous.* The browser tests only became useful once the page
  reported each step to a local HTTP server as it happened; `--dump-dom` and
  `--virtual-time-budget` both race `getUserMedia` and report a plausible
  lie.

## Testing without a device

Most of the bridge can be exercised with no phone attached, no APK installed
and no hardware codec, which matters for two reasons: sideloading here needs
a physical install tap, so nothing could otherwise be checked before that
happens; and the interesting failure modes — a bridge that accepts a
connection and then goes quiet, a bridge that is out of codec slots, a
semi-planar chroma layout — cannot be arranged reliably on real hardware.

```sh
./tools/selftest          # 47 checks, ~3 min
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
"Verification of the fixes" below). One gap remains open, and it is not open
for want of effort: #4's recoverable halves now heal themselves, but its
residue is a deliberate Android platform behaviour that no unprivileged app
can change.

Gap #17 was the interesting one. It stood as "partly fixed — irreducible
Chromium behaviour" for a long time, and that verdict was wrong. It was
reasoned from the wrong premise: that the only way to give a browser a
camera was Chromium's fake-device switch, because a real `/dev/video0`
needed a kernel module we could not load. What that premise misses is that
an application never talks to a kernel module — it talks to libc. Gaps
#20–#24 are what fell out of taking that seriously, and between them they
close #17 completely: an ordinary Chromium now gets the live camera and the
live microphone at the same time, with no switches at all.

| # | Gap | Severity | Status | Fix that shipped |
|---|---|---|---|---|
| 1 | **4+ concurrent sessions hang.** 3 concurrent passed cleanly; 4 reproducibly hung, jobs hitting a 45 s timeout and emitting truncated streams (59/60 frames, `bytestream -7` errors). Codec2 blocks in `configure()`/`start()` past its instance cap instead of failing. | **High** | ✅ fixed | `BridgeService` now reads `CodecCapabilities.getMaxSupportedInstances()` across the `c2.qti.*` video components, keeps one instance spare, and gates every encode/decode session behind a fair counting semaphore. Overflow waits 5 s for a slot, then is **rejected** with a `busy` error over the existing error channel instead of blocking. `info` mode reports the limit and how many slots are free. |
| 2 | **Client hangs forever on a stalled server.** `bridge_client` had no socket timeout and sat until an external `timeout(1)` killed it. | **High** | ✅ fixed | `SO_RCVTIMEO`/`SO_SNDTIMEO` (default 30 s, `BRIDGE_TIMEOUT` to override) on every connection, a distinct **exit code 3** for "bridge stalled", and an actionable diagnostic. The reader also `shutdown()`s the socket before joining the writer thread, so a timeout can't be re-introduced by the join. |
| 3 | **Transient first-run stall.** The very first sweep hung >120 s at 640x360; the identical command then ran in 0.15 s and never reproduced across ~80 later sessions. Most likely Android throttling the off-screen app. | Medium | ✅ mitigated | Now surfaces as a bounded timeout (fix #2) rather than an indefinite hang, and `bridge_client` **auto-retries once** on timeout (`BRIDGE_RETRIES`). Retry is disabled when either side is `-` (stdin/stdout can't be rewound, so retrying would silently truncate output); `hw-transcode` therefore propagates exit 3 with an explanation instead. |
| 4 | **App must stay open.** Foreground service survives backgrounding but not force-stop/swipe-away; it is not a Linux daemon. | Medium | ⚠️ mitigated | The two recoverable halves are now automatic: a `BootReceiver` restarts the service on `BOOT_COMPLETED`/`QUICKBOOT_POWERON` (so a reboot no longer leaves a dead port) and on `MY_PACKAGE_REPLACED` (so installing a new build doesn't), and the landing screen shows the Doze state with a one-tap battery-optimisation exemption. The notification is `setOngoing` with a content intent, so a backgrounded bridge is one tap from the foreground. **A force-stop still needs a manual launch** — Android deliberately blocks every receiver of a force-stopped package until the user launches it, and there is no way around that without root. |
| 5 | **`bridge.log` unreadable from Debian.** Android 11+ scoped storage denies `/sdcard/Android/data/com.selinuxbridge.app` to every other app and to the PRoot shell. | Low | ✅ fixed | New `mode=3` streams the log file back over the same loopback socket: `bridge_client log`, or `tools/bridge-status --log`. |
| 6 | **Only H.264, and only the codec.** No HEVC/VP9/AV1; no camera or other SELinux-gated hardware despite the app's name. | Low | ✅ fixed | Protocol v3 adds a `codec` field: `0=h264 1=hevc 2=vp9 3=av1`, selected with `bridge_client -c hevc …` or `hw-transcode -c hevc`. `info` now enumerates every one of the four and flags which are `[hardware]`. **Camera and microphone shipped in v6** as `mode=4`/`mode=5` — no protocol change was needed, exactly as predicted here — with `tools/bridge-webcam` and `tools/bridge-mic` on the Debian side. |
| 7 | **No `ffmpeg` integration.** AGC-1 ships an `FFCodec`; the bridge did not. | Low | ✅ fixed | `ffmpeg/selinuxbridge.c` registers `h264_selinuxbridge` and `hevc_selinuxbridge` as real libavcodec **encoders and decoders** on the existing `AV_CODEC_ID_H264`/`AV_CODEC_ID_HEVC` ids, so both `-c:v h264_selinuxbridge` (encode) and `-c:v h264_selinuxbridge -i in.mp4` (decode) just work. See "Using it from ffmpeg" below. |
| 8 | **No back-pressure / unbounded buffering.** The decode path read the entire elementary stream into RAM before sending anything. | Low | ✅ fixed | The Annex-B splitter is now streaming: it holds at most one NAL unit plus a read chunk. Memory is O(1) in clip length instead of O(n), and units start flowing immediately instead of after the whole input is read. Framing is byte-identical to the old splitter. |
| 9 | **Throwaway debug signing key.** `build.sh` wrote `build/debug.keystore`, which its own `rm -rf build` then destroyed, so every rebuild changed the app's signing identity and Android refused to update in place. | Low | ✅ fixed | The key moved to `selinux-bridge/keystore/` (gitignored), outside the wipe. An existing `build/debug.keystore` is migrated automatically before the wipe so already-installed copies keep updating. Two consecutive builds now produce the same certificate digest. |
| 10 | **Colour was silently destroyed.** Every encode produced a perfect luma plane and garbage chroma (Y PSNR 38 dB, U/V **6.7 dB**) at every resolution. `BridgeService` requested `COLOR_FormatYUV420Flexible` and then blitted the wire bytes straight into the input buffer — but "flexible" does not mean planar I420. On Qualcomm the chroma comes back **semi-planar**, U and V aliasing one region with a pixel stride of 2, and rows padded to the component's own alignment. Found only because a PSNR check happened to print U and V separately; frame counts, bitrate, decodability and luma quality all looked perfectly healthy. | **High** | ✅ fixed | New `I420.java` copies plane by plane through `getInputImage()`/`getOutputImage()`, honouring `getRowStride()` and `getPixelStride()`, so planar, semi-planar and padded layouts are all correct. The same path repacks decoder output into tightly packed I420. Covered by 10 JVM unit tests (`tools/test-i420`, which now runs 18 with the rotation cases from gap #19). |
| 11 | **Rate control was inoperative.** Every frame was queued with `presentationTimeUs = 0`, so the encoder believed the whole clip was instantaneous. A 6 Mbps request delivered **2.98 Mbps**. | Medium | ✅ fixed | Frames are now queued at `frameIndex * 1_000_000 / fps` in both directions. |
| 12 | **No tests, and the ones that mattered were untestable.** Everything was verified by hand against a live phone, so nothing could be checked before an install tap, and failure modes (a bridge that stalls, a bridge that is out of slots, a semi-planar chroma layout) could not be reproduced on demand at all. | Medium | ✅ fixed | `tools/selftest` runs 47 checks with no device attached — client round trips, exit codes, fault injection, flat-memory proof, both ffmpeg encoders **and both decoders**, protocol-v4 format records and mid-stream resolution changes, Annex-B parameter sets, timestamps, and the v6 capture modes including refused permissions and HAL size rounding — plus `tools/test-i420`'s 18 layout and rotation cases. `tools/mock-bridge.py` reproduces MediaCodec's awkward behaviour deliberately. |
| 13 | **Decode could not be wired into libavcodec.** The protocol never carried the decoded picture size, so a libavcodec decoder had no way to size its frames or to notice a resolution change. Callers had to know the dimensions up front and pass them in. | Medium | ✅ fixed | Protocol v4 adds decode format records (`-2 w h`), taken from the output `Image`'s own crop rectangle so they are the display size rather than the macroblock-padded coded size. `bridge_client decode` no longer takes dimensions at all, and `h264_selinuxbridge`/`hevc_selinuxbridge` now exist as decoders. |
| 14 | **Rate control was inaccurate.** `KEY_BITRATE_MODE` was never set, so Codec2 picked its own default -- VBR on this device's `c2.qti.*.encoder` components, where the requested bitrate is only an average the encoder may exceed freely. An explicit `-b:v` came back **+25% at 2 Mbps, +31% at 6 Mbps and +34% at 12 Mbps**, measured on ordinary content rather than a synthetic worst case. (Distinct from gap #11: that was zero timestamps making the encoder think the clip was instantaneous, which *under*-shot; this is the mode itself.) | Medium | ✅ fixed | Protocol v5 carries a rate-control mode in the **high byte of the codec field**, so the 24-byte header is unchanged and a v3/v4 client -- which sends a bare 0..3 -- lands on the new CBR default automatically. `bridge_client -r cbr\|vbr\|cq`, `hw-transcode -r`, and `ffmpeg -rc_mode cbr\|vbr\|cq`. CQ reinterprets the bitrate field as a quality in 1..100. An unsupported mode falls back to plain `KEY_BIT_RATE` rather than failing the session, and `info` now lists which modes each encoder advertises. |
| 15 | **A stale process was invisible.** Android normally kills an app's process when its package is replaced, so the next start runs the new code. A long-lived foreground service makes surviving that much more likely, and nothing then reloads it: `BootReceiver`'s `MY_PACKAGE_REPLACED` handler calls `startForegroundService()`, but that only delivers another `onStartCommand()` to the **already loaded** classes. The bridge kept serving the old protocol while the user was looking at a successful install, with no symptom at all beyond a version number that never changed. Hit for real on the v4 → v5 update. | Medium | ✅ fixed | `info` now reports the **build timestamp of the code actually answering**, and the service compares the package's `lastUpdateTime` against the value this process saw at startup. `lastUpdateTime` moves only on replacement, so a difference means the package changed *while this process was already running* — exactly the stale case, with no timing heuristic to get wrong. The app footer always shows `protocol vN · build <timestamp>`, in the same format `info` uses, so the running version can be read off the screen and compared directly. When a mismatch is detected the warning appears in `info`, in `bridge.log`, and as a banner at the top of the app carrying a **Restart now** button, which ends the process so `START_STICKY` restarts the service on the new code — refusing while any transcode is in flight. |
| 16 | **Constant quality silently degraded to VBR.** Qualcomm splits rate control across *components*: `c2.qti.hevc.encoder` does CBR and VBR, while constant quality lives on a **separate** `c2.qti.hevc.encoder.cq`. Picking the first `c2.qti.*` match therefore handed every CQ request to a component that cannot do CQ, which then fell back to its default — VBR — and encoded anyway. `-c h264 -r cq` produced a file **byte-for-byte the same size** as `-r vbr` (7,679,474 B), i.e. the mode was doing nothing at all. Found by this round of hardware testing, in a feature added one commit earlier. | High | ✅ fixed | Component selection is now rate-control aware and prefers one that supports the requested mode, so CQ lands on `*.encoder.cq`. If no component supports it the encode is **refused**, naming what is supported (`rate control 'cq' is not supported by c2.qti.avc.encoder (supported: cbr,vbr)`), because silently substituting rate control is precisely what hid #14. |
| 17 | **A browser could not be given a live camera *and* live audio at the same time.** Chromium's only route for an injected camera was `--use-file-for-fake-video-capture`, and that file device is only registered when `--use-fake-device-for-media-stream` is also present — verified by removing it, after which `getUserMedia({video:true})` failed outright (`videoOpened False`, `0x0`). But that same switch also replaces the **microphone** with a synthetic tone: a clip recorded through the demo came back clipping at full scale with a dead-constant RMS of 14448, nothing like the room. `FileVideoCaptureDevice` also plays the file from its first byte and never seeks, so the picture started as far behind the present as the file was long. | Medium | ✅ fixed | **Superseded by gaps #20–#24 and now closed.** The premise was wrong: a browser does not need a fake device if it is given a real one. With `native/hw-enable install`, an ordinary Chromium — no switches, no wrapper — enumerates `Phone Camera (SELinux Bridge)` and `phone_mic`, and `getUserMedia({video:true,audio:true})` returns **both live at once**. Measured: 720x1280@30, 4044 frames dequeued in one session, 18100/19200 sampled pixels non-black with frame-to-frame differences tracking live sensor noise, and the microphone reading the same signal `parec` sees. The camera can also be released and re-acquired repeatedly inside one browser process, which is what a real video call does. |
| 18 | **A capture session was accepted before it was checked.** `handleCamera` wrote its OK status line *and then* called `CameraSource.stream()`, which is where the camera index is validated. So an invalid index — `bridge_client camera -i 9` — produced `handshake ok, capture source: camera:9`, a session that opened and immediately stopped, an empty file, and **exit 0**. The client's own accounting said `camera: 0 unit(s), 0 byte(s)` and nothing anywhere said why. The mock refused it correctly, and the selftest passed, because the mock validates *before* replying — so the test suite was asserting the right behaviour against the wrong ordering, and only real hardware could show the difference. Found in the first hour of running v6 against the phone, in a feature added one commit earlier. | Medium | ✅ fixed | Camera selection is now a separate `CameraSource.resolve()` that runs **before** the status line, so a bad index is refused with `camera index 9 out of range (this device has 4, so 0..3; 'bridge_client info' lists them)` and a nonzero exit. `MicSource.check()` does the same for an audio format the device will not accept. Belt and braces on the client too: a capture that ends without ever receiving a format record now reports `capture produced nothing` and fails, so this class of silent acceptance cannot recur even from a bridge that gets the ordering wrong. The selftest gained a stand-in bridge that accepts and then dies, which is the exact shape of the bug. |
| 19 | **The camera came out on its side.** A phone's sensor is mounted to suit the industrial design rather than the screen, so the HAL hands over frames rotated by a fixed angle — 90° on this device's back sensors, 270° on the front. `CameraSource` passed those buffers through untouched, so every capture arrived a quarter turn over: `bridge-webcam` into `ffplay`, the ffmpeg encoders, and the browser demo all showed a sideways picture. Nothing in the protocol carried the angle either, so a downstream reader had no way to discover it and no way to ask for it. Invisible to every test in the suite, because the mock has no sensor and byte counts are identical whichever way up the image is. | Medium | ✅ fixed | `CameraSource` now reads `SENSOR_ORIENTATION` and rotates before the frame goes on the wire, so a capture is upright with nothing downstream having to know. The directive rides in **bits 8–15 of the existing codec field** — the same trick gap #14 used for rate control — so the 24-byte header is unchanged and there is no protocol bump; `0` means "use the sensor's own angle", which is what an existing v6 client sends, so it gets the fix for free. `bridge_client --rotate none\|90\|180\|270` overrides it. The rotation is a **blocked transpose** rather than the obvious nested loop, which would miss the cache on every write; it costs nothing worth reporting at 720p. A quarter turn swaps the *announced* dimensions, so the format record (gap #13) is what makes this safe — the size you ask for still picks the sensor mode, rather than the bridge quietly choosing a worse mode to hit the number you typed. `info` prints each sensor's angle. |
| 20 | **There was no camera device at all.** Everything above reached the camera through a FIFO or a file, because the container cannot create a device node: that needs `v4l2loopback`, hence `CAP_SYS_MODULE` and a module tree, and here `CapEff` is `0000000000000000` and `/lib/modules` does not exist. So every consumer had to be told where the camera was, and anything that insists on a V4L2 node — which is most things — could not be served. | **High** | ✅ fixed | `native/v4l2-shim.c` answers *for* the device instead of creating one. An application does not talk to a driver; it talks to libc, and a V4L2 client makes about a dozen distinct libc calls. Interposing those (`open`, `close`, `read`, `ioctl`, `mmap`/`mmap64`, `munmap`, the `stat` family) is indistinguishable from a driver, from the application's side. Stock `ffmpeg` captures **60 frames in exactly 2.000000 s at 30 fps** from `/dev/video0` with no flags; VLC, Chromium and `udevadm` all work unmodified. Geometry is discovered rather than assumed, because asking for 1280x720 yields **720x1280** here. |
| 21 | **Every Chromium- and Electron-based application aborted on launch.** Not "ran slowly" or "fell back to software" — the GPU process died with `exit_code=256` before any window appeared, so Chromium, VS Code and every Electron app were unusable. `--use-angle=swiftshader` failed identically, which ruled out the GL backend. The cause was `libpci` calling `pcilib: Cannot open /proc/bus/pci/devices`, a path this container cannot read at all (`ls` reports `-?????????? ?`). | **High** | ✅ fixed | `native/pci-shim.c` answers that one path. A/B control: **3 crashes** with the shim moved aside, **0** with it in place; with it, `/proc/<gpu-pid>/maps` contains `libvulkan_freedreno.so` and 16 `/opt/mesa-adreno` mappings and **zero** swrast/llvmpipe/SwiftShader entries — so the browser is not merely starting, it is on the Adreno. |
| 22 | **Device enumeration returned nothing, so browsers reported no camera *and* no microphone.** With `/dev/video0` working and PulseAudio serving audio, Chromium still listed **0 video and 0 audio** devices. `libudev` opens a `NETLINK_KOBJECT_UEVENT` socket during initialisation; that socket is denied here, `udev_monitor_new_from_netlink()` fails, and libudev then reports an empty device list rather than an error. One denied socket therefore zeroed out both device classes — and the microphone had nothing to do with netlink, which is what made this so misleading to chase. | **High** | ✅ fixed | `native/netlink-shim.c` hands out a working substitute socket so libudev initialises. Chromium then reports `1v/2a` and both `getUserMedia({audio:true})` and `{video:true}` succeed. |
| 23 | **The camera was openable but not discoverable.** `ffmpeg -i /dev/video0` worked while browsers and GStreamer still showed nothing, because they never scan `/dev`: they walk `/sys/class/video4linux`, read `name` and `dev` out of each entry, and only then open the node they were told about. Here `/sys` is traversable but not listable — the directories carry `x` without `r` — so `opendir("/sys/class/video4linux")` fails with `EACCES` while `open()` of a known path underneath succeeds. | **High** | ✅ fixed | `native/sysfs-shim.c` overlays a synthetic entry, generated at install time so it can name this device and reproduce the symlink layout a real driver produces (the class entry links into `/sys/devices`, the device links back to its subsystem; udev follows both and rejects an entry where they disagree). The overlay is **additive** — a path is redirected only where the synthetic tree has something at it — because a blanket `/sys` redirect breaks the C library itself, which reads `/sys/devices/system/cpu/online`. Verified: `ls /sys/class/video4linux` goes from `Permission denied` to `video0`, `udevadm info` goes from `Unknown device: No such device` to a full resolution (`N: video0`, `D: c 81:0`, `U: video4linux`), and `/sys/devices/system/cpu/online` still reads `0-7` through the shim. |
| 24 | **`libv4l2` applications bypassed the shim entirely.** VLC opened `/dev/video0`, and then failed at the first `VIDIOC_QUERYCAP` with `EACCES` — the kernel's answer to an `ioctl` on the pipe backing our handle. Tracing showed the `open()` reaching the shim and **not a single `ioctl` following it**. `libv4l2` — the userspace conversion layer VLC, cheese and most GTK camera apps go through — deliberately does not call libc: its private header defines `SYS_IOCTL` and friends as direct `syscall()` invocations, because `libv4l2` also ships `v4l2convert.so`, which interposes those very symbols, so calling them would make it recurse into itself. Nothing in the shim was wrong; it simply was not being asked. | Medium | ✅ fixed | `syscall()` is itself an ordinary libc function, so the shim interposes **it** as well and routes `SYS_ioctl`/`SYS_read`/`SYS_close`/`SYS_mmap`/`SYS_munmap`/`SYS_openat` on our handles back through the same implementations. The passthrough uses inline `svc` rather than `dlsym(RTLD_NEXT, "syscall")`, because `syscall()` can be called before the constructor has run and must not depend on the dynamic linker having got there first. VLC went from a 160-byte empty file to a **10.4 MB H.264 capture, 720x1280, 3069 frames**. |
| 25 | **Applications launched from the desktop menu got none of it.** The install hooked `~/.xprofile`, which is where a session is conventionally given its environment — and on this system **`startxfce4` never reads that file**. It execs `$XDG_CONFIG_HOME/xfce4/xinitrc` if present and `/etc/xdg/xfce4/xinitrc` otherwise, so the hook *looked* installed, reported `ok`, and did nothing. The resulting symptom is the confusing kind: the camera works in a terminal and is absent from the very same application started from the menu, which points suspicion at the application rather than at the environment it inherited. | Medium | ✅ fixed | `hw-enable` now writes the session hook where the session actually looks, generating `~/.config/xfce4/xinitrc` (or inserting a marked block into one that already exists) as well as `.xprofile`. The generated wrapper does not assume the system file carries an exec bit, because a wrapper that cannot hand over is not a missing camera but a desktop that never appears. `status` additionally reports whether the **running** session has the shims, since installing cannot retrofit a desktop that is already up. Uninstall is exact either way: a file this script generated is removed outright, one that pre-existed has only its block cut out and comes back byte for byte. |
| 26 | **GStreamer could not capture at all.** `v4l2src` negotiated caps, allocated and mapped four buffers, and then failed with `Buffer pool activation failed` / `not-negotiated`, which points at format negotiation — the one thing that had actually succeeded. The real message was two lines deeper in `GST_DEBUG`: `error with STREAMON 13 (Permission denied)`. `gst_v4l2_buffer_pool_new()` **dups** the device fd and drives streaming through the copy, and `dup` was not interposed, so the copy was the bare pipe underneath our handle and `EACCES` was the kernel answering an ioctl on a pipe. This matters well beyond `gst-launch`: GStreamer is what Cheese, GNOME Camera and a long tail of GTK applications use. | Medium | ✅ fixed | `dup`, `dup2`, `dup3` and `fcntl`/`fcntl64` with `F_DUPFD` are now interposed, and the result is registered as a **duplicate** rather than as a secondary handle — the distinction being that a `dup` shares one file description, so it must share the capture, where a second `open()` is a different description and is still refused at `REQBUFS` (gap #17's rule). Closing either reference leaves the capture running on the other, and if the original goes first the capture is handed to a surviving duplicate, which is what a pool outliving its object does. `fcntl64` is answered as well as `fcntl` for the same reason `mmap64` is. Verified: `gst-launch-1.0 v4l2src` writes **30 of 30 unique JPEGs**, and the pre-fix shim fails the new `tests/dupfd.c` with exactly `STREAMON on the dup: Permission denied`. |

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
| #14 rate | Same log, after the fix | `bitrate-mode=2` — `BITRATE_MODE_CBR`, i.e. the setting reaches the component |
| #14 rate | 300 frames of noisy 1080p, `cbr` vs `vbr` on identical input, real hardware | **cbr −0.7% / −0.2% / −1.3%** at 2 / 6 / 12 Mbps; `vbr` **+206% / +27.4% / +27.9%** on the same input. CBR holds the rate to within 1.3% everywhere; VBR shipped 3× the requested bits at 2 Mbps |
| #14 rate | PSNR of those CBR encodes | 31.57 / 32.72 / 33.11 dB average, rising monotonically with bitrate, all three planes within 0.9 dB of each other |
| #16 cq | `-c h264 -r cq` vs `-r vbr`, real hardware, before the fix | identical output size to the byte (7,679,474 B) — the mode was inert |
| #16 cq | `-c hevc -r cq`, real hardware, after the fix | selects `c2.qti.hevc.encoder.cq`, and quality now bites: q=40 → 15.3 MB, q=90 → 41.5 MB from the same 120 frames |
| #16 cq | `-c h264 -r cq`, real hardware, after the fix | refused: `rate control 'cq' is not supported by c2.qti.avc.encoder (supported: cbr,vbr)` — this device ships no AVC CQ component |
| #15 stale | v4 → v5 in-place update, real device | reproduced: APK on disk reported v5, the running bridge still reported v4, and `info` showed none of the v5 markers |
| #12 tests | `./tools/selftest` with no device attached | **47 passed, 0 failed** |
| #6 camera | `bridge_client camera -i 0 1280 720 30 30`, **real device** | 30 frames in **1.77 s**, exactly **41,472,000 B** (30 × 1280 × 720 × 1.5) |
| #6 camera | Frame content off the real sensor | a sharp, correctly exposed image (mean 138, stddev 61, range 3–255) with correct colour — so the capture-side I420 chroma path is right, which is the failure gap #10 was |
| #6 camera | Sustained 300 frames of 720p30 through the FIFO, **real device** | **28.6 fps** against a 30 fps target — ≈40 MB/s of raw I420 over loopback |
| #6 camera | `info` camera enumeration, **real device** | 4 cameras (2 back, 2 front), max 4080x3060, permissions reported `granted` |
| #6 camera | HAL size rounding, **real device** | 800x450 → **960x720**, 1000x1000 → **1088x1088**, 320x240 → exact; announced by the format record, which is precisely why it is mandatory |
| #6 camera | On-demand behaviour, **real device** | no bridge session at all while idle; camera opened the instant a reader attached, `reader gone, camera released` when it left |
| #6 mic | `bridge_client mic 48000 1 3`, **real device** | exactly **288,000 B** = 3.000 s of 48 kHz mono S16, in 3.15 s wall; rms 497 |
| #6 mic | `bridge-mic` → PulseAudio → `parec`, **real device** | rms 567, **−35.2 dBFS** — real room ambience, matching the −34.6 dBFS of the direct capture |
| #6 mic | All four audio sources, 2 s each, **real device** | each exactly **192,000 B**, and measurably different: `mic` −34.0, `camcorder` −32.0, `unprocessed` −42.0, `voice` **−72.7** dBFS. The 39 dB gap on `voice` is the platform noise suppressor gating steady room noise, i.e. the source selection really does reach the hardware |
| #6+#7 | **Phone camera → phone hardware H.264 encoder → mp4**, driven from Debian, two concurrent bridge sessions | 150 frames of 720p in **5.75 s**, 3.2 MB, `nb_frames=150`, decodes clean and the image survives intact |
| #17 browser | Chromium `getUserMedia({audio:true})` against the **real phone mic** | non-zero RMS across all 8 samples (`0.0482 … 0.0051`) — a web app really does get live phone audio |
| #18 order | `camera -i 9` on **real hardware**, before the fix | `handshake ok`, 0 units, 0 bytes, **exit 0** — the bug, reproduced |
| #18 order | Same command on **real hardware**, after the fix | refused at the status line: `camera index 9 out of range (this device has 4, so 0..3; 'bridge_client info' lists them)`, **exit 1** |
| #18 order | `mic 48000 5` (an impossible channel count), **real hardware** | refused: `channels must be 1 or 2, got 5`, **exit 1** |
| #18 order | Capture regression check on the same build | camera 20 frames byte-exact, mic 192,000 B = 2.00 s — refusing bad requests did not break good ones |
| #18 order | Stand-in bridge that accepts a capture session then dies | client reports `capture produced nothing` and exits nonzero instead of leaving an empty file |
| #6+#7 | **Phone camera → phone hardware HEVC encoder → mp4**, new build | 120 frames of 720p in **4.87 s**, 1.5 MB, `codec_name=hevc nb_frames=120` |
| #6 camera | `bridge-webcam` + `ffmpeg` reading the FIFO, 20 frames of 640x480 | exactly **9,216,000 B** (20 × 640 × 480 × 1.5) and **18 distinct** per-frame luma means — live, changing content rather than a repeated frame |
| #6 camera | Bridge sessions counted while nothing was reading the FIFO | **no session at all** for 3 s of idling, then exactly one the instant a reader attached — the camera is opened on demand, not held open |
| #6 camera | Reader detaches mid-stream | `reader gone, camera released`, writer re-armed for the next reader, no orphan left behind |
| #6 camera | `bridge-webcam status` across idle → reading → idle | reports `idle` / `streaming (reader pid …)` / `idle`, read from `/proc/*/fd` rather than inferred from the log |
| #6 mic | `bridge-mic start`, then `parec --device=bridge_mic` | source created, made default, **rms 8486** — real signal, not silence |
| #6 mic | Bridge → file, continuous, 15 s | **14.90 s of audio in 15.02 s of wall clock (0.992x)** — the phone delivers 48 kHz accurately |
| #6 mic | Old path: `module-pipe-source` → `parec`, 20 s | **24.10 s of audio (1.204x)** — the pipe source manufactures silence during gaps and still emits the real samples |
| #6 mic | Old path: `module-pipe-source` → `ffmpeg`, 30 s | **44.30 s of audio (1.445x)**, i.e. 14.3 s of lip-sync drift in half a minute |
| #6 mic | New path: null sink + `pacat` → `parec`, 20 s | **19.95 s of audio (0.996x)**, RMS 610, 99.9% non-zero — correct rate, real audio |
| #6 mic | 40 s camera + microphone captured together, new path | **600 video frames (40.00 s) and 40.00 s of audio — zero net drift** |
| #6 mic | Feeder killed, leaving the source with no writer | reader **blocks forever** rather than failing; `bridge-mic status` now reports `half-dead` instead |
| #17 browser | Chromium `getUserMedia({audio:true})` against `bridge_mic` | device enumerated and **non-zero RMS** (0.0366 → 0.0095 as WebRTC's AGC settles) — browser audio works |
| #17 browser | Chromium `getUserMedia({video:true})` against a **growing** file | picture follows the file: **50 distinct frames, 0 repeats** in 25 s. (An earlier run reported a constant `means=130.0` and was misread as a frozen reader — the prefill was uniform grey, so a moving picture and a stuck one looked identical.) |
| #17 browser | Feed truncated immediately before `getUserMedia` | **lag 0.13 s**, and in a screenshot the clock burned into the picture reads `14:24:33` while the page's own clock reads `2:24:33 PM` — the same second |
| #17 browser | Chromium launched **without** `--use-fake-device-for-media-stream` | `videoOpened False`, `videoSize 0x0` — the file video device is not registered without that switch |
| #17 browser | Audio recorded through the demo **with** that switch | full-scale clipping, constant RMS 14448 — Chromium's synthetic tone, not the phone. The switch fakes the microphone as well as the camera |
| #17 browser | Same, pointed at a FIFO | `NotFoundError: Requested device not found` — rejected at open |
| #17 browser | `bridge-browser --clip 3`, then Chromium `getUserMedia({video:true})` | `ok w=640 h=480 meanR=138.3 label=…/webcam-clip.y4m` — a recording from the phone camera *is* accepted as a camera, which is why `--clip` is the one video path offered |
| #19 rotate | `camera -i 0`, **real device**, before the fix | picture arrived a quarter turn over in `ffplay`, the ffmpeg encoders and the browser demo alike — the bug, reproduced everywhere the camera was used |
| #19 rotate | `camera -i 2 640 480`, front sensor (270°), **real device** | announced and delivered as **480x640, upright**; the requested size still selected the sensor mode, so the pixel count is unchanged and only the dimensions swap |
| #19 rotate | 8 JVM unit tests over the rotation paths (`tools/test-i420`) | corners move the right way at 90°, U and V travel with the luma, 180° reverses each plane, **four 90° turns are the identity**, 270° undoes 90°, sizes straddling the tile edge survive, `rotate 0` returns the frame untouched, and odd dimensions are **refused rather than corrupted** — 18/18 with the chroma cases |
| #19 rotate | Cost of the blocked transpose at 1280x720, **real device** | **24.8 fps rotated against 25.0 fps raw**, byte counts identical — the turn is free at capture rates |
| #19 rotate | `--rotate none` against the same sensor, **real device** | exactly what the sensor saw, i.e. the override reaches the hardware path rather than being applied twice or ignored |
| #19 rotate | A **v6 client that predates the fix** (sends a bare codec field) | rotates by the sensor angle automatically — `0` means "ask the sensor", so the header is wire-identical and old clients are fixed without being rebuilt |
| #20 device | Stock `ffmpeg -f v4l2 -i /dev/video0`, no flags, no wrapper | **60 frames in exactly 2.000000 s at 30 fps**, 720x1280, repeated after every change to the shim |
| #20 device | Two handles open at once (`tools/twoopen`-style probe) | second `open()` succeeds and answers `QUERYCAP`/`ENUM_FMT`; `REQBUFS` on it returns **EBUSY**; the first handle still delivers **30/30** frames — the kernel's own policy, which is many opens and exclusive *streaming* |
| #20 device | Acquire / release / re-acquire, 3 cycles, with a blocked `DQBUF` racing each teardown | **PASS**, blocked `DQBUF` returns within 0 ms of `STREAMOFF` every cycle |
| #21 pci | Chromium launched 3x with the shim moved aside, then 3x with it in place | **3 GPU-process crashes → 0**; with the shim, the GPU process maps `libvulkan_freedreno.so` and **no** software rasteriser |
| #22 udev | Chromium `enumerateDevices()` before / after the netlink shim | **0v/0a → 1v/2a** |
| #23 sysfs | `ls /sys/class/video4linux` before / after | `Permission denied` → `video0` |
| #23 sysfs | `udevadm info --path=/class/video4linux/video0` before / after | `Unknown device: No such device` → `N: video0`, `D: c 81:0`, `U: video4linux`, `E: DEVNAME=/dev/video0` |
| #23 sysfs | `/sys/devices/system/cpu/online` read through the overlay | `0-7`, unchanged — the overlay is additive, so glibc's own sysfs reads still work |
| #24 libv4l2 | VLC `v4l2:///dev/video0` → H.264 mp4, before the `syscall()` interposer | **160-byte empty file**; trace shows `open()` arriving and zero `ioctl`s |
| #24 libv4l2 | Same command after it | **10.4 MB, H.264, 720x1280, 3069 frames**; and 1429 frames from a plain login shell with no `LD_PRELOAD` typed at all |
| #17 browser | `getUserMedia({video:true, audio:true})` in an unmodified Chromium | **both live at once**: `Phone Camera (SELinux Bridge)` + `Default`, 720x1280@30, 18100/19200 sampled pixels non-black, frame-to-frame diffs tracking live sensor noise |
| #17 browser | Camera requested, released and requested again inside one browser process | `VIDEO#0=OK`, `VIDEO#1=OK`, `BOTH#0=OK` — what a real video call does |
| #17 browser | Microphone cross-check: browser analyser vs `parec` on the same source | agree — `parec` reads rms **41**, peak **233**, 99.0% non-zero over 7.94 s; the browser's lower numbers are the same signal scaled into the 0–255 byte domain |
| native | `hw-enable doctor` after `hw-enable install` | **all native hardware paths are working** — bridge reachable, 10 frames captured, libudev resolves the camera, real sysfs intact, 524,792 B of live audio in 3 s, Adreno GPU, Chromium renders, `h264_selinuxbridge` present |
| native | A **fresh login shell**, nothing typed | all four shims on `LD_PRELOAD`; `ffmpeg` captures 60 frames, `ls /sys/class/video4linux` lists `video0`, `udevadm` resolves it, default PulseAudio source is `phone_mic` |
| #25 session | Generated `~/.config/xfce4/xinitrc`, then `hw-enable uninstall` | file **removed outright**; `.xprofile` cleaned |
| #25 session | A pre-existing user `xinitrc` with its own settings, through install and uninstall | block inserted above the user's lines, then removed — `diff` reports the file **identical to the original** |
| #25 session | `hw-enable status` against a desktop started before the install | warns that menu-launched apps will not see the hardware until the session restarts, and that new terminals are fine |
| native | `native/run-tests` (ffmpeg, concurrent handles, re-acquire, duplicated handles, libv4l2, libudev) | **7 passed, 0 failed** |
| #26 gstreamer | `gst-launch-1.0 v4l2src ! jpegenc ! multifilesink`, 30 buffers | **30 of 30 frames written and all 30 distinct**, ~100–116 KB each, real luminance — not a frozen or black frame |
| #26 dup | `tests/dupfd.c`: allocate on the original, `STREAMON`/`DQBUF` on the duplicate | 1,382,400-byte frame through the dup; capture survives closing either reference, in both orders |
| #26 control | The same test against the **pre-fix** shim built from the previous commit | fails at exactly `STREAMON on the dup: Permission denied`, so the test measures the fix rather than the weather |
| #26 regression | Whole container re-checked under the new `dup`/`fcntl` interposers: `ls`, pipes, `git`, `tar`, `sort`, shell fd redirection, Python `os.dup` | all unaffected |
| #26 browser | Chromium re-run after the change: `getUserMedia({video:true,audio:true})`, snapshot, enumerate, release, re-acquire | `video=1 audio=1`, **720x1280 with 489,439/921,600 non-black pixels**, `video=1 audio=2` devices, re-acquire ok |

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
- **A browser can have the phone's camera or the phone's microphone, but
  not both at once**, and this one is not a gap in the bridge: the single
  Chromium switch that registers the injected camera also replaces the
  microphone with a synthetic tone (gap #17 has the measurements). The
  earlier claim here — that Chromium buffers the file at open and never
  re-reads it — was wrong; it reads continuously but starts from the first
  byte, so `bridge-demo` truncates the feed just before opening and gets
  the picture to within **0.13 s** of the present. Native applications get
  a genuinely live camera from `bridge-webcam`'s FIFO, and browsers get
  **working live audio** from `bridge-mic`. PipeWire would lift the
  either/or restriction but needs root to install and is untested here.
- **Sensors, GPS, NFC and the other hardware are still not bridged.**
  The pattern generalises — a new `mode=`, a branch in
  `BridgeService.java` using the relevant Android API, and a
  `bridge_client` subcommand; the loopback plumbing and per-client
  threading do not change — but only the codec, camera and microphone
  exist today.
- **Only H.264 and HEVC reach `ffmpeg`.** VP9 and AV1 are selectable over
  the wire (`bridge_client -c vp9`) but have no libavcodec wrapper, because
  neither has an `mp4toannexb`-style filter to normalise input framing and
  VP9/AV1 hardware encode is not advertised on every device that advertises
  decode. `bridge_client` remains the way to reach them.

- **VP9 and AV1 encode is software even here.** On the SM-F971U1 `info`
  lists no `c2.qti.*` encoder for either — only `c2.android.vp9.encoder`
  and `c2.android.av1.encoder`. They work, but they are CPU encoders, so
  they buy none of the CPU saving that is the point of this bridge. Check
  for the `[hardware]` marker in `info` before assuming otherwise. Decode
  is a different story: `c2.qti.vp9.decoder` and `c2.qti.av1.decoder` are
  both present and both hardware.
