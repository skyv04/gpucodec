package com.selinuxbridge.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.graphics.Rect;
import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.PrintWriter;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;

/**
 * Real hardware MediaCodec bridge, reachable over a loopback TCP socket.
 *
 * This exists because the Termux/PRoot Debian container is denied
 * /dev/dma_heap/* (Codec2's buffer allocator) and cannot exec into
 * /system/bin/app_process (labeled zygote_exec) from its untrusted_app_27
 * SELinux domain. A real, installed APK gets a normal Zygote-forked app
 * process with a real UID and app SELinux domain, which *is* allowed to
 * use MediaCodec/Codec2 normally. This service is that bridge: it runs
 * inside a real app process, and exposes hardware encode/decode to the
 * Debian side over 127.0.0.1, where no cross-domain permission problem
 * exists (it's just a normal socket).
 *
 * Verified working end-to-end on this device (see selinux-bridge/README.md
 * for the transcript): 30-frame encode -> valid H.264 -> decode -> 30
 * frames back, using c2.qti.* hardware codec components.
 *
 * Wire protocol v5 (all integers big-endian / DataInputStream/
 * DataOutputStream network order):
 *
 *   Client -> Server, once per connection:
 *     int32 mode        0 = encode (raw YUV420 flexible in, bitstream out)
 *                        1 = decode (bitstream in, raw YUV420 flexible out)
 *                        2 = info   (diagnostics only; all fields below
 *                            are ignored and may be zero)
 *                        3 = log    (streams back this app's bridge.log,
 *                            which Android scoped storage otherwise hides
 *                            from the Debian side; other fields ignored)
 *     int32 width
 *     int32 height      (decode may send 0 x 0: the real size comes back
 *                        in a format record, see below)
 *     int32 fps         (encode only, ignored otherwise)
 *     int32 bitrate     (encode only, ignored otherwise)
 *     int32 codec       bits 0-7:  0 = H.264/AVC, 1 = HEVC/H.265, 2 = VP9,
 *                                  3 = AV1 (new in v3)
 *                       bits 8-15: rate-control mode, encode only, new in
 *                                  v5. 0 = CBR, 1 = VBR, 2 = CQ (in which
 *                                  case the `bitrate` field above carries a
 *                                  quality in 1..100 rather than bits/s).
 *                                  A v3/v4 client sends 0..3 here, so its
 *                                  high bits are zero and it gets CBR --
 *                                  which is the point: leaving the mode
 *                                  unset made Codec2 pick VBR, and an
 *                                  explicit `-b:v 6M` then came back
 *                                  25-34% over target at every bitrate.
 *
 *   Server -> Client, once:
 *     int32 status      0 = ok, nonzero = failed
 *     if status == 0:
 *       int32 nameLen, then `nameLen` bytes: UTF-8 codec name actually
 *       selected (e.g. "c2.qti.avc.encoder"), or "n/a" for modes 2/3.
 *     if status != 0:
 *       int32 msgLen, then `msgLen` bytes: UTF-8 human-readable error.
 *       Connection is closed after this; no further frames follow.
 *
 *   Then, for mode 0/1, repeated in both directions as applicable:
 *     Client -> Server: int32 length, then `length` bytes of one input unit
 *                       (length == -1 means end-of-stream, no bytes follow)
 *     Server -> Client: int32 length, then `length` bytes of one output unit
 *                       (length == -1 means end-of-stream, no bytes follow)
 *
 *   New in v4, decode only: the server's output stream may carry format
 *   records, `int32 -2` followed by `int32 width` and `int32 height`. One
 *   always precedes the first frame, and another is emitted whenever the
 *   picture size changes mid-stream. Every frame that follows is tightly
 *   packed I420 at those dimensions. This is what lets a client -- notably
 *   the libavcodec decoder in ffmpeg/selinuxbridge.c -- learn the real
 *   picture size without parsing the bitstream, and survive a resolution
 *   change. Lengths <= -3 are reserved; a client must treat one as a
 *   protocol error rather than guessing.
 *
 *   For modes 2 and 3, the server sends one or more payload chunks (a text
 *   diagnostics report, or the log file's contents) followed by the -1 EOS
 *   marker, then closes. No client input is read.
 */
