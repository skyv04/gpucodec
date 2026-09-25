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
 *   bridge_client encode [-c CODEC] [-r RC] <w> <h> <fps> <bitrate> <in.yuv420> <out.bs>
 *   bridge_client decode [-c CODEC] [<w> <h>] <in.bs> <out.yuv420>
 *   bridge_client info
 *   bridge_client log
 *
 *   CODEC is one of h264 (default), hevc, vp9, av1.
 *   "-" as a filename means stdin/stdout.
 *
 * Environment:
 *   BRIDGE_TIMEOUT   seconds of socket inactivity before giving up (default 30)
 *   BRIDGE_RETRIES   retries after a timeout, for seekable files (default 1)
 *   BRIDGE_PORT      bridge port (default 7878)
 *
 * Exit codes:
 *   0 ok   1 error   2 usage   3 timed out (bridge stalled / app throttled)
 *
 * Wire protocol v5, matching BridgeService.java exactly (see that file for
 * the authoritative spec): big-endian ints, mode/width/height/fps/bitrate/
 * codec handshake, then a status reply carrying either the selected codec
 * name (success) or an error message (failure), then length-prefixed chunks
 * each direction, -1 length = EOS, and -2 = a decode format record carrying
 * the picture size that applies to every frame after it.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define RC_OK 0
#define RC_ERR 1
#define RC_USAGE 2
#define RC_TIMEOUT 3

/*
 * Set whenever a socket operation fails specifically because SO_RCVTIMEO /
 * SO_SNDTIMEO fired, so callers can distinguish "the bridge went quiet"
 * (recoverable, worth retrying) from "the connection was closed" (fatal).
 * Only ever written by a thread that is about to stop doing socket I/O,
 * and read after that thread is joined, so a plain volatile int is enough.
 */
static volatile int io_timed_out = 0;

static int timeout_secs(void) {
    const char *e = getenv("BRIDGE_TIMEOUT");
    int v = e ? atoi(e) : 0;
    return v > 0 ? v : 30;
}

static int retry_count(void) {
    const char *e = getenv("BRIDGE_RETRIES");
    if (!e) return 1;
    int v = atoi(e);
    return v > 0 ? v : 0;
}

static int bridge_port(void) {
    const char *e = getenv("BRIDGE_PORT");
    int v = e ? atoi(e) : 0;
    return v > 0 ? v : 7878;
}

static int write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) io_timed_out = 1;
            return -1;
        }
        if (w == 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) io_timed_out = 1;
            return -1;
        }
        if (r == 0) return -1; /* clean EOF, not a timeout */
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
 * error-message field of the handshake, and for the `info`/`log` payload). */
static char *read_lp_string(int fd) {
    int32_t len;
    if (read_i32(fd, &len) != 0 || len < 0) return NULL;
    char *s = malloc((size_t)len + 1);
    if (!s) return NULL;
    if (len > 0 && read_all(fd, s, (size_t)len) != 0) { free(s); return NULL; }
    s[len] = '\0';
    return s;
}

/* ---------------------------------------------------------------- codecs */

static const struct { const char *name; int id; } CODECS[] = {
    {"h264", 0}, {"avc", 0},
    {"hevc", 1}, {"h265", 1},
    {"vp9", 2},
    {"av1", 3},
    {NULL, 0}
};

static int codec_id_for(const char *name) {
    for (int i = 0; CODECS[i].name; i++)
        if (!strcmp(CODECS[i].name, name)) return CODECS[i].id;
    return -1;
}

/*
 * Rate control, encode only, new in protocol v5. It travels in the high
 * byte of the codec field, so a v3/v4 bridge -- which masks nothing and
 * compares the whole int against 0..3 -- rejects it loudly instead of
 * silently ignoring it.
 */
static int rc_mode_for(const char *name) {
    if (!strcmp(name, "cbr")) return 0;
    if (!strcmp(name, "vbr")) return 1;
    if (!strcmp(name, "cq"))  return 2;
    return -1;
}

/* ------------------------------------------------------------ NAL parsing */

/*
 * Index of the next Annex-B start code at or after `off`, or -1.
 * Checks the 4-byte form first at each position so a 00 00 00 01 code is
 * reported at its true start rather than one byte in.
 */
