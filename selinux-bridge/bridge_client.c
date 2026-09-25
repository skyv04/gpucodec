/*
 * bridge_client.c — talks to the on-device SELinux Hardware Bridge APK
 * (selinux-bridge/) over loopback TCP to reach real Qualcomm hardware
 * MediaCodec encode/decode from the Debian/PRoot side.
 *
 * This exists to answer, conclusively, whether a real installed APK
 * (with a normal Android app UID/SELinux domain) can do what the
 * Termux/PRoot shell cannot: allocate DMA-BUF buffers for Codec2.
 * See README.md for full context and the verified test transcript.
 *
 * Usage:
 *   bridge_client encode <width> <height> <fps> <bitrate> <in.yuv420> <out.h264>
 *   bridge_client decode <width> <height> <in.h264> <out.yuv420>
 *   bridge_client info
 *
 * Wire protocol v2, matching BridgeService.java exactly (see that file for
 * the authoritative spec): big-endian ints, mode/width/height/fps/bitrate
 * handshake, then a status reply carrying either the selected codec name
 * (success) or an error message (failure), then length-prefixed chunks
 * each direction, -1 length = EOS.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_i32(int fd, int32_t v) {
    int32_t be = (int32_t)htonl((uint32_t)v);
    return write_all(fd, &be, 4);
}

static int read_i32(int fd, int32_t *v) {
    int32_t be;
    if (read_all(fd, &be, 4) != 0) return -1;
    *v = (int32_t)ntohl((uint32_t)be);
    return 0;
}

/* Reads a length-prefixed UTF-8 string reply (used for the codec-name /
 * error-message field of the handshake, and for the `info` payload). */
static char *read_lp_string(int fd) {
    int32_t len;
    if (read_i32(fd, &len) != 0 || len < 0) return NULL;
    char *s = malloc((size_t)len + 1);
    if (len > 0 && read_all(fd, s, (size_t)len) != 0) { free(s); return NULL; }
    s[len] = '\0';
    return s;
}

struct writer_args {
    int sock;
    int mode;
    int width, height;
    FILE *fin;
    long sent; /* filled in by writer_thread */
};

static void *writer_thread(void *arg) {
    struct writer_args *a = arg;
    if (a->mode == 0) {
        size_t frame_size = (size_t)(a->width * a->height * 3 / 2);
        uint8_t *buf = malloc(frame_size);
        size_t r;
        while ((r = fread(buf, 1, frame_size, a->fin)) == frame_size) {
            write_i32(a->sock, (int32_t)r);
            write_all(a->sock, buf, r);
            a->sent++;
        }
        write_i32(a->sock, -1); /* EOS */
        free(buf);
    } else {
        /* Split Annex-B (00 00 00 01 / 00 00 01 start codes) into individual
         * NAL units, one per input buffer -- MediaCodec's decoder expects
         * access-unit framing, not one giant blob. Sending the whole
         * elementary stream as a single unit was tried first and produced
         * only a fraction of the expected output (observed: 110528 bytes
         * instead of the expected 2592000) -- this fixes that. */
        /* Read the whole elementary stream into memory. Grown dynamically
         * (rather than fseek+ftell to size it up front) so this also works
         * when fin is a pipe (e.g. piped straight from ffmpeg), which isn't
         * seekable. */
        size_t cap = 1 << 20, sz = 0;
        uint8_t *buf = malloc(cap);
        size_t r;
        while ((r = fread(buf + sz, 1, cap - sz, a->fin)) > 0) {
            sz += r;
            if (sz == cap) { cap *= 2; buf = realloc(buf, cap); }
        }

        long i = 0;
        while (i < sz) {
            long start = i;
            long nal_begin;
            if (i + 4 <= sz && buf[i]==0 && buf[i+1]==0 && buf[i+2]==0 && buf[i+3]==1)
                nal_begin = i + 4;
            else if (i + 3 <= sz && buf[i]==0 && buf[i+1]==0 && buf[i+2]==1)
                nal_begin = i + 3;
            else { i++; continue; }

            long j = nal_begin;
            long next = sz;
            while (j + 3 <= sz) {
                if (buf[j]==0 && buf[j+1]==0 && (buf[j+2]==1 || (j+4<=sz && buf[j+2]==0 && buf[j+3]==1))) {
                    next = j;
                    break;
                }
                j++;
            }

            long len = next - start;
            write_i32(a->sock, (int32_t)len);
            write_all(a->sock, buf + start, len);
            a->sent++;
            i = next;
        }
        write_i32(a->sock, -1);
        free(buf);
    }
    return NULL;
}

