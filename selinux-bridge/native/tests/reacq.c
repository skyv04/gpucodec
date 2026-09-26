/* Chromium's teardown pattern: one thread blocked in DQBUF while another
   stops the stream and tears the device down, then the whole cycle again. */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

static int vfd;
static volatile int dq_returned;
static int delay_us = 2000000;

static void *dq_thread(void *p)
{
    (void)p;
    for (;;) {
        struct v4l2_buffer d; memset(&d,0,sizeof d);
        d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; d.memory = V4L2_MEMORY_MMAP;
        if (ioctl(vfd, VIDIOC_DQBUF, &d) != 0) break;
        ioctl(vfd, VIDIOC_QBUF, &d);
    }
    dq_returned = 1;
    return NULL;
}

static int cycle(int round)
{
    vfd = open("/dev/video0", O_RDWR);
    if (vfd < 0) { printf("round %d: open failed: %s\n", round, strerror(errno)); return 1; }

    struct v4l2_format f; memset(&f,0,sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(vfd, VIDIOC_G_FMT, &f)) { printf("round %d: G_FMT failed\n", round); return 1; }

    struct v4l2_requestbuffers rb; memset(&rb,0,sizeof rb);
    rb.count = 4; rb.type = f.type; rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(vfd, VIDIOC_REQBUFS, &rb)) { printf("round %d: REQBUFS failed\n", round); return 1; }
    void *m[4]; size_t ml[4];
    for (unsigned i = 0; i < rb.count; i++) {
        struct v4l2_buffer b; memset(&b,0,sizeof b);
        b.type = rb.type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        ioctl(vfd, VIDIOC_QUERYBUF, &b);
        ml[i] = b.length;
        m[i] = mmap(NULL, b.length, PROT_READ|PROT_WRITE, MAP_SHARED, vfd, b.m.offset);
        ioctl(vfd, VIDIOC_QBUF, &b);
    }
    int t = f.type;
    if (ioctl(vfd, VIDIOC_STREAMON, &t)) { printf("round %d: STREAMON failed\n", round); return 1; }

    dq_returned = 0;
    pthread_t th;
    pthread_create(&th, NULL, dq_thread, NULL);
    usleep(delay_us);

    /* Tear down from this thread while dq_thread is inside DQBUF. */
    ioctl(vfd, VIDIOC_STREAMOFF, &t);

    struct timespec a, b2;
    clock_gettime(CLOCK_MONOTONIC, &a);
    pthread_join(th, NULL);
    clock_gettime(CLOCK_MONOTONIC, &b2);
    double ms = (b2.tv_sec - a.tv_sec) * 1000.0 + (b2.tv_nsec - a.tv_nsec) / 1e6;
    printf("round %d: blocked DQBUF returned after STREAMOFF in %.0f ms\n", round, ms);

    for (unsigned i = 0; i < rb.count; i++) munmap(m[i], ml[i]);
    struct v4l2_requestbuffers z; memset(&z,0,sizeof z);
    z.count = 0; z.type = f.type; z.memory = V4L2_MEMORY_MMAP;
    if (ioctl(vfd, VIDIOC_REQBUFS, &z)) { printf("round %d: REQBUFS(0) failed\n", round); return 1; }
    printf("round %d: REQBUFS(0) ok\n", round);
    close(vfd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) delay_us = atoi(argv[1]);
    for (int r = 1; r <= 3; r++) {
        if (cycle(r)) { printf("FAIL at round %d\n", r); return 1; }
        sleep(1);
    }
    printf("PASS: 3 acquire/release cycles\n");
    return 0;
}
