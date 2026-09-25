package com.selinuxbridge.app;

import android.graphics.Rect;
import android.media.Image;

import java.nio.ByteBuffer;

/**
 * Conversion between tightly packed I420 (what the wire protocol carries) and
 * whatever layout MediaCodec actually hands out.
 *
 * This is not a formality. Requesting COLOR_FormatYUV420Flexible does not mean
 * the buffer is planar I420: on Qualcomm the chroma comes back semi-planar, so
 * U and V share one region with a pixel stride of 2, and rows are padded to an
 * alignment that has nothing to do with the picture width. Copying the wire
 * bytes straight into the input buffer therefore produces a perfect luma plane
 * and completely wrong colour -- which is easy to miss, because every frame
 * count, every bitrate and every PSNR-on-Y check still looks healthy.
 *
 * Kept free of MediaCodec so it can be unit tested on a normal JVM; see
 * tools/test-i420.
 */
final class I420 {

    private I420() {}

    /** Copies one tightly packed plane into a codec plane of any layout. */
    static void writePlane(Image.Plane plane, byte[] src, int srcOff, int w, int h) {
        ByteBuffer buf = plane.getBuffer();
        int rowStride = plane.getRowStride();
        int pixStride = plane.getPixelStride();

        if (pixStride == 1 && rowStride == w) {
            buf.position(0);
            buf.put(src, srcOff, w * h);
            return;
        }
        if (pixStride == 1) {
            for (int y = 0; y < h; y++) {
                buf.position(y * rowStride);
                buf.put(src, srcOff + y * w, w);
            }
            return;
        }
        /*
         * Semi-planar: absolute puts, because the U and V plane buffers alias
         * the same memory one byte apart and a relative put on one would move
         * a position the other also depends on.
         */
        for (int y = 0; y < h; y++) {
            int row = y * rowStride, sr = srcOff + y * w;
            for (int x = 0; x < w; x++) {
                buf.put(row + x * pixStride, src[sr + x]);
            }
        }
    }

    /** Reads one codec plane back out as tightly packed bytes. */
    static void readPlane(Image.Plane plane, int left, int top, int w, int h,
                          byte[] dst, int dstOff) {
        ByteBuffer buf = plane.getBuffer();
        int rowStride = plane.getRowStride();
        int pixStride = plane.getPixelStride();

        for (int y = 0; y < h; y++) {
            int row = (top + y) * rowStride + left * pixStride;
            int dr = dstOff + y * w;
            if (pixStride == 1) {
                buf.position(row);
                buf.get(dst, dr, w);
            } else {
                for (int x = 0; x < w; x++) {
                    dst[dr + x] = buf.get(row + x * pixStride);
                }
            }
        }
    }

    static int frameSize(int w, int h) {
        int cw = (w + 1) / 2, ch = (h + 1) / 2;
        return w * h + 2 * cw * ch;
    }

    /** Scatters a tightly packed I420 frame into a codec input image. */
    static void fill(Image img, byte[] src, int w, int h) {
        int cw = (w + 1) / 2, ch = (h + 1) / 2;
        Image.Plane[] p = img.getPlanes();
        writePlane(p[0], src, 0, w, h);
        writePlane(p[1], src, w * h, cw, ch);
        writePlane(p[2], src, w * h + cw * ch, cw, ch);
    }

    /** Gathers a codec output image into a tightly packed I420 frame. */
    static byte[] toI420(Image img) {
        Rect crop = img.getCropRect();
        int w = crop.width(), h = crop.height();
        int cw = (w + 1) / 2, ch = (h + 1) / 2;
        byte[] out = new byte[frameSize(w, h)];
        Image.Plane[] p = img.getPlanes();
        readPlane(p[0], crop.left, crop.top, w, h, out, 0);
        readPlane(p[1], crop.left / 2, crop.top / 2, cw, ch, out, w * h);
        readPlane(p[2], crop.left / 2, crop.top / 2, cw, ch, out, w * h + cw * ch);
        return out;
    }

    /**
     * Rotates a tightly packed I420 frame by 0/90/180/270 degrees clockwise.
     *
     * Camera sensors are mounted at whatever angle suited the industrial
     * design, so raw frames come out sideways on most phones; this is what
     * puts them upright. The rotated frame is exactly the same number of
     * bytes, because a quarter turn only swaps the dimensions -- a w x h
     * frame with w/2 x h/2 chroma becomes h x w with h/2 x w/2 chroma.
     *
     * The quarter turns are a transpose, which is the cache-hostile case:
     * consecutive source bytes land a whole output row apart. Walking in
     * tiles keeps both the source row and the destination column slice in
     * cache and is several times faster than the naive loop, which matters
     * because this runs on every frame of a live capture.
     *
     * Requires even dimensions, which every camera size is; an odd one is
     * returned unchanged rather than corrupted.
     */
    static byte[] rotate(byte[] src, int w, int h, int degrees) {
        degrees = ((degrees % 360) + 360) % 360;
        if (degrees == 0 || (w & 1) != 0 || (h & 1) != 0) return src;
        if (degrees != 90 && degrees != 180 && degrees != 270) return src;
        int cw = w / 2, ch = h / 2;
        int uOff = w * h, vOff = uOff + cw * ch;
        byte[] dst = new byte[src.length];
        rotatePlane(src, 0, w, h, dst, 0, degrees);
        rotatePlane(src, uOff, cw, ch, dst, uOff, degrees);
        rotatePlane(src, vOff, cw, ch, dst, vOff, degrees);
        return dst;
    }

    /** One plane of {@link #rotate}; output is h x w for the quarter turns. */
    private static void rotatePlane(byte[] src, int so, int w, int h,
                                    byte[] dst, int dof, int degrees) {
        final int B = 32;
        if (degrees == 180) {
            for (int y = 0; y < h; y++) {
                int si = so + y * w;
                int di = dof + (h - 1 - y) * w + (w - 1);
                for (int x = 0; x < w; x++) dst[di - x] = src[si + x];
            }
            return;
        }
        for (int y0 = 0; y0 < h; y0 += B) {
            int ym = Math.min(y0 + B, h);
            for (int x0 = 0; x0 < w; x0 += B) {
                int xm = Math.min(x0 + B, w);
                for (int y = y0; y < ym; y++) {
                    int si = so + y * w;
                    if (degrees == 90) {
                        // (x, y) -> (h - 1 - y, x) in an h-wide output
                        int db = dof + (h - 1 - y);
                        for (int x = x0; x < xm; x++) dst[db + x * h] = src[si + x];
                    } else {
                        // 270: (x, y) -> (y, w - 1 - x)
                        for (int x = x0; x < xm; x++) dst[dof + (w - 1 - x) * h + y] = src[si + x];
                    }
                }
            }
        }
    }
}
