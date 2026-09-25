package com.selinuxbridge.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
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
 * Wire protocol v3 (all integers big-endian / DataInputStream/
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
 *     int32 height
 *     int32 fps         (encode only, ignored otherwise)
 *     int32 bitrate     (encode only, ignored otherwise)
 *     int32 codec       0 = H.264/AVC, 1 = HEVC/H.265, 2 = VP9, 3 = AV1
 *                        (new in v3; encode/decode only, ignored otherwise)
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
        return new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("SELinux Hardware Bridge")
                .setContentText("Hardware MediaCodec bridge on 127.0.0.1:" + PORT)
                .setSmallIcon(android.R.drawable.stat_sys_download_done)
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

    private static MediaCodec selectCodec(String mime, boolean encoder) throws IOException {
        MediaCodecList list = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
        String preferred = null;
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
            if (name.toLowerCase(Locale.US).startsWith("c2.qti.")) {
                preferred = name;
                break;
            }
        }
        if (preferred != null) {
            return MediaCodec.createByCodecName(preferred);
        }
        return encoder ? MediaCodec.createEncoderByType(mime) : MediaCodec.createDecoderByType(mime);
    }

    private static String infoReport() {
        StringBuilder sb = new StringBuilder();
        sb.append("SELinux Hardware Bridge diagnostics\n");
        sb.append("device: ").append(Build.MODEL).append(" (" ).append(Build.HARDWARE).append(")\n");
        sb.append("android: ").append(Build.VERSION.RELEASE)
          .append(" (sdk ").append(Build.VERSION.SDK_INT).append(")\n");
        sb.append("protocol: v3\n");
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
                  .append(hw ? " [hardware]" : "").append("\n");
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
            int codecId = in.readInt();

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
                        codec = selectCodec(mime, true);
                        MediaFormat fmt = MediaFormat.createVideoFormat(mime, width, height);
                        fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                                MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
                        fmt.setInteger(MediaFormat.KEY_BIT_RATE, bitrate > 0 ? bitrate : 4_000_000);
                        fmt.setInteger(MediaFormat.KEY_FRAME_RATE, fps > 0 ? fps : 30);
                        fmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
                        codec.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
                    } else {
                        codec = selectCodec(mime, false);
                        MediaFormat fmt = MediaFormat.createVideoFormat(mime, width, height);
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

    /** Synchronous dequeue/enqueue pump. Simple and robust for a bridge process. */
    private void runPump(MediaCodec codec, DataInputStream in, DataOutputStream out,
                         boolean encode, int width, int height, int fps) throws IOException {
        boolean inputDone = false;
        boolean outputDone = false;
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        long framesIn = 0, framesOut = 0;

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
                if (!encode && info.size > 0) {
                    Image img = codec.getOutputImage(obIdx);
                    if (img != null) {
                        tmp = I420.toI420(img);
                    } else {
                        ByteBuffer buf = codec.getOutputBuffer(obIdx);
                        tmp = new byte[info.size];
                        buf.position(info.offset);
                        buf.limit(info.offset + info.size);
                        buf.get(tmp);
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
