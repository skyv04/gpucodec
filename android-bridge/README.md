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

**`android-bridge/` is that APK.** It's a minimal Android app whose only job
is to expose real hardware `MediaCodec` (H.264 encode and decode) over a
loopback TCP socket, so the Debian/PRoot side can drive it directly.

## What's here

| File | Purpose |
|---|---|
| `AndroidManifest.xml` | One activity, one foreground service. `INTERNET` permission only for the loopback socket. |
| `app/src/main/java/com/gpucodec/bridge/MainActivity.java` | Launcher UI; starts the bridge service in the foreground so Android doesn't kill it. |
| `app/src/main/java/com/gpucodec/bridge/BridgeService.java` | The actual bridge: `ServerSocket` on `127.0.0.1:7878`, wire protocol below, drives `android.media.MediaCodec` synchronously. |
| `bridge_client.c` | Debian-side test client: connects over loopback, sends raw frames or a bitstream, prints round-trip stats. |
| `build.sh` | Rebuilds the signed APK from source using raw SDK command-line tools (`aapt2`, `javac`, `d8`, `apksigner`) — no gradle/network dependency. |

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

## Wire protocol

All integers are 4-byte big-endian (`DataInputStream`/`DataOutputStream`
network order). One TCP connection = one encode or decode session.

1. Client → server, once: `mode(0=encode,1=decode) width height fps bitrate`
   (`fps`/`bitrate` only matter for encode; send 0 for decode)
2. Server → client, once: `status` (`0` = codec configured OK)
3. Then, repeated:
   - Client → server: `int32 length` then `length` bytes (one raw YUV420
     flexible frame for encode, or one H.264 Annex-B unit for decode).
     `length == -1` signals end-of-stream, no bytes follow.
   - Server → client: `int32 length` then `length` bytes of the
     corresponding output (H.264 Annex-B for encode, raw YUV420 flexible
     for decode). `length == -1` on the final reply signals end-of-stream.

## Testing the bridge

Once the app is installed and open on-screen:

```
# encode: 320x180 raw NV12/I420 frames -> H.264 Annex B
./bridge_client encode 320 180 30 2000000 in.yuv420 out.h264

# decode: a full H.264 elementary stream -> raw YUV420 frames (concatenated)
./bridge_client decode 320 180 in.h264 out.yuv420
```

Check the app's logcat-equivalent (Android Studio's Logcat, or `adb logcat`
once connected) for a line like:

```
codec = c2.qti.avc.encoder
```

If the codec name starts with `c2.qti.`, that's Qualcomm's real hardware
Codec2 component doing the work — not software (`c2.android.*`) and not
AGC-1. That single log line is the answer to "does a real APK unlock the
hardware codec": if it's there and frames flow, yes; if the app crashes or
`codec` prints a software name, more work is needed (e.g. explicit codec
selection via `MediaCodecList`).

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
