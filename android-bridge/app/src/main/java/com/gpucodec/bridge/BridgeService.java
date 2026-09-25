package com.gpucodec.bridge;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
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
 * Verified working end-to-end on this device (see android-bridge/README.md
 * for the transcript): 30-frame encode -> valid H.264 -> decode -> 30
 * frames back, using c2.qti.* hardware codec components.
 *
 * Wire protocol v2 (all integers big-endian / DataInputStream/
 * DataOutputStream network order):
 *
 *   Client -> Server, once per connection:
 *     int32 mode        0 = encode (raw YUV420 flexible in, H.264 Annex B out)
 *                        1 = decode (H.264 Annex B in, raw YUV420 flexible out)
 *                        2 = info   (diagnostics only; width/height/fps/
 *                            bitrate below are ignored and may be zero)
 *     int32 width
 *     int32 height
 *     int32 fps         (encode only, ignored otherwise)
 *     int32 bitrate     (encode only, ignored otherwise)
 *
 *   Server -> Client, once:
 *     int32 status      0 = ok, nonzero = failed
 *     if status == 0:
 *       int32 nameLen, then `nameLen` bytes: UTF-8 codec name actually
 *       selected (e.g. "c2.qti.avc.encoder"), or "n/a" for mode 2.
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
 *   For mode 2, the server sends exactly one payload chunk (a text report
 *   of installed AVC codecs) followed by the -1 EOS marker, then closes.
 */
public class BridgeService extends Service {
    private static final String TAG = "GPUCodecBridge";
    private static final int PORT = 7878;
    private static final String CHANNEL_ID = "gpucodec_bridge";

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
                    CHANNEL_ID, "PRoot Codec Bridge", NotificationManager.IMPORTANCE_LOW);
            nm.createNotificationChannel(ch);
        }
        return new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("PRoot Codec Bridge")
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
        sb.append("PRoot Codec Bridge diagnostics\n");
        sb.append("device: ").append(Build.MODEL).append(" (" ).append(Build.HARDWARE).append(")\n");
        sb.append("android: ").append(Build.VERSION.RELEASE)
          .append(" (sdk ").append(Build.VERSION.SDK_INT).append(")\n\n");
        MediaCodecList list = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
        sb.append("AVC (H.264) codecs available:\n");
        for (MediaCodecInfo info : list.getCodecInfos()) {
            boolean supportsAvc = false;
            for (String t : info.getSupportedTypes()) {
                if (t.equalsIgnoreCase(MediaFormat.MIMETYPE_VIDEO_AVC)) { supportsAvc = true; break; }
            }
            if (!supportsAvc) continue;
            String kind = info.isEncoder() ? "encoder" : "decoder";
            String hw = info.getName().toLowerCase(Locale.US).startsWith("c2.qti.") ? " [hardware]" : "";
            sb.append("  ").append(kind).append(": ").append(info.getName()).append(hw).append("\n");
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

            if (mode == 2) {
                writeOkStatus(out, "n/a");
                String report = infoReport();
                byte[] rb = report.getBytes("UTF-8");
                out.writeInt(rb.length);
                out.write(rb);
                out.writeInt(-1);
                out.flush();
                log("served info request");
                return;
            }

            MediaCodec codec;
            try {
                if (mode == 0) {
                    codec = selectCodec(MediaFormat.MIMETYPE_VIDEO_AVC, true);
                    MediaFormat fmt = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height);
                    fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                            MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
                    fmt.setInteger(MediaFormat.KEY_BIT_RATE, bitrate > 0 ? bitrate : 4_000_000);
                    fmt.setInteger(MediaFormat.KEY_FRAME_RATE, fps > 0 ? fps : 30);
                    fmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
                    codec.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
                } else if (mode == 1) {
                    codec = selectCodec(MediaFormat.MIMETYPE_VIDEO_AVC, false);
                    MediaFormat fmt = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height);
                    codec.configure(fmt, null, null, 0);
                } else {
                    writeErrorStatus(out, "unknown mode " + mode);
                    return;
                }
            } catch (Exception e) {
                log("codec setup failed: " + e);
                writeErrorStatus(out, "codec setup failed: " + e);
                return;
            }

            String codecName = codec.getName();
            log("codec = " + codecName + " (" + width + "x" + height + ")");
            codec.start();
            writeOkStatus(out, codecName);

            try {
                runPump(codec, in, out);
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
        } catch (Exception e) {
            log("client session failed: " + e);
        }
        log("client disconnected");
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

    /** Synchronous dequeue/enqueue pump. Simple and robust for a bridge process. */
    private void runPump(MediaCodec codec, DataInputStream in, DataOutputStream out) throws IOException {
        boolean inputDone = false;
        boolean outputDone = false;
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        long framesIn = 0, framesOut = 0;

        while (!outputDone) {
            if (!inputDone) {
                int ibIdx = codec.dequeueInputBuffer(10_000);
                if (ibIdx >= 0) {
                    int len = in.readInt();
                    if (len < 0) {
                        codec.queueInputBuffer(ibIdx, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        inputDone = true;
                    } else {
                        ByteBuffer buf = codec.getInputBuffer(ibIdx);
                        byte[] tmp = new byte[len];
                        in.readFully(tmp);
                        buf.clear();
                        buf.put(tmp);
                        codec.queueInputBuffer(ibIdx, 0, len, 0, 0);
                        framesIn++;
                    }
                }
            }

            int obIdx = codec.dequeueOutputBuffer(info, 10_000);
            if (obIdx >= 0) {
                ByteBuffer buf = codec.getOutputBuffer(obIdx);
                byte[] tmp = new byte[info.size];
                if (buf != null && info.size > 0) {
                    buf.position(info.offset);
                    buf.limit(info.offset + info.size);
                    buf.get(tmp);
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
