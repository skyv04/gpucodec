/* A duplicated descriptor, which is the pattern GStreamer's v4l2src uses.

   gst_v4l2_buffer_pool_new() dups the device fd and then drives streaming
   through the copy, while format negotiation and buffer allocation happened
   on the original. A dup shares one file description, so both numbers must
   behave as the same handle -- unlike a second open(), which is a different
   description and is deliberately refused at REQBUFS.

   Before dup() was interposed the copy was simply the pipe underneath, so
   this sequence set capture up perfectly and then failed at STREAMON with
   EACCES, which reads like a permissions problem with the camera.

   Also checks the ownership rule that comes with sharing: closing one
   reference must not tear down a capture the other is still using. */
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

static int fail(const char *what)
{
    printf("FAIL: %s: %s\n", what, strerror(errno));
    return 1;
}

int main(void)
{
    void *m[4];
    unsigned i;
    int orig = open("/dev/video0", O_RDWR);
    if (orig < 0) return fail("open");

    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(orig, VIDIOC_G_FMT, &f)) return fail("G_FMT");

    /* Set up on the original, exactly as v4l2src does. */
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof rb);
    rb.count = 4; rb.type = f.type; rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(orig, VIDIOC_REQBUFS, &rb)) return fail("REQBUFS");

    for (i = 0; i < rb.count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        b.type = rb.type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        if (ioctl(orig, VIDIOC_QUERYBUF, &b)) return fail("QUERYBUF");
        m[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                    orig, b.m.offset);
        if (m[i] == MAP_FAILED) return fail("mmap");
        if (ioctl(orig, VIDIOC_QBUF, &b)) return fail("QBUF");
    }

    /* ...and stream on the duplicate. */
    int pool = dup(orig);
    if (pool < 0) return fail("dup");
    if (pool == orig) { printf("FAIL: dup returned the same fd\n"); return 1; }

    int type = (int)f.type;
    if (ioctl(pool, VIDIOC_STREAMON, &type)) return fail("STREAMON on the dup");

    struct v4l2_buffer d;
    memset(&d, 0, sizeof d);
    d.type = f.type; d.memory = V4L2_MEMORY_MMAP;
    if (ioctl(pool, VIDIOC_DQBUF, &d)) return fail("DQBUF on the dup");
    if (d.bytesused == 0) { printf("FAIL: empty frame from the dup\n"); return 1; }
    printf("dup streamed: frame of %u bytes\n", d.bytesused);
    if (ioctl(pool, VIDIOC_QBUF, &d)) return fail("re-QBUF");

    /* Dropping one reference must leave the capture running on the other. */
    close(pool);
    memset(&d, 0, sizeof d);
    d.type = f.type; d.memory = V4L2_MEMORY_MMAP;
    if (ioctl(orig, VIDIOC_DQBUF, &d))
        return fail("DQBUF on the original after closing the dup");
    if (d.bytesused == 0) {
        printf("FAIL: capture died when the duplicate closed\n");
        return 1;
    }
    printf("survived closing the dup: frame of %u bytes\n", d.bytesused);

    if (ioctl(orig, VIDIOC_STREAMOFF, &type)) return fail("STREAMOFF");
    for (i = 0; i < rb.count; i++) munmap(m[i], f.fmt.pix.sizeimage);
    close(orig);

    /* The reverse order matters just as much: keep the duplicate and let
       the original go, which is what a pool outliving its object does. */
    orig = open("/dev/video0", O_RDWR);
    if (orig < 0) return fail("reopen");
    int keep = dup(orig);
    if (keep < 0) return fail("second dup");
    close(orig);
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(keep, VIDIOC_G_FMT, &f))
        return fail("G_FMT on a duplicate that outlived the original");
    printf("duplicate outlived the original: %ux%u\n",
           f.fmt.pix.width, f.fmt.pix.height);
    close(keep);

    printf("PASS: duplicated handles share one capture\n");
    return 0;
}
