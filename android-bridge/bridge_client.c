/*
 * bridge_client.c — talks to the on-device GPUCodec Bridge APK
 * (android-bridge/) over loopback TCP to reach real Qualcomm hardware
 * MediaCodec encode/decode from the Debian/PRoot side.
 *
 * This exists to answer, conclusively, whether a real installed APK
 * (with a normal Android app UID/SELinux domain) can do what the
 * Termux/PRoot shell cannot: allocate DMA-BUF buffers for Codec2.
 * See README.md "Hardware codec bridge (experimental)" for full context.
 *
 * Usage:
 *   bridge_client encode <width> <height> <fps> <bitrate> <in.yuv420> <out.h264>
 *   bridge_client decode <width> <height> <in.h264> <out.yuv420>
 *
 * Wire protocol matches BridgeService.java exactly (see that file for the
 * authoritative spec): big-endian ints, mode/width/height/fps/bitrate
 * handshake, then length-prefixed chunks each direction, -1 length = EOS.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage:\n"
            "  %s encode <w> <h> <fps> <bitrate> <in.yuv420> <out.h264>\n"
            "  %s decode <w> <h> <in.h264> <out.yuv420>\n",
            argv[0], argv[0]);
        return 2;
    }

    int mode;
    int width, height, fps = 0, bitrate = 0;
    const char *infile, *outfile;

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
    } else {
        fprintf(stderr, "unknown mode '%s'\n", argv[1]);
        return 2;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(7878);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("connect (is the GPUCodec Bridge app open on-screen?)");
        return 1;
    }

    write_i32(sock, mode);
    write_i32(sock, width);
    write_i32(sock, height);
    write_i32(sock, fps);
    write_i32(sock, bitrate);

    int32_t status;
    if (read_i32(sock, &status) != 0 || status != 0) {
        fprintf(stderr, "server rejected handshake (status=%d)\n", status);
        return 1;
    }
    fprintf(stderr, "handshake ok, codec configured on-device\n");

    FILE *fin = fopen(infile, "rb");
    FILE *fout = fopen(outfile, "wb");
    if (!fin || !fout) { perror("fopen"); return 1; }

    size_t frame_size = mode == 0 ? (size_t)(width * height * 3 / 2) : 0;
    /* For decode input we send whole Annex-B access units; for simplicity
     * this client treats the whole input file as one unit per read for
     * encode (raw NV12/I420 frame_size chunks) and as one single blob for
     * decode (a full elementary stream in one shot — fine for short clips
     * used to validate the bridge). */

    long sent = 0, got = 0;
    if (mode == 0) {
        uint8_t *buf = malloc(frame_size);
        size_t r;
        while ((r = fread(buf, 1, frame_size, fin)) == frame_size) {
            write_i32(sock, (int32_t)r);
            write_all(sock, buf, r);
            sent++;

            int32_t olen;
            if (read_i32(sock, &olen) != 0) break;
            if (olen > 0) {
                uint8_t *ob = malloc(olen);
                read_all(sock, ob, olen);
                fwrite(ob, 1, olen, fout);
                free(ob);
                got++;
            }
        }
        write_i32(sock, -1); /* EOS */
        free(buf);
    } else {
        fseek(fin, 0, SEEK_END);
        long sz = ftell(fin);
        fseek(fin, 0, SEEK_SET);
        uint8_t *buf = malloc(sz);
        fread(buf, 1, sz, fin);
        write_i32(sock, (int32_t)sz);
        write_all(sock, buf, sz);
        write_i32(sock, -1);
        sent = 1;
        free(buf);
    }

    /* drain remaining outputs until EOS */
    for (;;) {
        int32_t olen;
        if (read_i32(sock, &olen) != 0) break;
        if (olen < 0) break;
        if (olen > 0) {
            uint8_t *ob = malloc(olen);
            read_all(sock, ob, olen);
            fwrite(ob, 1, olen, fout);
            free(ob);
            got++;
        }
    }

    fprintf(stderr, "done: sent=%ld units, received=%ld units\n", sent, got);
    fclose(fin);
    fclose(fout);
    close(sock);
    return 0;
}