public class BridgeService extends Service {
    private static final String TAG = "SELinuxBridge";
    private static final int PORT = 7878;
    private static final String CHANNEL_ID = "selinux_bridge";

    /*
     * Codec2 exposes a hard cap on simultaneously-configured codec
     * instances. Exceeding it does not fail cleanly -- configure()/start()
     * block indefinitely, which in testing showed up as 4 concurrent
     * sessions hanging (3 were fine) and emitting truncated streams. Gate
     * sessions behind a semaphore sized from the codec's own advertised
     * limit and reject the overflow with a real error message, so a client
     * gets told "busy" immediately instead of hanging forever.
     */
    private static final int CODEC_SLOT_LIMIT = maxConcurrentInstances();
    private static final Semaphore CODEC_SLOTS = new Semaphore(CODEC_SLOT_LIMIT, true);

    /** Sessions holding a codec right now. Used to refuse an unsafe restart. */
    static int activeSessions() {
        return CODEC_SLOT_LIMIT - CODEC_SLOTS.availablePermits();
    }

    /**
     * How long a client will wait for a free codec slot before being told
     * the bridge is busy. Long enough to ride out a session that is about
     * to finish, short enough that a caller gets a real answer quickly.
     */
    private static final long SLOT_WAIT_MS = 5_000L;

    private Thread serverThread;
    private volatile boolean running = true;
    private File logFile;

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        // logcat is unreachable from the Termux/PRoot shell this bridge
        // serves, so mirror everything to a plain file under the app's own
        // external files dir too -- readable from Debian via /sdcard on
        // this device without any special permission.
        File dir = getExternalFilesDir(null);
        logFile = new File(dir != null ? dir : getFilesDir(), "bridge.log");
        log("service created, log file: " + logFile.getAbsolutePath());
        String stale = staleProcessWarning(this);
        if (stale != null) log(stale.replace('\n', ' '));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        startForeground(1, buildNotification());
        if (serverThread == null) {
            serverThread = new Thread(this::serverLoop, "bridge-accept");
            serverThread.start();
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        running = false;
        log("service destroyed");
        super.onDestroy();
    }