static long sc_find(const uint8_t *b, size_t len, size_t off) {
    for (size_t i = off; i + 3 <= len; i++) {
        if (b[i] != 0 || b[i + 1] != 0) continue;
        if (i + 4 <= len && b[i + 2] == 0 && b[i + 3] == 1) return (long)i;
        if (b[i + 2] == 1) return (long)i;
    }
    return -1;
}

/* ------------------------------------------------------------- the writer */

struct writer_args {
    int sock;
    int mode;
    int width, height;
    FILE *fin;
    long sent;   /* filled in by writer_thread */
    int failed;  /* nonzero if a socket write failed */
};

static int send_unit(struct writer_args *a, const uint8_t *p, size_t n) {
    if (write_i32(a->sock, (int32_t)n) != 0) return -1;
    if (write_all(a->sock, p, n) != 0) return -1;
    a->sent++;
    return 0;
}

static void *writer_thread(void *arg) {
    struct writer_args *a = arg;

    if (a->mode == 0) {
        size_t frame_size = (size_t)a->width * (size_t)a->height * 3 / 2;
        uint8_t *buf = malloc(frame_size);
        if (!buf) { a->failed = 1; return NULL; }
        size_t r;
        while ((r = fread(buf, 1, frame_size, a->fin)) == frame_size) {
            if (send_unit(a, buf, r) != 0) { a->failed = 1; break; }
        }
        free(buf);
    } else {
        /*
         * Split Annex-B (00 00 00 01 / 00 00 01 start codes) into individual
         * NAL units, one per input buffer -- MediaCodec's decoder expects
         * access-unit framing, not one giant blob. Sending the whole
         * elementary stream as a single unit was tried first and produced
         * only a fraction of the expected output (observed: 110528 bytes
         * instead of the expected 2592000) -- this fixes that.
         *
         * This is a *streaming* splitter: it holds at most one NAL unit
         * plus a read chunk in memory. The previous version slurped the
         * entire elementary stream into RAM first, which put a hard ceiling
         * on clip length (a long 4K stream would simply OOM the container)
         * and defeated pipelining, since nothing was sent until the whole
         * input had been read. It also works on non-seekable input (a pipe
         * straight from ffmpeg), which is why it grows a buffer rather than
         * using fseek/ftell.
         */
        size_t cap = 1u << 20, len = 0, cur = 0;
        uint8_t *buf = malloc(cap);
        int have_cur = 0, eof = 0;
        if (!buf) { a->failed = 1; return NULL; }

        for (;;) {
            if (!have_cur) {
                long p = sc_find(buf, len, 0);
                if (p >= 0) {
                    /* Discard any leading garbage before the first NAL. */
                    memmove(buf, buf + p, len - (size_t)p);
                    len -= (size_t)p;
                    cur = 0;
                    have_cur = 1;
                } else if (eof) {
                    break;
                } else if (len > 3) {
                    /* Keep only a possible split start code across the seam. */
                    memmove(buf, buf + len - 3, 3);
                    len = 3;
                }
            }

            if (have_cur) {
                long next = sc_find(buf, len, cur + 3);
                if (next >= 0) {
                    if (send_unit(a, buf + cur, (size_t)next - cur) != 0) {
                        a->failed = 1;
                        break;
                    }
                    memmove(buf, buf + next, len - (size_t)next);
                    len -= (size_t)next;
                    cur = 0;
                    continue; /* more may already be buffered */
                }
                if (eof) {
                    if (len > cur && send_unit(a, buf + cur, len - cur) != 0)
                        a->failed = 1;
                    break;
                }
            }

            if (len == cap) {
                size_t ncap = cap * 2;
                uint8_t *nb = realloc(buf, ncap);
                if (!nb) { a->failed = 1; break; }
                buf = nb;
                cap = ncap;
            }
            size_t r = fread(buf + len, 1, cap - len, a->fin);
            if (r == 0) eof = 1;
            len += r;
        }
        free(buf);
    }

    /* Always try to send EOS: without it the server waits forever. */
    if (!a->failed && write_i32(a->sock, -1) != 0) a->failed = 1;
    return NULL;
}

/* ------------------------------------------------------------- connection */

