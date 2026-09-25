package com.selinuxbridge.app;

import android.content.Context;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Range;
import android.util.Size;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Camera2 capture, handed to the Debian side as tightly packed I420.
 *
 * The container cannot reach the camera itself for the same reason it cannot
 * reach Codec2: its shell runs as untrusted_app_27 with no camera grant and
 * no /dev/video* access, and no amount of work inside PRoot changes that. As
 * with encode and decode, the fix is to do the privileged part in a real app
 * process and ship the *result* over a loopback socket.
 *
 * Two things here are deliberate and easy to get wrong:
 *
 *  - The requested size is a hint. A camera advertises a fixed set of sizes
 *    for YUV_420_888 and silently misbehaves if handed anything else, so the
 *    closest advertised size is chosen and the real one is announced on the
 *    wire with a v4 format record. A client must read that record rather than
 *    assume it got what it asked for.
 *
 *  - Frames are dropped, not queued, when the consumer is slow. This is a
 *    webcam, not a recording: a backlog would show up as a growing delay
 *    between the user moving and the far end seeing it, which is worse than
 *    a dropped frame. The bounded queue below discards the oldest frame on
 *    overflow for exactly that reason.
 */
final class CameraSource {

    private CameraSource() {}

    /** How long to wait for the camera HAL to open before giving up. */
    private static final long OPEN_TIMEOUT_MS = 5000;
    /** Frames buffered between the camera callback and the socket writer. */
    private static final int QUEUE_DEPTH = 3;

    /** Rotation directives, as carried in bits 8-15 of the request's codec field. */
    static final int ROTATE_AUTO = 0;
    static final int ROTATE_NONE = 1;
    static final int ROTATE_90 = 2;
    static final int ROTATE_180 = 3;
    static final int ROTATE_270 = 4;
    /** Sentinel queued when capture has finished. */
    private static final byte[] END = new byte[0];

    interface Logger { void log(String msg); }

    /**
     * Ordered camera list: back cameras first, then front, then anything
     * else. Index 0 is therefore "the rear camera" and index 1 "the selfie
     * camera" on every normal phone, which is what a caller actually wants
     * to say. Raw HAL ids are not stable enough to expose directly.
     */
    static List<String> orderedCameraIds(CameraManager cm) throws CameraAccessException {
        List<String> back = new ArrayList<>(), front = new ArrayList<>(), other = new ArrayList<>();
        for (String id : cm.getCameraIdList()) {
            Integer facing = cm.getCameraCharacteristics(id)
                    .get(CameraCharacteristics.LENS_FACING);
            if (facing != null && facing == CameraCharacteristics.LENS_FACING_BACK) back.add(id);
            else if (facing != null && facing == CameraCharacteristics.LENS_FACING_FRONT) front.add(id);
            else other.add(id);
        }
        List<String> all = new ArrayList<>(back);
        all.addAll(front);
        all.addAll(other);
        return all;
    }

