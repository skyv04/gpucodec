/* Two handles on /dev/video0 in one process: the pattern Chromium uses when
   it probes a camera's capabilities while another stream already holds it. */
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

int main(void)
{
    int a = open("/dev/video0", O_RDWR);
    printf("primary open -> %d\n", a);
    if (a < 0) return 1;

    struct v4l2_format f; memset(&f,0,sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(a, VIDIOC_G_FMT, &f)) { perror("G_FMT"); return 1; }
    printf("primary G_FMT -> %ux%u\n", f.fmt.pix.width, f.fmt.pix.height);

    struct v4l2_requestbuffers rb; memset(&rb,0,sizeof rb);
    rb.count = 4; rb.type = f.type; rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(a, VIDIOC_REQBUFS, &rb)) { perror("REQBUFS"); return 1; }
    void *m[4];
    for (unsigned i = 0; i < rb.count; i++) {
        struct v4l2_buffer b; memset(&b,0,sizeof b);
        b.type = rb.type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        if (ioctl(a, VIDIOC_QUERYBUF, &b)) { perror("QUERYBUF"); return 1; }
        m[i] = mmap(NULL, b.length, PROT_READ|PROT_WRITE, MAP_SHARED, a, b.m.offset);
        if (m[i] == MAP_FAILED) { perror("mmap"); return 1; }
        if (ioctl(a, VIDIOC_QBUF, &b)) { perror("QBUF"); return 1; }
    }
    int t = f.type;
    if (ioctl(a, VIDIOC_STREAMON, &t)) { perror("STREAMON"); return 1; }
    printf("primary streaming\n");

    /* Now the probe, exactly while the first handle is capturing. */
    int b2 = open("/dev/video0", O_RDWR);
    printf("secondary open -> %d%s\n", b2, b2 < 0 ? strerror(errno) : "");
    if (b2 >= 0) {
        struct v4l2_capability cap; memset(&cap,0,sizeof cap);
        int r = ioctl(b2, VIDIOC_QUERYCAP, &cap);
        printf("secondary QUERYCAP -> %d card=%s\n", r, r ? "-" : (char*)cap.card);
        struct v4l2_fmtdesc fd; memset(&fd,0,sizeof fd);
        fd.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; fd.index = 0;
        r = ioctl(b2, VIDIOC_ENUM_FMT, &fd);
        printf("secondary ENUM_FMT -> %d desc=%s\n", r, r ? "-" : (char*)fd.description);
        struct v4l2_requestbuffers rb2; memset(&rb2,0,sizeof rb2);
        rb2.count = 2; rb2.type = fd.type; rb2.memory = V4L2_MEMORY_MMAP;
        r = ioctl(b2, VIDIOC_REQBUFS, &rb2);
        printf("secondary REQBUFS -> %d (%s) [expect -1 EBUSY]\n", r, r ? strerror(errno) : "ok");
        close(b2);
        printf("secondary closed\n");
    }

    /* The primary must still be streaming after the probe came and went. */
    int got = 0;
    for (int i = 0; i < 30; i++) {
        struct v4l2_buffer d; memset(&d,0,sizeof d);
        d.type = f.type; d.memory = V4L2_MEMORY_MMAP;
        if (ioctl(a, VIDIOC_DQBUF, &d) == 0) { got++; ioctl(a, VIDIOC_QBUF, &d); }
        else break;
    }
    printf("primary frames after probe: %d [expect 30]\n", got);
    ioctl(a, VIDIOC_STREAMOFF, &t);
    close(a);
    return got == 30 ? 0 : 2;
}