    private void log(String msg) {
        String line = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US)
                .format(new Date()) + " " + msg;
        Log.i(TAG, msg);
        try (FileWriter fw = new FileWriter(logFile, true);
             PrintWriter pw = new PrintWriter(fw)) {
            pw.println(line);
        } catch (IOException e) {
            Log.e(TAG, "failed to write log file", e);
        }
    }

    private Notification buildNotification() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel ch = new NotificationChannel(
                    CHANNEL_ID, "SELinux Hardware Bridge", NotificationManager.IMPORTANCE_LOW);
            nm.createNotificationChannel(ch);
        }
        /*
         * Tapping the notification has to reopen the app, because that is the
         * one recovery the user always has: a force-stopped bridge cannot
         * restart itself (gap #4), and the notification is the only handle on
         * it once the app is off-screen.
         */
        PendingIntent open = PendingIntent.getActivity(
                this, 0, new Intent(this, MainActivity.class)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                                | Intent.FLAG_ACTIVITY_CLEAR_TOP),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        return new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("SELinux Hardware Bridge")
                .setContentText("Hardware MediaCodec bridge on 127.0.0.1:" + PORT)
                .setSmallIcon(android.R.drawable.stat_sys_download_done)
                .setContentIntent(open)
                .setOngoing(true)
                .setShowWhen(false)
                .build();
    }

    private void serverLoop() {
        try (ServerSocket server = new ServerSocket(PORT, 8, InetAddress.getByName("127.0.0.1"))) {
            log("listening on 127.0.0.1:" + PORT);
            while (running) {
                Socket sock = server.accept();
                sock.setTcpNoDelay(true);
                log("client connected from " + sock.getRemoteSocketAddress());
                Thread t = new Thread(() -> handleClient(sock), "bridge-client");
                t.start();
            }
        } catch (IOException e) {
            log("server loop failed: " + e);
        }
    }

    /**
     * Picks a real Codec2 component by name where possible, so a device
     * that also ships software fallbacks (c2.android.*, OMX.google.*)
     * cannot silently substitute one for the hardware path this bridge
     * exists to prove out. Falls back to the platform default
     * (createEncoderByType/createDecoderByType) if no hardware match is
     * found, which is still correct behavior on devices without a
     * Qualcomm Codec2 stack.
     */
    /**
     * Maps the wire protocol's numeric codec id to an Android MIME type.
     * Returns null for an unknown id so the caller can report a clean
     * protocol error rather than guessing.
     */
    private static String mimeForCodecId(int id) {
        switch (id) {
            case 0: return MediaFormat.MIMETYPE_VIDEO_AVC;
            case 1: return MediaFormat.MIMETYPE_VIDEO_HEVC;
            case 2: return MediaFormat.MIMETYPE_VIDEO_VP9;
            case 3: return MediaFormat.MIMETYPE_VIDEO_AV1;
            default: return null;
        }
    }

    /**
     * Smallest max-instance count advertised by the hardware components
     * this bridge might actually pick, minus a safety margin of one.
     *
     * Codec2 reports this via CodecCapabilities.getMaxSupportedInstances().
     * It is a per-component figure and encode/decode share the pool in
     * practice, so take the minimum across the components we'd select and
     * leave one instance spare -- observed behaviour is that saturating the
     * limit exactly still risks a blocking configure(), and a bridge that
     * says "busy" is far better than one that hangs.
     */
    private static int maxConcurrentInstances() {
        int min = Integer.MAX_VALUE;
        try {
            MediaCodecList list = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
            for (MediaCodecInfo info : list.getCodecInfos()) {
                String name = info.getName().toLowerCase(Locale.US);
                if (!name.startsWith("c2.qti.")) continue;
                for (String t : info.getSupportedTypes()) {
                    // Only the video types this bridge can actually be asked
                    // for -- c2.qti.* also covers audio components whose
                    // instance limits have nothing to do with the video
                    // hardware the semaphore is protecting.
                    if (mimeIsSupportedVideo(t)) {
                        int n = info.getCapabilitiesForType(t).getMaxSupportedInstances();
                        if (n > 0 && n < min) min = n;
                    }
                }
            }
        } catch (Throwable ignored) {
            // This runs in a static initializer: anything escaping here would
            // stop the service loading at all, so swallow it and fall back.
        }
        if (min == Integer.MAX_VALUE) return 3;
        return Math.max(1, min - 1);
    }

    private static boolean mimeIsSupportedVideo(String mime) {
        for (int id = 0; id <= 3; id++) {
            String m = mimeForCodecId(id);
            if (m != null && m.equalsIgnoreCase(mime)) return true;
        }
        return false;
    }

    /**
     * @param rcMode requested rate control for encoders, or -1 to not care
     *               (decoders, which have no such notion).
     *
     * Qualcomm splits rate control across components: c2.qti.hevc.encoder
     * does CBR and VBR, while constant quality lives on a *separate*
     * c2.qti.hevc.encoder.cq. Taking the first c2.qti.* match therefore
     * hands a CQ request to a component that cannot do CQ. So when a mode
     * is asked for, prefer a component that supports it, while still
     * defaulting to the first hardware match when nothing does.
     */
    private static MediaCodec selectCodec(String mime, boolean encoder, int rcMode)
            throws IOException {
        MediaCodecList list = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
        String preferred = null;
        String rcCapable = null;
        for (MediaCodecInfo info : list.getCodecInfos()) {
            if (info.isEncoder() != encoder) continue;
            boolean supportsMime = false;
            for (String t : info.getSupportedTypes()) {
                if (t.equalsIgnoreCase(mime)) { supportsMime = true; break; }
            }
            if (!supportsMime) continue;
            String name = info.getName();
            // Qualcomm's real hardware Codec2 components are named
            // "c2.qti.*"; software ones are "c2.android.*"/"OMX.google.*".
            if (!name.toLowerCase(Locale.US).startsWith("c2.qti.")) continue;
            if (preferred == null) preferred = name;
            if (!encoder || rcMode < 0) break;
            if (supportsRateMode(info, mime, rcMode)) { rcCapable = name; break; }
        }
        String chosen = rcCapable != null ? rcCapable : preferred;
        if (chosen != null) {
            return MediaCodec.createByCodecName(chosen);
        }
        return encoder ? MediaCodec.createEncoderByType(mime) : MediaCodec.createDecoderByType(mime);
    }

    /**
     * Apply the requested rate-control mode to an encoder format.
     *
     * Leaving KEY_BITRATE_MODE unset is not neutral: Codec2 then picks its
     * own default, which on this device's c2.qti.*.encoder components is
     * VBR, where the requested bitrate is only an average the encoder may
     * exceed freely. Measured on real hardware that overshot an explicit
     * request by +25% at 2 Mbps, +31% at 6 Mbps and +34% at 12 Mbps -- on
     * ordinary content, not a synthetic worst case. Anyone passing
     * `-b:v 6M` means 6 Mbps, so CBR is the default here.
     *
     * Not every component advertises every mode, so an unsupported request
     * falls back to plain KEY_BIT_RATE rather than failing the session:
     * a slightly-wrong bitrate beats no encode at all.
     */
    private void applyRateControl(MediaFormat fmt, MediaCodec codec,
                                  int rcMode, int bitrate) throws IOException {
        int mode = androidBitrateMode(rcMode);
        String mime = fmt.getString(MediaFormat.KEY_MIME);

        boolean supported = false;
        try {
            MediaCodecInfo.EncoderCapabilities ec = codec.getCodecInfo()
                    .getCapabilitiesForType(mime).getEncoderCapabilities();
            supported = ec != null && ec.isBitrateModeSupported(mode);
        } catch (Exception e) {
            log("could not query encoder bitrate modes: " + e);
        }

        if (!supported) {
            /*
             * Refuse rather than quietly encode in some other mode. Silently
             * substituting rate control is precisely how gap #14 hid for so
             * long: the caller asks for one thing, gets another, and only
             * finds out by measuring the output afterwards. A caller that
             * asked for CQ and got VBR can overshoot its bitrate several
             * times over, so this has to be loud.
             */
            String have = rateModeList(codec.getCodecInfo(), mime);
            throw new IOException("rate control '" + rcName(rcMode).toLowerCase(Locale.US)
                    + "' is not supported by " + codec.getName()
                    + (have.isEmpty() ? "" : " (supported: " + have + ")"));
        }

        if (rcMode == 2) {
            /* In CQ the bitrate field carries a quality, not bits/s. */
            int quality = bitrate > 0 ? Math.min(bitrate, 100) : 80;
            fmt.setInteger(MediaFormat.KEY_BITRATE_MODE, mode);
            fmt.setInteger(MediaFormat.KEY_QUALITY, quality);
            log("rate control: CQ quality=" + quality + " on " + codec.getName());
            return;
        }

        fmt.setInteger(MediaFormat.KEY_BIT_RATE, bitrate > 0 ? bitrate : 4_000_000);
        fmt.setInteger(MediaFormat.KEY_BITRATE_MODE, mode);
        log("rate control: " + rcName(rcMode) + " at "
                + (bitrate > 0 ? bitrate : 4_000_000) + " bps on " + codec.getName());
    }

    private static String rcName(int rcMode) {
        switch (rcMode) {
            case 1:  return "VBR";
            case 2:  return "CQ";
            default: return "CBR";
        }
    }

    /** " rc: cbr,vbr" -- which rate-control modes this encoder will honour. */
    private static String rateModes(MediaCodecInfo info, String mime) {
        String list = rateModeList(info, mime);
        return list.isEmpty() ? "" : "  rc: " + list;
    }

    /** Comma-separated rate-control modes a component actually supports. */
    private static String rateModeList(MediaCodecInfo info, String mime) {
        try {
            MediaCodecInfo.EncoderCapabilities ec =
                    info.getCapabilitiesForType(mime).getEncoderCapabilities();
            if (ec == null) return "";
            StringBuilder m = new StringBuilder();
            if (ec.isBitrateModeSupported(
                    MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)) m.append("cbr,");
            if (ec.isBitrateModeSupported(
                    MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)) m.append("vbr,");
            if (ec.isBitrateModeSupported(
                    MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CQ)) m.append("cq,");
            if (m.length() == 0) return "";
            return m.substring(0, m.length() - 1);
        } catch (Exception e) {
            return "";
        }
    }

    /** Wire rate-control mode -> MediaCodec BITRATE_MODE_* constant. */
    private static int androidBitrateMode(int rcMode) {
        switch (rcMode) {
            case 1:  return MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR;
            case 2:  return MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CQ;
            default: return MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR;
        }
    }

    private static boolean supportsRateMode(MediaCodecInfo info, String mime, int rcMode) {
        try {
            MediaCodecInfo.EncoderCapabilities ec =
                    info.getCapabilitiesForType(mime).getEncoderCapabilities();
            return ec != null && ec.isBitrateModeSupported(androidBitrateMode(rcMode));
        } catch (Exception e) {
            return false;
        }
    }

    /**
     * Detects the case where this process is running code older than the APK
     * currently installed on disk.
     *
     * Android usually kills an app's process when its package is replaced,
     * so the next start picks up the new code. When the process survives --
     * which a long-lived foreground service makes much more likely -- nothing
     * reloads it: BootReceiver's MY_PACKAGE_REPLACED handler calls
     * startForegroundService(), but that only delivers another
     * onStartCommand() to the *already loaded* classes. The bridge then keeps
     * serving the old protocol indefinitely while the user is looking at a
     * successful install, with no symptom beyond a version number that never
     * changes. Observed exactly once, on the v4 -> v5 update.
     *
     * The test is exact rather than a timing heuristic. The first code in
     * this process to ask records the package's lastUpdateTime as it stood
     * at startup; lastUpdateTime moves only when the package is replaced, so
     * if it has moved since, the replacement happened *while this process was
     * already running* -- which is precisely the stale case. Comparing a
     * build timestamp against lastUpdateTime instead would have to guess how
     * long a user takes to install an APK, and would cry wolf on every
     * ordinary update.
     */
    private static long updateTimeAtStart = -1;

    static synchronized String staleProcessWarning(android.content.Context ctx) {
        try {
            long now = ctx.getPackageManager()
                    .getPackageInfo(ctx.getPackageName(), 0).lastUpdateTime;
            if (updateTimeAtStart < 0) {
                updateTimeAtStart = now;
                return null;
            }
            if (now != updateTimeAtStart) {
                return "STALE PROCESS: the package was replaced at "
                        + new SimpleDateFormat("HH:mm:ss", Locale.US).format(new Date(now))
                        + ", after this process started.\n"
                        + "  The update did not restart the service, so this is still the old\n"
                        + "  build. Force-stop the app and reopen it (or reboot).\n";
            }
        } catch (Exception e) {
            /* Never let a diagnostic break the diagnostics. */
        }
        return null;
    }

    private String infoReport() {
        StringBuilder sb = new StringBuilder();
        sb.append("SELinux Hardware Bridge diagnostics\n");
        sb.append("device: ").append(Build.MODEL).append(" (" ).append(Build.HARDWARE).append(")\n");
        sb.append("android: ").append(Build.VERSION.RELEASE)
          .append(" (sdk ").append(Build.VERSION.SDK_INT).append(")\n");
        sb.append("protocol: v5\n");
        // Names the build actually answering, which is the quickest way to
        // tell an update that took effect from one that did not.
        sb.append("build: ").append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US)
                .format(new Date(BuildStamp.BUILD_TIME))).append("\n");
        String stale = staleProcessWarning(this);
        if (stale != null) sb.append(stale);
        sb.append("concurrent codec slots: ").append(CODEC_SLOT_LIMIT)
          .append(" (").append(CODEC_SLOTS.availablePermits()).append(" free)\n\n");

        MediaCodecList list = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
        String[] mimes = {
                MediaFormat.MIMETYPE_VIDEO_AVC,
                MediaFormat.MIMETYPE_VIDEO_HEVC,
                MediaFormat.MIMETYPE_VIDEO_VP9,
                MediaFormat.MIMETYPE_VIDEO_AV1,
        };
        String[] labels = {"h264 (AVC)", "hevc (H.265)", "vp9", "av1"};

        for (int i = 0; i < mimes.length; i++) {
            sb.append(labels[i]).append("  [codec id ").append(i).append("]\n");
            boolean any = false;
            for (MediaCodecInfo info : list.getCodecInfos()) {
                boolean supports = false;
                for (String t : info.getSupportedTypes()) {
                    if (t.equalsIgnoreCase(mimes[i])) { supports = true; break; }
                }
                if (!supports) continue;
                any = true;
                String kind = info.isEncoder() ? "encoder" : "decoder";
                boolean hw = info.getName().toLowerCase(Locale.US).startsWith("c2.qti.");
                sb.append("  ").append(kind).append(": ").append(info.getName())
                  .append(hw ? " [hardware]" : "");
                if (info.isEncoder()) sb.append(rateModes(info, mimes[i]));
                sb.append("\n");
            }
            if (!any) sb.append("  (none)\n");
            sb.append("\n");
        }
        return sb.toString();
    }

    private void handleClient(Socket sock) {
        try (Socket s = sock;
             DataInputStream in = new DataInputStream(s.getInputStream());
             DataOutputStream out = new DataOutputStream(s.getOutputStream())) {

            int mode = in.readInt();
            int width = in.readInt();
            int height = in.readInt();
            int fps = in.readInt();
            int bitrate = in.readInt();
            int codecField = in.readInt();
            int codecId = codecField & 0xff;
            int rcMode = (codecField >> 8) & 0xff;

            if (mode == 2) {
                writeOkStatus(out, "n/a");
                writeChunk(out, infoReport().getBytes("UTF-8"));
                out.writeInt(-1);
                out.flush();
                log("served info request");
                return;
            }

            if (mode == 3) {
                writeOkStatus(out, "n/a");
                streamLog(out);
                out.writeInt(-1);
                out.flush();
                log("served log request");
                return;
            }

            if (mode != 0 && mode != 1) {
                writeErrorStatus(out, "unknown mode " + mode);
                return;
            }

            String mime = mimeForCodecId(codecId);
            if (mime == null) {
                writeErrorStatus(out, "unknown codec id " + codecId
                        + " (expected 0=h264, 1=hevc, 2=vp9, 3=av1)");
                return;
            }
            if (rcMode > 2) {
                writeErrorStatus(out, "unknown rate-control mode " + rcMode
                        + " (expected 0=cbr, 1=vbr, 2=cq)");
                return;
            }

            /*
             * Acquire a codec slot before touching MediaCodec at all.
             * A short timed wait absorbs the normal case where another
             * session is about to finish; past that, refuse rather than
             * block, since a blocked configure() is unrecoverable and
             * silently corrupts the stream for everyone.
             */
            boolean slotHeld;
            try {
                slotHeld = CODEC_SLOTS.tryAcquire(SLOT_WAIT_MS, TimeUnit.MILLISECONDS);
            } catch (InterruptedException ie) {
                Thread.currentThread().interrupt();
                return;
            }
            if (!slotHeld) {
                log("rejecting session: all " + CODEC_SLOT_LIMIT + " codec slots busy");
                writeErrorStatus(out, "all " + CODEC_SLOT_LIMIT
                        + " hardware codec slots are busy, try again shortly");
                return;
            }

            try {
                MediaCodec codec;
                try {
                    if (mode == 0) {
                        codec = selectCodec(mime, true, rcMode);
                        MediaFormat fmt = MediaFormat.createVideoFormat(mime, width, height);
                        fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                                MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
                        applyRateControl(fmt, codec, rcMode, bitrate);
                        fmt.setInteger(MediaFormat.KEY_FRAME_RATE, fps > 0 ? fps : 30);
                        fmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
                        codec.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
                    } else {
                        codec = selectCodec(mime, false, -1);
                        /*
                         * A decode client is allowed not to know the picture
                         * size -- that is the whole point of the v4 format
                         * record. MediaFormat still wants something at
                         * configure() time, and the component overrides it
                         * from the stream's own parameter sets, so a hint is
                         * enough.
                         */
                        int cfgW = width > 0 ? width : 1920;
                        int cfgH = height > 0 ? height : 1080;
                        MediaFormat fmt = MediaFormat.createVideoFormat(mime, cfgW, cfgH);
                        codec.configure(fmt, null, null, 0);
                    }
                } catch (Exception e) {
                    log("codec setup failed: " + e);
                    writeErrorStatus(out, "codec setup failed: " + e);
                    return;
                }

                String codecName = codec.getName();
                log("codec = " + codecName + " (" + mime + ", " + width + "x" + height + ")");
                codec.start();
                writeOkStatus(out, codecName);

                try {
                    runPump(codec, in, out, mode == 0, width, height, fps);
                } catch (Exception e) {
                    // A mid-stream failure (client vanished, codec faulted, app
                    // backgrounded and killed the process, etc.) must not take
                    // the whole service down -- this thread simply ends and
                    // the accept loop keeps serving new connections.
                    log("pump failed: " + e);
                } finally {
                    try { codec.stop(); } catch (Exception ignored) {}
                    try { codec.release(); } catch (Exception ignored) {}
                }
            } finally {
                CODEC_SLOTS.release();
            }
        } catch (Exception e) {
            log("client session failed: " + e);
        }
        log("client disconnected");
    }

    private static void writeChunk(DataOutputStream out, byte[] data) throws IOException {
        out.writeInt(data.length);
        out.write(data);
    }

    /**
     * Streams this app's bridge.log back over the socket.
     *
     * Android 11+ scoped storage makes /sdcard/Android/data/<pkg>/ opaque
     * to every other app and to the Termux/PRoot shell, so the Debian side
     * cannot simply read the file. Serving it over the same loopback socket
     * that already works avoids needing adb or root just to see the log.
     */
    private void streamLog(DataOutputStream out) throws IOException {
        if (logFile == null || !logFile.exists()) {
            writeChunk(out, "(no log file yet)\n".getBytes("UTF-8"));
            return;
        }
        writeChunk(out, ("=== " + logFile.getAbsolutePath() + " ===\n").getBytes("UTF-8"));
        byte[] buf = new byte[64 * 1024];
        try (FileInputStream fis = new FileInputStream(logFile)) {
            int n;
            while ((n = fis.read(buf)) > 0) {
                out.writeInt(n);
                out.write(buf, 0, n);
            }
        }
        out.flush();
    }

    private static void writeOkStatus(DataOutputStream out, String name) throws IOException {
        out.writeInt(0);
        byte[] nb = name.getBytes("UTF-8");
        out.writeInt(nb.length);
        out.write(nb);
        out.flush();
    }

    private static void writeErrorStatus(DataOutputStream out, String msg) throws IOException {
        out.writeInt(1);
        byte[] mb = msg.getBytes("UTF-8");
        out.writeInt(mb.length);
        out.write(mb);
        out.flush();
    }

    private static int formatInt(MediaFormat fmt, String key, int fallback) {
        try {
            if (fmt != null && fmt.containsKey(key)) return fmt.getInteger(key);
        } catch (Exception ignored) {}
        return fallback;
    }

    /*
     * MediaFormat's width/height are the *coded* size, padded up to the
     * component's macroblock alignment; the display size is the crop
     * rectangle. getOutputImage() already applies the crop, so these two are
     * only needed on the fallback ByteBuffer path. The crop keys are
     * inclusive on both ends, hence the +1.
     */
    private static int cropWidth(MediaFormat fmt, int fallback) {
        if (fmt != null && fmt.containsKey("crop-left") && fmt.containsKey("crop-right")) {
            int w = formatInt(fmt, "crop-right", 0) - formatInt(fmt, "crop-left", 0) + 1;
            if (w > 0) return w;
        }
        return formatInt(fmt, MediaFormat.KEY_WIDTH, fallback);
    }

    private static int cropHeight(MediaFormat fmt, int fallback) {
        if (fmt != null && fmt.containsKey("crop-top") && fmt.containsKey("crop-bottom")) {
            int h = formatInt(fmt, "crop-bottom", 0) - formatInt(fmt, "crop-top", 0) + 1;
            if (h > 0) return h;
        }
        return formatInt(fmt, MediaFormat.KEY_HEIGHT, fallback);
    }

    /** Synchronous dequeue/enqueue pump. Simple and robust for a bridge process. */
    private void runPump(MediaCodec codec, DataInputStream in, DataOutputStream out,
                         boolean encode, int width, int height, int fps) throws IOException {
        boolean inputDone = false;
        boolean outputDone = false;
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        long framesIn = 0, framesOut = 0;
        /* Last picture size announced to the client via a v4 format record. */
        int announcedW = -1, announcedH = -1;

        /*
         * Rate control is driven entirely by presentation timestamps: queueing
         * every frame at 0 makes the encoder believe the whole clip is
         * instantaneous and it undershoots the requested bitrate badly.
         */
        int rate = fps > 0 ? fps : 30;
        long frameDurUs = 1_000_000L / rate;

        /*
         * When the input is raw video the component picks its own stride and
         * slice height, so the size reported to queueInputBuffer has to follow
         * the codec's layout rather than the picture dimensions.
         */
        int rawSize = 0;
        if (encode) {
            MediaFormat inFmt = null;
            try { inFmt = codec.getInputFormat(); } catch (Exception ignored) {}
            int stride = formatInt(inFmt, MediaFormat.KEY_STRIDE, width);
            int sliceH = formatInt(inFmt, MediaFormat.KEY_SLICE_HEIGHT, height);
            if (stride < width) stride = width;
            if (sliceH < height) sliceH = height;
            rawSize = stride * sliceH * 3 / 2;
        }

        while (!outputDone) {
            if (!inputDone) {
                int ibIdx = codec.dequeueInputBuffer(10_000);
                if (ibIdx >= 0) {
                    int len = in.readInt();
                    if (len < 0) {
                        codec.queueInputBuffer(ibIdx, 0, 0, framesIn * frameDurUs,
                                MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        inputDone = true;
                    } else {
                        byte[] tmp = new byte[len];
                        in.readFully(tmp);
                        int size = len;
                        Image img = encode ? codec.getInputImage(ibIdx) : null;
                        if (img != null) {
                            I420.fill(img, tmp, width, height);
                            ByteBuffer raw = codec.getInputBuffer(ibIdx);
                            size = rawSize;
                            if (raw != null && size > raw.capacity()) size = raw.capacity();
                        } else {
                            ByteBuffer buf = codec.getInputBuffer(ibIdx);
                            buf.clear();
                            buf.put(tmp);
                        }
                        codec.queueInputBuffer(ibIdx, 0, size, framesIn * frameDurUs, 0);
                        framesIn++;
                    }
                }
            }

            int obIdx = codec.dequeueOutputBuffer(info, 10_000);
            if (obIdx >= 0) {
                byte[] tmp;
                int outW = -1, outH = -1;
                if (!encode && info.size > 0) {
                    Image img = codec.getOutputImage(obIdx);
                    if (img != null) {
                        Rect crop = img.getCropRect();
                        outW = crop.width();
                        outH = crop.height();
                        tmp = I420.toI420(img);
                    } else {
                        ByteBuffer buf = codec.getOutputBuffer(obIdx);
                        tmp = new byte[info.size];
                        buf.position(info.offset);
                        buf.limit(info.offset + info.size);
                        buf.get(tmp);
                        MediaFormat of = null;
                        try { of = codec.getOutputFormat(obIdx); } catch (Exception ignored) {}
                        outW = cropWidth(of, width);
                        outH = cropHeight(of, height);
                    }
                } else {
                    ByteBuffer buf = codec.getOutputBuffer(obIdx);
                    tmp = new byte[Math.max(info.size, 0)];
                    if (buf != null && info.size > 0) {
                        buf.position(info.offset);
                        buf.limit(info.offset + info.size);
                        buf.get(tmp);
                    }
                }
                /*
                 * Protocol v4: announce the picture size before the frame it
                 * applies to, and again whenever it changes. A decode client
                 * therefore never has to parse the bitstream to size its
                 * buffers, and a mid-stream resolution change is a normal
                 * event rather than a stream of mis-sized frames.
                 */
                if (!encode && outW > 0 && outH > 0
                        && (outW != announcedW || outH != announcedH)) {
                    out.writeInt(-2);
                    out.writeInt(outW);
                    out.writeInt(outH);
                    announcedW = outW;
                    announcedH = outH;
                    log("announcing output size " + outW + "x" + outH);
                }
                out.writeInt(tmp.length);
                if (tmp.length > 0) out.write(tmp);
                out.flush();
                codec.releaseOutputBuffer(obIdx, false);
                if (tmp.length > 0) framesOut++;
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                    outputDone = true;
                    out.writeInt(-1);
                    out.flush();
                }
            } else if (obIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                log("output format changed: " + codec.getOutputFormat());
            }
        }
        log("session done: framesIn=" + framesIn + " framesOut=" + framesOut);
    }
}