    static String facingOf(CameraManager cm, String id) {
        try {
            Integer f = cm.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING);
            if (f == null) return "unknown";
            if (f == CameraCharacteristics.LENS_FACING_BACK) return "back";
            if (f == CameraCharacteristics.LENS_FACING_FRONT) return "front";
            return "external";
        } catch (CameraAccessException e) {
            return "unknown";
        }
    }

    /** Sizes this camera can actually deliver as YUV_420_888. */
    static Size[] supportedSizes(CameraManager cm, String id) throws CameraAccessException {
        StreamConfigurationMap map = cm.getCameraCharacteristics(id)
                .get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        if (map == null) return new Size[0];
        Size[] sizes = map.getOutputSizes(ImageFormat.YUV_420_888);
        return sizes == null ? new Size[0] : sizes;
    }

    /**
     * Closest advertised size to the request, preferring one that is at
     * least as large so the client can downscale rather than upscale. Falls
     * back to smallest-area difference when nothing is big enough.
     */
    static Size chooseSize(Size[] avail, int reqW, int reqH) {
        if (avail.length == 0) return null;
        long want = (long) reqW * reqH;
        Size best = null;
        long bestScore = Long.MAX_VALUE;
        for (Size s : avail) {
            long area = (long) s.getWidth() * s.getHeight();
            // Penalise undersized options so a big-enough size wins ties.
            long score = Math.abs(area - want) + (area < want ? want : 0);
            if (s.getWidth() == reqW && s.getHeight() == reqH) return s;
            if (score < bestScore) { bestScore = score; best = s; }
        }
        return best;
    }

    /**
     * A resolved camera: which HAL id, and at what size.
     *
     * This exists so that everything that can fail for a reason worth
     * telling the caller about -- no cameras, an index out of range, no
     * usable size -- happens *before* the session's status line is
     * written. Once that line says OK there is nowhere left to report an
     * error, and the client sees a stream that simply stops, which is the
     * kind of silent failure this bridge is supposed to avoid.
     */
    static final class Plan {
        final int index;
        final String id;
        final Size size;
        final int rotation;
        Plan(int index, String id, Size size, int rotation) {
            this.index = index; this.id = id; this.size = size; this.rotation = rotation;
        }
        /** Picture size as delivered to the client, i.e. after rotation. */
        int width() { return quarter() ? size.getHeight() : size.getWidth(); }
        int height() { return quarter() ? size.getWidth() : size.getHeight(); }
        private boolean quarter() { return rotation == 90 || rotation == 270; }
    }

    /**
     * Turns the rotation directive carried in the request into degrees.
     *
     * 0 means "auto", which is what every existing client sends and what
     * anyone actually wants: use the sensor's mounting angle so the picture
     * comes out upright. The explicit values exist because a tripod, a
     * desk stand or a downstream filter can each make the automatic answer
     * the wrong one, and because "give me exactly what the sensor saw" has
     * to stay reachable for debugging.
     */
    static int rotationDegrees(CameraManager cm, String id, int directive) throws IOException {
        switch (directive) {
            case ROTATE_AUTO: return sensorOrientation(cm, id);
            case ROTATE_NONE: return 0;
            case ROTATE_90: return 90;
            case ROTATE_180: return 180;
            case ROTATE_270: return 270;
            default:
                throw new IOException("rotation directive " + directive + " is not one of"
                        + " 0=auto 1=none 2=90 3=180 4=270");
        }
    }

    /** The sensor's mounting angle, or 0 if the device will not say. */
    static int sensorOrientation(CameraManager cm, String id) {
        try {
            Integer o = cm.getCameraCharacteristics(id)
                    .get(CameraCharacteristics.SENSOR_ORIENTATION);
            return o == null ? 0 : ((o % 360) + 360) % 360;
        } catch (CameraAccessException e) {
            return 0;
        }
    }

    /** Picks the camera, size and rotation, or throws with a message worth showing a user. */
    static Plan resolve(Context ctx, int cameraIndex, int reqW, int reqH, int rotateDirective)
            throws IOException {
        CameraManager cm = (CameraManager) ctx.getSystemService(Context.CAMERA_SERVICE);
        if (cm == null) throw new IOException("no camera service");
        try {
            List<String> ids = orderedCameraIds(cm);
            if (ids.isEmpty()) throw new IOException("device reports no cameras");
            if (cameraIndex < 0 || cameraIndex >= ids.size()) {
                throw new IOException("camera index " + cameraIndex + " out of range"
                        + " (this device has " + ids.size() + ", so 0.." + (ids.size() - 1)
                        + "; 'bridge_client info' lists them)");
            }
            String id = ids.get(cameraIndex);
            int rotation = rotationDegrees(cm, id, rotateDirective);
            /*
             * The hint selects the *sensor* size, not the delivered one.
             * Un-rotating it first would be the tidier-looking choice and is
             * wrong: sensors only advertise landscape sizes, so a quarter
             * turn always yields a portrait picture, and asking for the
             * nearest thing to 720x1280 just picks a worse sensor mode for
             * the same portrait result. Ask for 1280x720 from a sideways
             * sensor and you get that many pixels, announced as 720x1280.
             */
            Size size = chooseSize(supportedSizes(cm, id), reqW, reqH);
            if (size == null) throw new IOException("camera " + id + " offers no YUV_420_888 size");
            return new Plan(cameraIndex, id, size, rotation);
        } catch (CameraAccessException e) {
            throw new IOException("camera enumeration failed: " + e.getMessage(), e);
        }
    }

    /**
     * Streams I420 frames into {@code sink} until {@code maxFrames} have been
     * sent, the sink throws (client hung up), or {@code stop} is signalled.
     *
     * @param maxFrames 0 means "until the client disconnects".
     * @return the number of frames actually delivered.
     */
    static int stream(Context ctx, Plan plan, int fps,
                      int maxFrames, FrameSink sink, Logger logger) throws IOException {

        CameraManager cm = (CameraManager) ctx.getSystemService(Context.CAMERA_SERVICE);
        if (cm == null) throw new IOException("no camera service");

        String id = plan.id;
        Size size = plan.size;

        logger.log("camera " + plan.index + " (hal id " + id + ", " + facingOf(cm, id)
                + ") using " + size.getWidth()
                + "x" + size.getHeight() + " @ " + fps + "fps"
                + (plan.rotation == 0 ? " (sensor is upright)"
                   : ", rotated " + plan.rotation + " deg to "
                     + plan.width() + "x" + plan.height()));

        HandlerThread thread = new HandlerThread("bridge-camera");
        thread.start();
        Handler handler = new Handler(thread.getLooper());

        ImageReader reader = ImageReader.newInstance(
                size.getWidth(), size.getHeight(), ImageFormat.YUV_420_888, QUEUE_DEPTH + 1);
        ArrayBlockingQueue<byte[]> queue = new ArrayBlockingQueue<>(QUEUE_DEPTH);
        AtomicReference<String> failure = new AtomicReference<>(null);
        CameraDevice[] deviceHolder = new CameraDevice[1];
        CameraCaptureSession[] sessionHolder = new CameraCaptureSession[1];
        int delivered = 0;
        int[] dropped = new int[1];

        try {
            reader.setOnImageAvailableListener(r -> {
                Image img = null;
                try {
                    img = r.acquireLatestImage();
                    if (img == null) return;
                    byte[] frame = I420.toI420(img);
                    // Drop the oldest rather than block the HAL callback.
                    if (!queue.offer(frame)) {
                        queue.poll();
                        dropped[0]++;
                        queue.offer(frame);
                    }
                } catch (Throwable t) {
                    failure.compareAndSet(null, "frame conversion failed: " + t);
                } finally {
                    if (img != null) img.close();
                }
            }, handler);

            CountDownLatch opened = new CountDownLatch(1);
            try {
                cm.openCamera(id, new CameraDevice.StateCallback() {
                    @Override public void onOpened(CameraDevice camera) {
                        deviceHolder[0] = camera;
                        opened.countDown();
                    }
                    @Override public void onDisconnected(CameraDevice camera) {
                        failure.compareAndSet(null, "camera disconnected");
                        camera.close();
                        opened.countDown();
                    }
                    @Override public void onError(CameraDevice camera, int error) {
                        failure.compareAndSet(null, "camera open error " + error
                                + (error == CameraDevice.StateCallback.ERROR_CAMERA_IN_USE
                                   ? " (in use by another app)" : ""));
                        camera.close();
                        opened.countDown();
                    }
                }, handler);
            } catch (CameraAccessException e) {
                throw new IOException("openCamera failed: " + e.getMessage(), e);
            } catch (SecurityException e) {
                throw new IOException("CAMERA permission not granted to the bridge app"
                        + " -- open the app and allow camera access", e);
            }

            if (!awaitQuietly(opened, OPEN_TIMEOUT_MS)) {
                throw new IOException("camera did not open within " + OPEN_TIMEOUT_MS + "ms");
            }
            if (failure.get() != null) throw new IOException(failure.get());
            CameraDevice device = deviceHolder[0];
            if (device == null) throw new IOException("camera open produced no device");

            CountDownLatch configured = new CountDownLatch(1);
            try {
                device.createCaptureSession(
                        Collections.singletonList(reader.getSurface()),
                        new CameraCaptureSession.StateCallback() {
                            @Override public void onConfigured(CameraCaptureSession session) {
                                sessionHolder[0] = session;
                                configured.countDown();
                            }
                            @Override public void onConfigureFailed(CameraCaptureSession session) {
                                failure.compareAndSet(null, "capture session configuration failed");
                                configured.countDown();
                            }
                        }, handler);
            } catch (CameraAccessException e) {
                throw new IOException("createCaptureSession failed: " + e.getMessage(), e);
            }

            if (!awaitQuietly(configured, OPEN_TIMEOUT_MS)) {
                throw new IOException("capture session did not configure within "
                        + OPEN_TIMEOUT_MS + "ms");
            }
            if (failure.get() != null) throw new IOException(failure.get());
            CameraCaptureSession session = sessionHolder[0];
            if (session == null) throw new IOException("capture session was not created");

            try {
                CaptureRequest.Builder req =
                        device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                req.addTarget(reader.getSurface());
                Range<Integer> range = chooseFpsRange(cm, id, fps);
                if (range != null) {
                    req.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, range);
                    logger.log("camera fps range " + range);
                }
                session.setRepeatingRequest(req.build(), null, handler);
            } catch (CameraAccessException e) {
                throw new IOException("setRepeatingRequest failed: " + e.getMessage(), e);
            }

            // Announce the size we actually got before any pixels.
            sink.format(plan.width(), plan.height());

            while (maxFrames == 0 || delivered < maxFrames) {
                byte[] frame;
                try {
                    frame = queue.poll(OPEN_TIMEOUT_MS, TimeUnit.MILLISECONDS);
                } catch (InterruptedException ie) {
                    Thread.currentThread().interrupt();
                    break;
                }
                if (frame == null) {
                    if (failure.get() != null) throw new IOException(failure.get());
                    throw new IOException("camera delivered no frame for "
                            + OPEN_TIMEOUT_MS + "ms");
                }
                if (frame == END) break;
                /*
                 * Rotated here, on the sending thread, rather than in the
                 * ImageReader callback: that callback runs on the HAL's
                 * handler, and a transpose there would hold up the next
                 * frame's delivery. Here it overlaps with the client's read.
                 */
                sink.frame(I420.rotate(frame, size.getWidth(), size.getHeight(), plan.rotation));
                delivered++;
            }
        } finally {
            try { if (sessionHolder[0] != null) sessionHolder[0].close(); } catch (Throwable ignored) {}
            try { if (deviceHolder[0] != null) deviceHolder[0].close(); } catch (Throwable ignored) {}
            try { reader.close(); } catch (Throwable ignored) {}
            thread.quitSafely();
        }

        if (dropped[0] > 0) {
            logger.log("camera: dropped " + dropped[0] + " frame(s) to keep latency down");
        }
        return delivered;
    }

    /** Closest advertised AE range to the requested rate. */
    private static Range<Integer> chooseFpsRange(CameraManager cm, String id, int fps) {
        if (fps <= 0) return null;
        try {
            Range<Integer>[] ranges = cm.getCameraCharacteristics(id)
                    .get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
            if (ranges == null) return null;
            Range<Integer> best = null;
            int bestScore = Integer.MAX_VALUE;
            for (Range<Integer> r : ranges) {
                // Prefer a range whose upper bound is the requested rate, and
                // among those the narrowest, so exposure does not drift the
                // frame rate around under changing light.
                int score = Math.abs(r.getUpper() - fps) * 10 + (r.getUpper() - r.getLower());
                if (score < bestScore) { bestScore = score; best = r; }
            }
            return best;
        } catch (CameraAccessException e) {
            return null;
        }
    }

    private static boolean awaitQuietly(CountDownLatch latch, long ms) {
        try {
            return latch.await(ms, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
    }

    /** Where captured frames go. Kept abstract so tests need no socket. */
    interface FrameSink {
        void format(int width, int height) throws IOException;
        void frame(byte[] data) throws IOException;
    }
}