static int connect_bridge(void) {
    int port = bridge_port();
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }

    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /*
     * Without these the client hangs forever if the bridge app stalls --
     * which it demonstrably can, e.g. when Android throttles the app in the
     * background, or (before the server-side semaphore fix) when Codec2's
     * max-concurrent-instance cap made configure() block. An indefinite
     * hang is the worst possible failure mode for something that gets
     * called from scripts and pipelines, so bound every socket operation
     * and report a distinct exit code when the bound is hit.
     */
    struct timeval tv;
    tv.tv_sec = timeout_secs();
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr,
            "connect to 127.0.0.1:%d failed: %s\n"
            "Is the SELinux Hardware Bridge app installed and open on-screen?\n"
            "(It must stay open/foregrounded; it does not run as a background daemon.)\n",
            port, strerror(errno));
        close(sock);
        return -1;
    }
    return sock;
}

static void timeout_hint(void) {
    fprintf(stderr,
        "bridge stalled: no data for %ds.\n"
        "The app is probably backgrounded or being throttled by Android --\n"
        "bring SELinux Hardware Bridge to the foreground and retry.\n"
        "(raise the limit with BRIDGE_TIMEOUT=<seconds> if the clip is very large)\n",
        timeout_secs());
}

/* ---------------------------------------------------------------- session */

struct session {
    int mode;
    int width, height, fps, bitrate;
    int codec;
    int rc_mode;
    const char *infile, *outfile;
};

/* Drains a text payload (modes 2 and 3) to stdout. */
static int run_text_session(int sock) {
    for (;;) {
        int32_t olen;
        if (read_i32(sock, &olen) != 0) {
            if (io_timed_out) return RC_TIMEOUT;
            break;
        }
        if (olen < 0) break;
        if (olen > 0) {
            char *buf = malloc((size_t)olen + 1);
            if (!buf) return RC_ERR;
            if (read_all(sock, buf, (size_t)olen) != 0) {
                free(buf);
                return io_timed_out ? RC_TIMEOUT : RC_ERR;
            }
            buf[olen] = '\0';
            fputs(buf, stdout);
            free(buf);
        }
    }
    fflush(stdout);
    return RC_OK;
}