static int connect_bridge(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(7878);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr,
            "connect to 127.0.0.1:7878 failed: %s\n"
            "Is the SELinux Hardware Bridge app installed and open on-screen?\n"
            "(It must stay open/foregrounded; it does not run as a background daemon.)\n",
            strerror(errno));
        close(sock);
        return -1;
    }
    return sock;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage:\n"
            "  %s encode <w> <h> <fps> <bitrate> <in.yuv420> <out.h264>\n"
            "  %s decode <w> <h> <in.h264> <out.yuv420>\n"
            "  %s info\n",
            argv[0], argv[0], argv[0]);
        return 2;
    }

    int mode;
    int width = 0, height = 0, fps = 0, bitrate = 0;
    const char *infile = NULL, *outfile = NULL;

    if (!strcmp(argv[1], "encode")) {
        if (argc != 8) { fprintf(stderr, "encode needs 6 args\n"); return 2; }
        mode = 0;
        width = atoi(argv[2]); height = atoi(argv[3]);
        fps = atoi(argv[4]); bitrate = atoi(argv[5]);
        infile = argv[6]; outfile = argv[7];
    } else if (!strcmp(argv[1], "decode")) {
        if (argc != 6) { fprintf(stderr, "decode needs 4 args\n"); return 2; }
        mode = 1;
        width = atoi(argv[2]); height = atoi(argv[3]);
        infile = argv[4]; outfile = argv[5];
    } else if (!strcmp(argv[1], "info")) {
        mode = 2;
    } else {
        fprintf(stderr, "unknown mode '%s'\n", argv[1]);
        return 2;
    }

    int sock = connect_bridge();
    if (sock < 0) return 1;

    write_i32(sock, mode);
    write_i32(sock, width);
    write_i32(sock, height);
    write_i32(sock, fps);
    write_i32(sock, bitrate);

    int32_t status;
    if (read_i32(sock, &status) != 0) {
        fprintf(stderr, "lost connection during handshake\n");
        return 1;
    }
    char *reply = read_lp_string(sock);
    if (status != 0) {
        fprintf(stderr, "server rejected request: %s\n", reply ? reply : "(no message)");
        free(reply);
        close(sock);
        return 1;
    }
    if (mode == 2) {
        fprintf(stderr, "connected (bridge is alive)\n");
    } else {
        fprintf(stderr, "handshake ok, codec selected on-device: %s\n", reply ? reply : "?");
    }
    free(reply);

    if (mode == 2) {
        int32_t olen;
        for (;;) {
            if (read_i32(sock, &olen) != 0) break;
            if (olen < 0) break;
            char *buf = malloc((size_t)olen + 1);
            if (olen > 0) read_all(sock, buf, (size_t)olen);
            buf[olen] = '\0';
            fputs(buf, stdout);
            free(buf);
        }
        close(sock);
        return 0;
    }

    /* "-" means stdin/stdout, mirroring the agc-* tool conventions, so this
     * can be piped straight from/to ffmpeg without touching a temp file. */
    FILE *fin = !strcmp(infile, "-") ? stdin : fopen(infile, "rb");
    FILE *fout = !strcmp(outfile, "-") ? stdout : fopen(outfile, "wb");
    if (!fin || !fout) { perror("fopen"); return 1; }

    struct writer_args wargs = { .sock = sock, .mode = mode,
                                  .width = width, .height = height, .fin = fin };
    pthread_t writer;
    /*
     * MediaCodec has algorithmic lookahead: it can accept several input
     * frames before it emits the first output unit. A strictly
     * request/response client (write frame, wait for its reply, repeat)
     * deadlocks here — server tries to read the next input length while
     * the client is blocked waiting for output that hasn't been produced
     * yet. Fixed by decoupling: a writer thread streams all input
     * independently (ending with the -1 EOS marker), while the main
     * thread concurrently drains output as it arrives. This was an actual
     * observed hang against the real device, not a hypothetical.
     */
    pthread_create(&writer, NULL, writer_thread, &wargs);

    long got = 0;
    for (;;) {
        int32_t olen;
        if (read_i32(sock, &olen) != 0) {
            fprintf(stderr, "connection dropped mid-stream (bridge app closed/killed?)\n");
            break;
        }
        if (olen < 0) break;
        if (olen > 0) {
            uint8_t *ob = malloc(olen);
            read_all(sock, ob, olen);
            fwrite(ob, 1, olen, fout);
            free(ob);
            got++;
        }
    }

    pthread_join(writer, NULL);
    fprintf(stderr, "done: sent=%ld units, received=%ld units\n", wargs.sent, got);
    fclose(fin);
    fclose(fout);
    close(sock);
    return 0;
}
