package com.gpucodec.bridge;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;

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
 * Wire protocol (all integers big-endian / DataInputStream/DataOutputStream
 * network order):
 *
 *   Client -> Server, once per connection:
 *     int32 mode        0 = encode (raw YUV420 flexible in, H.264 Annex B out)
 *                        1 = decode (H.264 Annex B in, raw YUV420 flexible out)
 *     int32 width
 *     int32 height
 *     int32 fps         (encode only, ignored for decode)
 *     int32 bitrate     (encode only, ignored for decode)
 *
 *   Server -> Client, once: int32 status (0 = ok, nonzero = failed to configure)
 *
 *   Then repeated, either direction as applicable:
 *     Client -> Server: int32 length, then `length` bytes of one input unit
 *                       (length == -1 means end-of-stream, no bytes follow)
 *     Server -> Client: int32 length, then `length` bytes of one output unit
 *                       (length == -1 means end-of-stream, no bytes follow)
 */
public class BridgeService extends Service {
    private static final String TAG = "GPUCodecBridge";
    private static final int PORT = 7878;
    private static final String CHANNEL_ID = "gpucodec_bridge";

    private Thread serverThread;
    private volatile boolean running = true;

    @Override
    public IBinder onBind(Intent intent) {
        return null;
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
        super.onDestroy();
    }

    private Notification buildNotification() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel ch = new NotificationChannel(
                    CHANNEL_ID, "GPUCodec Bridge", NotificationManager.IMPORTANCE_LOW);
            nm.createNotificationChannel(ch);
        }
        return new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("GPUCodec Bridge")
                .setContentText("Hardware MediaCodec bridge on 127.0.0.1:" + PORT)
                .setSmallIcon(android.R.drawable.stat_sys_download_done)
                .build();
    }

    private void serverLoop() {
        try (ServerSocket server = new ServerSocket(PORT, 4, InetAddress.getByName("127.0.0.1"))) {
            Log.i(TAG, "listening on 127.0.0.1:" + PORT);
            while (running) {
                Socket sock = server.accept();
                Log.i(TAG, "client connected");
                Thread t = new Thread(() -> handleClient(sock), "bridge-client");
                t.start();
            }
        } catch (IOException e) {
            Log.e(TAG, "server loop failed", e);
        }
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

            MediaCodec codec;
            if (mode == 0) {
                codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
                MediaFormat fmt = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height);
                fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                        MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
                fmt.setInteger(MediaFormat.KEY_BIT_RATE, bitrate > 0 ? bitrate : 4_000_000);
                fmt.setInteger(MediaFormat.KEY_FRAME_RATE, fps > 0 ? fps : 30);
                fmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
                codec.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            } else {
                codec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
                MediaFormat fmt = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height);
                codec.configure(fmt, null, null, 0);
            }
            Log.i(TAG, "codec = " + codec.getName());
            codec.start();
            out.writeInt(0); // status ok
            out.flush();

            runPump(codec, in, out);

            codec.stop();
            codec.release();
        } catch (Exception e) {
            Log.e(TAG, "client session failed", e);
        }
        Log.i(TAG, "client disconnected");
    }

    /** Synchronous dequeue/enqueue pump. Simple and robust for a bridge process. */
    private void runPump(MediaCodec codec, DataInputStream in, DataOutputStream out) throws IOException {
        boolean inputDone = false;
        boolean outputDone = false;
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();

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
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                    outputDone = true;
                    out.writeInt(-1);
                    out.flush();
                }
            } else if (obIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                Log.i(TAG, "output format changed: " + codec.getOutputFormat());
            }
        }
    }
}