static int run_session(const struct session *s) {
    io_timed_out = 0;

    int sock = connect_bridge();
    if (sock < 0) return RC_ERR;

    if (write_i32(sock, s->mode) != 0 ||
        write_i32(sock, s->width) != 0 ||
        write_i32(sock, s->height) != 0 ||
        write_i32(sock, s->fps) != 0 ||
        write_i32(sock, s->bitrate) != 0 ||
        write_i32(sock, s->codec | (s->rc_mode << 8)) != 0) {
        fprintf(stderr, "failed to send handshake\n");
        close(sock);
        return io_timed_out ? RC_TIMEOUT : RC_ERR;
    }

    int32_t status;
    if (read_i32(sock, &status) != 0) {
        if (io_timed_out) { timeout_hint(); close(sock); return RC_TIMEOUT; }
        fprintf(stderr, "lost connection during handshake\n");
        close(sock);
        return RC_ERR;
    }

    char *reply = read_lp_string(sock);
    if (status != 0) {
        fprintf(stderr, "server rejected request: %s\n", reply ? reply : "(no message)");
        free(reply);
        close(sock);
        return RC_ERR;
    }
    if (s->mode == 2 || s->mode == 3) {
        fprintf(stderr, "connected (bridge is alive)\n");
    } else {
        fprintf(stderr, "handshake ok, codec selected on-device: %s\n", reply ? reply : "?");
    }
    free(reply);

    if (s->mode == 2 || s->mode == 3) {
        int rc = run_text_session(sock);
        if (rc == RC_TIMEOUT) timeout_hint();
        close(sock);
        return rc;
    }

    /* "-" means stdin/stdout, mirroring the agc-* tool conventions, so this
     * can be piped straight from/to ffmpeg without touching a temp file. */
    FILE *fin = !strcmp(s->infile, "-") ? stdin : fopen(s->infile, "rb");
    if (!fin) { perror(s->infile); close(sock); return RC_ERR; }
    FILE *fout = !strcmp(s->outfile, "-") ? stdout : fopen(s->outfile, "wb");
    if (!fout) {
        perror(s->outfile);
        if (fin != stdin) fclose(fin);
        close(sock);
        return RC_ERR;
    }

    struct writer_args wargs = { .sock = sock, .mode = s->mode,
                                 .width = s->width, .height = s->height,
                                 .fin = fin, .sent = 0, .failed = 0 };
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
    int rc = RC_OK;
    /* Picture size most recently announced by a v4 format record. */
    int det_w = 0, det_h = 0;
    for (;;) {
        int32_t olen;
        if (read_i32(sock, &olen) != 0) {
            if (io_timed_out) {
                timeout_hint();
                rc = RC_TIMEOUT;
            } else {
                fprintf(stderr, "connection dropped mid-stream (bridge app closed/killed?)\n");
                rc = RC_ERR;
            }
            break;
        }
        if (olen == -2) {
            /*
             * Protocol v4 format record. Decoding is the only case where the
             * client may not know the picture size up front -- the bitstream
             * carries it, not the caller -- and the size can legitimately
             * change part-way through a stream.
             */
            int32_t fw, fh;
            if (read_i32(sock, &fw) != 0 || read_i32(sock, &fh) != 0) {
                rc = io_timed_out ? RC_TIMEOUT : RC_ERR;
                if (rc == RC_TIMEOUT) timeout_hint();
                break;
            }
            if (fw != det_w || fh != det_h) {
                fprintf(stderr, "%s frame size: %dx%d\n",
                        det_w ? "new" : "detected", fw, fh);
                det_w = fw;
                det_h = fh;
            }
            continue;
        }
        if (olen == -1) break;
        if (olen < -2) {
            fprintf(stderr, "protocol error: unexpected record %d "
                            "(client too old for this bridge?)\n", olen);
            rc = RC_ERR;
            break;
        }
        if (olen > 0) {
            uint8_t *ob = malloc((size_t)olen);
            if (!ob) { rc = RC_ERR; break; }
            if (read_all(sock, ob, (size_t)olen) != 0) {
                free(ob);
                rc = io_timed_out ? RC_TIMEOUT : RC_ERR;
                if (rc == RC_TIMEOUT) timeout_hint();
                break;
            }
            fwrite(ob, 1, (size_t)olen, fout);
            free(ob);
            got++;
        }
    }

    /*
     * Shut the socket down before joining: if we bailed out on a timeout the
     * writer may still be blocked in write(), and without this the join
     * would reintroduce exactly the indefinite hang this change removes.
     */
    if (rc != RC_OK) shutdown(sock, SHUT_RDWR);
    pthread_join(writer, NULL);

    if (rc == RC_OK && wargs.failed) {
        fprintf(stderr, "input stream to bridge failed\n");
        rc = io_timed_out ? RC_TIMEOUT : RC_ERR;
    }

    fprintf(stderr, "done: sent=%ld units, received=%ld units\n", wargs.sent, got);
    if (s->mode == 1 && det_w > 0)
        fprintf(stderr, "output is tightly packed I420 at %dx%d\n", det_w, det_h);
    fflush(fout);
    if (fin != stdin) fclose(fin);
    if (fout != stdout) fclose(fout);
    close(sock);
    return rc;
}

/* ------------------------------------------------------------------- main */

static void usage(const char *prog) {
    fprintf(stderr,
        "usage:\n"
        "  %s encode [-c CODEC] [-r RC] <w> <h> <fps> <bitrate> <in.yuv420> <out.bs>\n"
        "  %s decode [-c CODEC] [<w> <h>] <in.bs> <out.yuv420>\n"
        "  %s info\n"
        "  %s log\n"
        "\n"
        "  CODEC  h264 (default) | hevc | vp9 | av1\n"
        "  RC     cbr (default) | vbr | cq   (encode only)\n"
        "         cbr holds the requested bitrate; vbr treats it as an\n"
        "         average and can overshoot by ~30%%; cq reads <bitrate>\n"
        "         as a quality in 1..100 instead\n"
        "  \"-\"    as a filename means stdin/stdout\n"
        "  decode dimensions are optional: the bridge announces the real\n"
        "  picture size, and reports it again if it changes mid-stream\n"
        "\n"
        "env: BRIDGE_TIMEOUT=%d  BRIDGE_RETRIES=%d  BRIDGE_PORT=%d\n",
        prog, prog, prog, prog, timeout_secs(), retry_count(), bridge_port());
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return RC_USAGE; }

    /* A bridge that dies or hangs up mid-session must surface as a normal
     * write error on the next send, not as SIGPIPE killing us with signal 13
     * before the reader has had a chance to report why. */
    signal(SIGPIPE, SIG_IGN);

    struct session s;
    memset(&s, 0, sizeof(s));
    s.codec = 0;

    const char *sub = argv[1];
    int ai = 2;

    /* Flags between the subcommand and its positional args, in any order. */
    while (ai + 1 < argc && argv[ai][0] == '-' && argv[ai][1] != '\0'
           && strcmp(argv[ai], "-")) {
        if (!strcmp(argv[ai], "-c")) {
            s.codec = codec_id_for(argv[ai + 1]);
            if (s.codec < 0) {
                fprintf(stderr, "unknown codec '%s' (want h264, hevc, vp9 or av1)\n",
                        argv[ai + 1]);
                return RC_USAGE;
            }
        } else if (!strcmp(argv[ai], "-r")) {
            s.rc_mode = rc_mode_for(argv[ai + 1]);
            if (s.rc_mode < 0) {
                fprintf(stderr, "unknown rate control '%s' (want cbr, vbr or cq)\n",
                        argv[ai + 1]);
                return RC_USAGE;
            }
        } else {
            fprintf(stderr, "unknown flag '%s'\n", argv[ai]);
            usage(argv[0]);
            return RC_USAGE;
        }
        ai += 2;
    }
    int rest = argc - ai;

    if (!strcmp(sub, "encode")) {
        if (rest != 6) { fprintf(stderr, "encode needs 6 args\n"); usage(argv[0]); return RC_USAGE; }
        s.mode = 0;
        s.width = atoi(argv[ai]); s.height = atoi(argv[ai + 1]);
        s.fps = atoi(argv[ai + 2]); s.bitrate = atoi(argv[ai + 3]);
        s.infile = argv[ai + 4]; s.outfile = argv[ai + 5];
    } else if (!strcmp(sub, "decode")) {
        /*
         * The dimensions are optional for decoding: protocol v4 has the
         * bridge announce the real picture size, so the common case is to
         * just name the two files and let the bitstream speak for itself.
         */
        s.mode = 1;
        if (rest == 4) {
            s.width = atoi(argv[ai]); s.height = atoi(argv[ai + 1]);
            s.infile = argv[ai + 2]; s.outfile = argv[ai + 3];
        } else if (rest == 2) {
            s.width = 0; s.height = 0;
            s.infile = argv[ai]; s.outfile = argv[ai + 1];
        } else {
            fprintf(stderr, "decode needs 2 or 4 args\n"); usage(argv[0]); return RC_USAGE;
        }
    } else if (!strcmp(sub, "info")) {
        s.mode = 2;
    } else if (!strcmp(sub, "log")) {
        s.mode = 3;
    } else {
        fprintf(stderr, "unknown mode '%s'\n", sub);
        usage(argv[0]);
        return RC_USAGE;
    }

    if (s.mode == 0 && (s.width <= 0 || s.height <= 0)) {
        fprintf(stderr, "width and height must be positive\n");
        return RC_USAGE;
    }
    if (s.mode == 1 && (s.width < 0 || s.height < 0)) {
        fprintf(stderr, "width and height must not be negative\n");
        return RC_USAGE;
    }

    /*
     * A timeout is the one failure worth retrying automatically: the stall
     * observed during stress testing was transient (one hang in ~80
     * sessions, and the identical command then ran normally). Only retry
     * when the streams are real files -- stdin cannot be rewound, so a
     * retry there would silently produce truncated output.
     */
    int retries = retry_count();
    int streaming_stdio = (s.mode == 0 || s.mode == 1) &&
                          (!strcmp(s.infile, "-") || !strcmp(s.outfile, "-"));
    if (streaming_stdio) retries = 0;

    int rc = RC_ERR;
    for (int attempt = 0; attempt <= retries; attempt++) {
        if (attempt > 0)
            fprintf(stderr, "retrying (attempt %d of %d)...\n", attempt + 1, retries + 1);
        rc = run_session(&s);
        if (rc != RC_TIMEOUT) break;
    }
    return rc;
}
