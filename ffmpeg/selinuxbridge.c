/*
 * SELinux Hardware Bridge — libavcodec encoder and decoder wrapper.
 *
 * Registers `h264_selinuxbridge` and `hevc_selinuxbridge` as real
 * libavcodec encoders *and* decoders that hand frames to Qualcomm's Codec2
 * hardware block through the companion Android app (see
 * selinux-bridge/README.md).
 *
 * Why this exists: the Termux/PRoot container is SELinux-denied
 * access to /dev/dma_heap, so it can never drive Codec2 directly and ffmpeg has no
 * hardware encoder available here at all -- x264 on the CPU is the only
 * option, and it costs roughly 19x more container CPU than the hardware
 * block for the same clip. The companion APK runs in a normal Android app
 * domain where Codec2 *is* permitted, and exposes it over loopback TCP.
 * This file makes that reachable as an ordinary ffmpeg encoder, so
 * anything that drives libavcodec gets hardware offload with no pipeline
 * surgery:
 *
 *     ffmpeg -i in.mp4 -c:v h264_selinuxbridge -b:v 4M out.mp4
 *     ffmpeg -c:v h264_selinuxbridge -i in.mp4 -c:v rawvideo out.yuv
 *
 * These are registered against the existing AV_CODEC_ID_H264 /
 * AV_CODEC_ID_HEVC ids -- the bridge emits bit-exact standard streams, so
 * no new codec id, descriptor or container tag is needed (this follows the
 * same convention as h264_v4l2m2m, h264_nvenc and friends).
 *
 * Threading: MediaCodec has algorithmic lookahead and will accept several
 * input frames before emitting the first output unit, so a strict
 * write-then-read loop deadlocks against it. A reader thread drains the
 * socket into a packet queue while encode2() feeds frames in, mirroring
 * the design already proven in selinux-bridge/bridge_client.c.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "encode.h"
#include "internal.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/thread.h"

#define SB_DEFAULT_PORT    7878
#define SB_DEFAULT_TIMEOUT 30

typedef struct SBPacket {
    uint8_t *data;
    int size;
    /*
     * Protocol v4 format record. Decode sessions interleave these with the
     * output frames so a client learns the stream's real dimensions without
     * parsing the bitstream, and survives a mid-stream resolution change.
     */
    int is_format;
    int fmt_w, fmt_h;
    struct SBPacket *next;
} SBPacket;

typedef struct SBContext {
    AVClass *avclass;

    int port;
    int timeout;

    int sock;
    int codec_id_wire;      /* 0 = h264, 1 = hevc */
    int is_decoder;
    int dec_w, dec_h;       /* dimensions of the last v4 format record */
    char codec_name[128];   /* component the device actually picked */

    pthread_t reader;
    int reader_started;
    pthread_mutex_t lock;
    pthread_cond_t cond;

    SBPacket *head, *tail;
    int reader_done;        /* reader saw EOS or died */
    int reader_error;

    int eos_sent;
    uint8_t *frame_buf;     /* tightly packed I420 scratch */
    size_t frame_size;

    /*
     * Timestamps the bridge cannot give back. The wire protocol carries raw
     * frames and raw access units with no pts, and Codec2 is configured
     * without B-frames, so output order equals input order: queueing each
     * input pts and popping one per emitted packet restores them exactly.
     * Without this the muxer warns "Timestamps are unset" and invents them.
     */
    int64_t *pts_q;
    int pts_cap, pts_head, pts_count;

    /*
     * A codec-config packet carries parameter sets but no picture, so it can
     * never be output on its own.  When the caller did not ask for a global
     * header the stream is Annex-B and the parameter sets have to stay in it,
     * so they are held here and prepended to the next real access unit.
     */
    uint8_t *pend_ps;
    int pend_ps_size;
} SBContext;

static int sb_pts_push(SBContext *s, int64_t pts)
{
    if (s->pts_count == s->pts_cap) {
        int ncap = s->pts_cap ? s->pts_cap * 2 : 64;
        int64_t *nq = av_malloc_array(ncap, sizeof(*nq));
        if (!nq)
            return AVERROR(ENOMEM);
        for (int i = 0; i < s->pts_count; i++)
            nq[i] = s->pts_q[(s->pts_head + i) % s->pts_cap];
        av_free(s->pts_q);
        s->pts_q = nq;
        s->pts_cap = ncap;
        s->pts_head = 0;
    }
    s->pts_q[(s->pts_head + s->pts_count) % s->pts_cap] = pts;
    s->pts_count++;
    return 0;
}

static int sb_pts_pop(SBContext *s, int64_t *pts)
{
    if (!s->pts_count)
        return 0;
    *pts = s->pts_q[s->pts_head];
    s->pts_head = (s->pts_head + 1) % s->pts_cap;
    s->pts_count--;
    return 1;
}

/*
 * Decoding is the one case where popping in submission order is wrong: the
 * bridge hands frames back in *display* order while packets go in in decode
 * order, so a stream with B-frames would get its timestamps permuted. The
 * protocol carries no per-unit timestamp to hand back, but the smallest
 * outstanding pts is by definition the one belonging to the next frame to be
 * displayed, and the component's own reorder delay guarantees that pts has
 * already been submitted by the time the frame appears. For a stream without
 * B-frames this degenerates to plain FIFO order.
 */
static int sb_pts_pop_min(SBContext *s, int64_t *pts)
{
    int best = -1;

    for (int i = 0; i < s->pts_count; i++) {
        int idx = (s->pts_head + i) % s->pts_cap;
        if (s->pts_q[idx] == AV_NOPTS_VALUE)
            continue;
        if (best < 0 || s->pts_q[idx] < s->pts_q[best])
            best = idx;
    }
    if (best < 0)
        return sb_pts_pop(s, pts);

    *pts = s->pts_q[best];
    /* Close the hole by sliding the entries ahead of it back one slot. */
    while (best != s->pts_head) {
        int prev = (best - 1 + s->pts_cap) % s->pts_cap;
        s->pts_q[best] = s->pts_q[prev];
        best = prev;
    }
    s->pts_head = (s->pts_head + 1) % s->pts_cap;
    s->pts_count--;
    return 1;
}

/* ------------------------------------------------------------------ io */

static int sb_write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return AVERROR(errno);
        }
        if (w == 0)
            return AVERROR(EIO);
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int sb_read_all(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return AVERROR(errno);
        }
        if (r == 0)
            return AVERROR_EOF;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int sb_write_i32(int fd, int32_t v)
{
    uint32_t be = htonl((uint32_t)v);
    return sb_write_all(fd, &be, 4);
}

static int sb_read_i32(int fd, int32_t *v)
{
    uint32_t be;
    int ret = sb_read_all(fd, &be, 4);
    if (ret < 0)
        return ret;
    *v = (int32_t)ntohl(be);
    return 0;
}

/* ------------------------------------------------------------- reader */

static void *sb_reader_thread(void *arg)
{
    SBContext *s = arg;

    for (;;) {
        SBPacket *node;
        int32_t len;
        if (sb_read_i32(s->sock, &len) < 0) {
            pthread_mutex_lock(&s->lock);
            s->reader_error = 1;
            break;
        }
        if (len == -2) {
            /*
             * Protocol v4 format record: two more int32s, then the stream
             * continues. Queued rather than applied here so the dimension
             * change lands in the output sequence at exactly the right
             * point relative to the frames around it.
             */
            int32_t fw, fh;
            if (sb_read_i32(s->sock, &fw) < 0 || sb_read_i32(s->sock, &fh) < 0) {
                pthread_mutex_lock(&s->lock);
                s->reader_error = 1;
                break;
            }
            node = av_mallocz(sizeof(*node));
            if (!node) {
                pthread_mutex_lock(&s->lock);
                s->reader_error = 1;
                break;
            }
            node->is_format = 1;
            node->fmt_w = fw;
            node->fmt_h = fh;
            goto enqueue;
        }
        if (len < 0) {
            pthread_mutex_lock(&s->lock);
            break;
        }
        if (len == 0)
            continue;

        node = av_mallocz(sizeof(*node));
        if (!node) {
            pthread_mutex_lock(&s->lock);
            s->reader_error = 1;
            break;
        }
        node->data = av_malloc((size_t)len);
        if (!node->data || sb_read_all(s->sock, node->data, (size_t)len) < 0) {
            av_free(node->data);
            av_free(node);
            pthread_mutex_lock(&s->lock);
            s->reader_error = 1;
            break;
        }
        node->size = len;

enqueue:
        pthread_mutex_lock(&s->lock);
        if (s->tail)
            s->tail->next = node;
        else
            s->head = node;
        s->tail = node;
        pthread_cond_signal(&s->cond);
        pthread_mutex_unlock(&s->lock);
    }

    /* Falls through here still holding the lock from every break above. */
    s->reader_done = 1;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

/* Caller must hold s->lock. */
static SBPacket *sb_pop(SBContext *s)
{
    SBPacket *node = s->head;
    if (!node)
        return NULL;
    s->head = node->next;
    if (!s->head)
        s->tail = NULL;
    node->next = NULL;
    return node;
}

/* --------------------------------------------------------------- init */

static av_cold int sb_connect(AVCodecContext *avctx, SBContext *s)
{
    struct sockaddr_in addr;
    struct timeval tv;
    int one = 1;

    s->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (s->sock < 0)
        return AVERROR(errno);

    setsockopt(s->sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* Bound every socket operation: a stalled bridge app must surface as an
     * encoder error, never as an ffmpeg process that hangs forever. */
    tv.tv_sec  = s->timeout;
    tv.tv_usec = 0;
    setsockopt(s->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)s->port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(s->sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        av_log(avctx, AV_LOG_ERROR,
               "selinuxbridge: cannot reach the bridge on 127.0.0.1:%d (%s).\n"
               "Install the SELinux Hardware Bridge app and leave it open on-screen.\n",
               s->port, strerror(errno));
        close(s->sock);
        s->sock = -1;
        return AVERROR(ECONNREFUSED);
    }
    return 0;
}

static void sb_scan_nals(const uint8_t *buf, int size, int hevc,
                         int *all_param_sets, int *is_key, int *ps_prefix);

/* Handshake: six int32s out, then status + component name back. */
static av_cold int sb_handshake(AVCodecContext *avctx, SBContext *s, int sock,
                                int fps)
{
    int32_t status, nlen;
    char msg[512];
    const int mode = s->is_decoder ? 1 : 0;

    if (sb_write_i32(sock, mode) < 0 ||
        sb_write_i32(sock, avctx->width) < 0 ||
        sb_write_i32(sock, avctx->height) < 0 ||
        sb_write_i32(sock, s->is_decoder ? 0 : fps) < 0 ||
        sb_write_i32(sock, s->is_decoder ? 0
                           : (avctx->bit_rate > 0 ? (int)avctx->bit_rate : 4000000)) < 0 ||
        sb_write_i32(sock, s->codec_id_wire) < 0) {
        av_log(avctx, AV_LOG_ERROR, "selinuxbridge: handshake write failed\n");
        return AVERROR(EIO);
    }

    if (sb_read_i32(sock, &status) < 0) {
        av_log(avctx, AV_LOG_ERROR, "selinuxbridge: handshake read failed\n");
        return AVERROR(EIO);
    }
    if (sb_read_i32(sock, &nlen) < 0 || nlen < 0)
        return AVERROR(EIO);
    if (nlen > (int32_t)sizeof(msg) - 1)
        nlen = sizeof(msg) - 1;
    if (nlen > 0 && sb_read_all(sock, msg, (size_t)nlen) < 0)
        return AVERROR(EIO);
    msg[nlen] = '\0';
    if (status != 0) {
        av_log(avctx, AV_LOG_ERROR, "selinuxbridge: bridge refused: %s\n", msg);
        return AVERROR_EXTERNAL;
    }
    av_strlcpy(s->codec_name, msg, sizeof(s->codec_name));
    return 0;
}

/*
 * Muxers such as mp4 need the parameter sets (avcC/hvcC) when the header is
 * written, which happens before the first frame is ever encoded.  MediaCodec
 * only reveals them after it has been fed, and libavformat's late-extradata
 * side-data path does not cover H.264 or HEVC, so the only way to have them in
 * time is to run a throwaway session at init: one grey frame in, parameter
 * sets out, session closed.  It costs one codec open/close and is skipped
 * unless the caller actually asked for a global header.
 */
static av_cold int sb_probe_extradata(AVCodecContext *avctx, SBContext *s,
                                      int fps)
{
    struct sockaddr_in addr;
    struct timeval tv;
    int sock, one = 1, ret = 0, guard;
    uint8_t *dummy = NULL;
    size_t dummy_size = (size_t)avctx->width * avctx->height * 3 / 2;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return AVERROR(errno);

    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    tv.tv_sec  = s->timeout;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)s->port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return AVERROR(ECONNREFUSED);
    }

    ret = sb_handshake(avctx, s, sock, fps);
    if (ret < 0)
        goto done;

    dummy = av_mallocz(dummy_size);
    if (!dummy) {
        ret = AVERROR(ENOMEM);
        goto done;
    }
    memset(dummy + (size_t)avctx->width * avctx->height, 0x80,
           dummy_size - (size_t)avctx->width * avctx->height);

    if (sb_write_i32(sock, (int32_t)dummy_size) < 0 ||
        sb_write_all(sock, dummy, dummy_size) < 0 ||
        sb_write_i32(sock, -1) < 0) {
        ret = AVERROR(EIO);
        goto done;
    }

    for (guard = 0; guard < 16; guard++) {
        int32_t len;
        uint8_t *buf;
        int all_ps, is_key, ps_prefix;

        if (sb_read_i32(sock, &len) < 0) {
            ret = AVERROR(EIO);
            goto done;
        }
        if (len < 0)
            break;
        if (len == 0)
            continue;
        buf = av_malloc((size_t)len);
        if (!buf) {
            ret = AVERROR(ENOMEM);
            goto done;
        }
        if (sb_read_all(sock, buf, (size_t)len) < 0) {
            av_free(buf);
            ret = AVERROR(EIO);
            goto done;
        }
        sb_scan_nals(buf, len, s->codec_id_wire == 1, &all_ps, &is_key,
                     &ps_prefix);
        if (ps_prefix > 0) {
            avctx->extradata = av_mallocz(ps_prefix + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!avctx->extradata) {
                av_free(buf);
                ret = AVERROR(ENOMEM);
                goto done;
            }
            memcpy(avctx->extradata, buf, ps_prefix);
            avctx->extradata_size = ps_prefix;
            av_free(buf);
            goto done;
        }
        av_free(buf);
    }
    ret = AVERROR_EXTERNAL;

done:
    av_free(dummy);
    close(sock);
    return ret;
}

static av_cold int sb_encode_init(AVCodecContext *avctx)
{
    SBContext *s = avctx->priv_data;
    int fps, ret;

    s->sock = -1;
    s->codec_id_wire = (avctx->codec_id == AV_CODEC_ID_HEVC) ? 1 : 0;

    /* Same precedence as bridge_client: -bridge_port, then $BRIDGE_PORT. */
    if (s->port <= 0) {
        const char *env = getenv("BRIDGE_PORT");
        s->port = env ? atoi(env) : 0;
        if (s->port <= 0 || s->port > 65535)
            s->port = SB_DEFAULT_PORT;
    }

    if (avctx->pix_fmt != AV_PIX_FMT_YUV420P) {
        av_log(avctx, AV_LOG_ERROR, "selinuxbridge: only yuv420p is supported\n");
        return AVERROR(EINVAL);
    }
    if (avctx->width <= 0 || avctx->height <= 0)
        return AVERROR(EINVAL);

    fps = avctx->framerate.num && avctx->framerate.den
        ? av_q2d(avctx->framerate)
        : (avctx->time_base.num ? 1.0 / av_q2d(avctx->time_base) : 30);
    if (fps <= 0)
        fps = 30;

    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
        ret = sb_probe_extradata(avctx, s, fps);
        if (ret < 0)
            av_log(avctx, AV_LOG_WARNING,
                   "selinuxbridge: could not probe parameter sets (%s); the "
                   "muxer may reject the stream\n", av_err2str(ret));
    }

    ret = sb_connect(avctx, s);
    if (ret < 0)
        return ret;

    ret = sb_handshake(avctx, s, s->sock, fps);
    if (ret < 0)
        return ret;

    av_log(avctx, AV_LOG_INFO, "selinuxbridge: using on-device component %s\n",
           s->codec_name);

    s->frame_size = (size_t)avctx->width * avctx->height * 3 / 2;
    s->frame_buf  = av_malloc(s->frame_size);
    if (!s->frame_buf)
        return AVERROR(ENOMEM);

    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    if (pthread_create(&s->reader, NULL, sb_reader_thread, s) != 0) {
        av_log(avctx, AV_LOG_ERROR, "selinuxbridge: cannot start reader thread\n");
        return AVERROR(ENOMEM);
    }
    s->reader_started = 1;
    return 0;
}

static av_cold int sb_close(AVCodecContext *avctx)
{
    SBContext *s = avctx->priv_data;
    SBPacket *node;

    av_freep(&s->pend_ps);
    s->pend_ps_size = 0;

    if (s->sock >= 0 && !s->eos_sent) {
        sb_write_i32(s->sock, -1);
        s->eos_sent = 1;
    }
    if (s->sock >= 0)
        shutdown(s->sock, SHUT_RDWR);

    if (s->reader_started) {
        pthread_join(s->reader, NULL);
        pthread_mutex_destroy(&s->lock);
        pthread_cond_destroy(&s->cond);
        s->reader_started = 0;
    }
    if (s->sock >= 0) {
        close(s->sock);
        s->sock = -1;
    }

    while ((node = s->head)) {
        s->head = node->next;
        av_free(node->data);
        av_free(node);
    }
    s->tail = NULL;
    av_freep(&s->frame_buf);
    av_freep(&s->pts_q);
    return 0;
}

/* ------------------------------------------------------------- encode */

/*
 * Walks the Annex-B NAL units in `buf`, reporting whether every one of them
 * is a parameter set (i.e. the packet is MediaCodec's codec-config blob,
 * which must become avctx->extradata instead of being emitted, or muxers
 * needing a global header have nothing to write) and whether any of them is
 * an IRAP/IDR picture (i.e. the packet is a keyframe).
 *
 * Keyframes have to be detected rather than assumed: the bridge configures
 * KEY_I_FRAME_INTERVAL = 1, which Android measures in *seconds*, not
 * frames, so the stream is emphatically not all-intra and flagging every
 * packet as a keyframe corrupts seeking.
 */
/*
 * Walk the Annex-B NAL units in a buffer and report three things:
 *   all_param_sets - every NAL is a parameter set, so this is a codec-config
 *                    packet rather than a picture
 *   is_key         - the buffer contains an IRAP/IDR slice
 *   ps_prefix      - byte length of the leading run of parameter sets, which is
 *                    what belongs in extradata when the encoder emits them
 *                    in-band ahead of the first IDR instead of as a separate
 *                    BUFFER_FLAG_CODEC_CONFIG packet
 */
static void sb_scan_nals(const uint8_t *buf, int size, int hevc,
                         int *all_param_sets, int *is_key, int *ps_prefix)
{
    int i = 0, seen = 0, only_ps = 1, key = 0, in_prefix = 1, prefix = 0;

    while (i + 3 <= size) {
        int sc, type, nal_start = i, is_ps;
        if (buf[i] != 0 || buf[i + 1] != 0) {
            i++;
            continue;
        }
        if (i + 4 <= size && buf[i + 2] == 0 && buf[i + 3] == 1)
            sc = 4;
        else if (buf[i + 2] == 1)
            sc = 3;
        else {
            i++;
            continue;
        }
        if (i + sc >= size)
            break;

        if (hevc) {
            type = (buf[i + sc] >> 1) & 0x3F;
            is_ps = (type == 32 || type == 33 || type == 34);  /* VPS/SPS/PPS */
            if (type >= 16 && type <= 21)                      /* BLA..CRA */
                key = 1;
        } else {
            type = buf[i + sc] & 0x1F;
            is_ps = (type == 7 || type == 8);                  /* SPS/PPS */
            if (type == 5)                                     /* IDR slice */
                key = 1;
        }
        if (!is_ps) {
            only_ps = 0;
            if (in_prefix) {
                in_prefix = 0;
                prefix = nal_start;
            }
        }
        seen++;
        i += sc;
    }

    *all_param_sets = seen > 0 && only_ps;
    *is_key = key;
    *ps_prefix = (seen > 0 && in_prefix) ? size : prefix;
}

static int sb_send_frame(AVCodecContext *avctx, SBContext *s, const AVFrame *frame)
{
    const int w = avctx->width, h = avctx->height;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    uint8_t *dst = s->frame_buf;
    int ret;

    for (int y = 0; y < h; y++)
        memcpy(dst + (size_t)y * w, frame->data[0] + (size_t)y * frame->linesize[0], w);
    dst += (size_t)w * h;
    for (int y = 0; y < ch; y++)
        memcpy(dst + (size_t)y * cw, frame->data[1] + (size_t)y * frame->linesize[1], cw);
    dst += (size_t)cw * ch;
    for (int y = 0; y < ch; y++)
        memcpy(dst + (size_t)y * cw, frame->data[2] + (size_t)y * frame->linesize[2], cw);

    ret = sb_write_i32(s->sock, (int32_t)s->frame_size);
    if (ret < 0)
        return ret;
    return sb_write_all(s->sock, s->frame_buf, s->frame_size);
}

static int sb_collect(AVCodecContext *avctx, AVPacket *pkt, int *got_packet,
                      int draining)
{
    SBContext *s = avctx->priv_data;
    SBPacket *node;
    int all_ps, is_key, ps_prefix;
    int ret;

    for (;;) {
        pthread_mutex_lock(&s->lock);
        /*
         * While frames are still coming in, never block: the codec is
         * entitled to hold several of them before producing anything. Once
         * the caller is draining there is nothing left to feed the
         * pipeline, so waiting is both safe and required to collect the
         * tail packets.
         */
        while (!s->head && !s->reader_done && draining)
            pthread_cond_wait(&s->cond, &s->lock);
        node = sb_pop(s);
        ret = (!node && s->reader_error) ? AVERROR_EXTERNAL : 0;
        pthread_mutex_unlock(&s->lock);

        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "selinuxbridge: bridge connection failed mid-stream\n");
            return ret;
        }
        if (!node)
            return 0;

        /*
         * MediaCodec emits SPS/PPS as a standalone codec-config packet
         * before the first picture. That belongs in extradata, not in the
         * packet stream, or muxers needing a global header (mp4/mov) have
         * nothing to write. Loop rather than recurse so this never blocks
         * on a fresh packet the caller isn't entitled to wait for.
         */
        sb_scan_nals(node->data, node->size, s->codec_id_wire == 1,
                     &all_ps, &is_key, &ps_prefix);

        /*
         * MediaCodec normally hands the parameter sets over once, in a packet
         * flagged BUFFER_FLAG_CODEC_CONFIG.  Some vendor components instead
         * prepend them to every IRAP access unit.  Handle both by harvesting
         * the leading parameter sets the first time we see them.
         */
        if (!avctx->extradata && ps_prefix > 0) {
            avctx->extradata = av_mallocz(ps_prefix + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!avctx->extradata) {
                av_free(node->data);
                av_free(node);
                return AVERROR(ENOMEM);
            }
            memcpy(avctx->extradata, node->data, ps_prefix);
            avctx->extradata_size = ps_prefix;
        }

        /* A codec-config packet carries no picture, so it must not be output. */
        if (all_ps) {
            if (!(avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER)) {
                av_free(s->pend_ps);
                s->pend_ps = node->data;
                s->pend_ps_size = node->size;
                av_free(node);
                continue;
            }
            av_free(node->data);
            av_free(node);
            continue;
        }

        ret = ff_get_encode_buffer(avctx, pkt, s->pend_ps_size + node->size, 0);
        if (ret < 0) {
            av_free(node->data);
            av_free(node);
            return ret;
        }
        if (s->pend_ps_size) {
            memcpy(pkt->data, s->pend_ps, s->pend_ps_size);
            av_freep(&s->pend_ps);
            s->pend_ps_size = 0;
        }
        memcpy(pkt->data + (pkt->size - node->size), node->data, node->size);
        if (is_key)
            pkt->flags |= AV_PKT_FLAG_KEY;
        /* No B-frames, so output order is input order and dts == pts. */
        if (sb_pts_pop(s, &pkt->pts))
            pkt->dts = pkt->pts;
        av_free(node->data);
        av_free(node);
        *got_packet = 1;
        return 0;
    }
}

static int sb_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                           const AVFrame *frame, int *got_packet)
{
    SBContext *s = avctx->priv_data;
    int ret;

    *got_packet = 0;

    if (frame) {
        ret = sb_pts_push(s, frame->pts);
        if (ret < 0)
            return ret;
        ret = sb_send_frame(avctx, s, frame);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "selinuxbridge: sending a frame failed (%s) -- the bridge app may "
                   "have been backgrounded or closed\n", av_err2str(ret));
            return ret;
        }
    } else if (!s->eos_sent) {
        ret = sb_write_i32(s->sock, -1);
        if (ret < 0)
            return ret;
        s->eos_sent = 1;
    }

    return sb_collect(avctx, pkt, got_packet, frame == NULL);
}

/* ------------------------------------------------------------- decode */

/*
 * Decoding is the mirror image of the encode path and shares all of its
 * plumbing: the same socket, the same reader thread, the same packet queue.
 * The two differences are that the payloads travel the other way round --
 * Annex-B access units in, tightly packed I420 out -- and that the output
 * stream is punctuated by protocol-v4 format records, which is how the
 * decoder learns the picture size without parsing the bitstream itself and
 * how it survives a resolution change mid-stream.
 *
 * Input has to be Annex-B, so the registration below attaches the standard
 * h264_mp4toannexb / hevc_mp4toannexb bitstream filters exactly as
 * h264_mediacodec does; mp4/mov sources are therefore handled transparently.
 */
static av_cold int sb_decode_init(AVCodecContext *avctx)
{
    SBContext *s = avctx->priv_data;
    int ret;

    s->sock = -1;
    s->is_decoder = 1;
    s->codec_id_wire = (avctx->codec_id == AV_CODEC_ID_HEVC) ? 1 : 0;

    if (s->port <= 0) {
        const char *env = getenv("BRIDGE_PORT");
        s->port = env ? atoi(env) : 0;
        if (s->port <= 0 || s->port > 65535)
            s->port = SB_DEFAULT_PORT;
    }

    avctx->pix_fmt = AV_PIX_FMT_YUV420P;

    ret = sb_connect(avctx, s);
    if (ret < 0)
        return ret;

    ret = sb_handshake(avctx, s, s->sock, 0);
    if (ret < 0)
        return ret;

    av_log(avctx, AV_LOG_INFO, "selinuxbridge: using on-device component %s\n",
           s->codec_name);

    /*
     * Until the bridge announces the real size, assume whatever the
     * container claimed. A format record always precedes the first frame,
     * so this is only ever a starting guess.
     */
    s->dec_w = avctx->width;
    s->dec_h = avctx->height;

    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    if (pthread_create(&s->reader, NULL, sb_reader_thread, s) != 0) {
        av_log(avctx, AV_LOG_ERROR, "selinuxbridge: cannot start reader thread\n");
        return AVERROR(ENOMEM);
    }
    s->reader_started = 1;
    return 0;
}

/* Copies one tightly packed I420 frame off the wire into an AVFrame. */
static int sb_unpack_frame(AVCodecContext *avctx, AVFrame *frame,
                           const uint8_t *buf, int size, int w, int h)
{
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    const int64_t want = (int64_t)w * h + 2LL * cw * ch;
    const uint8_t *src = buf;
    int ret;

    if (w <= 0 || h <= 0) {
        av_log(avctx, AV_LOG_ERROR,
               "selinuxbridge: frame received before any format record\n");
        return AVERROR_INVALIDDATA;
    }
    if (size < want) {
        av_log(avctx, AV_LOG_ERROR,
               "selinuxbridge: short frame, got %d bytes, expected %"PRId64
               " for %dx%d\n", size, want, w, h);
        return AVERROR_INVALIDDATA;
    }

    if (w != avctx->width || h != avctx->height) {
        ret = ff_set_dimensions(avctx, w, h);
        if (ret < 0)
            return ret;
    }

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;

    for (int y = 0; y < h; y++)
        memcpy(frame->data[0] + (size_t)y * frame->linesize[0], src + (size_t)y * w, w);
    src += (size_t)w * h;
    for (int y = 0; y < ch; y++)
        memcpy(frame->data[1] + (size_t)y * frame->linesize[1], src + (size_t)y * cw, cw);
    src += (size_t)cw * ch;
    for (int y = 0; y < ch; y++)
        memcpy(frame->data[2] + (size_t)y * frame->linesize[2], src + (size_t)y * cw, cw);

    return 0;
}

static int sb_collect_frame(AVCodecContext *avctx, AVFrame *frame,
                            int *got_frame, int draining)
{
    SBContext *s = avctx->priv_data;
    SBPacket *node;
    int ret;

    for (;;) {
        pthread_mutex_lock(&s->lock);
        while (!s->head && !s->reader_done && draining)
            pthread_cond_wait(&s->cond, &s->lock);
        node = sb_pop(s);
        ret = (!node && s->reader_error) ? AVERROR_EXTERNAL : 0;
        pthread_mutex_unlock(&s->lock);

        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "selinuxbridge: bridge connection failed mid-stream\n");
            return ret;
        }
        if (!node)
            return 0;

        if (node->is_format) {
            if (node->fmt_w != s->dec_w || node->fmt_h != s->dec_h)
                av_log(avctx, AV_LOG_VERBOSE,
                       "selinuxbridge: stream is %dx%d\n", node->fmt_w, node->fmt_h);
            s->dec_w = node->fmt_w;
            s->dec_h = node->fmt_h;
            av_free(node);
            continue;
        }

        ret = sb_unpack_frame(avctx, frame, node->data, node->size,
                              s->dec_w, s->dec_h);
        av_free(node->data);
        av_free(node);
        if (ret < 0)
            return ret;

        if (!sb_pts_pop_min(s, &frame->pts))
            frame->pts = AV_NOPTS_VALUE;
        *got_frame = 1;
        return 0;
    }
}

static int sb_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                           int *got_frame, AVPacket *avpkt)
{
    SBContext *s = avctx->priv_data;
    int ret;

    *got_frame = 0;

    if (avpkt->size > 0) {
        ret = sb_pts_push(s, avpkt->pts);
        if (ret < 0)
            return ret;
        ret = sb_write_i32(s->sock, avpkt->size);
        if (ret >= 0)
            ret = sb_write_all(s->sock, avpkt->data, (size_t)avpkt->size);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "selinuxbridge: sending an access unit failed (%s) -- the bridge "
                   "app may have been backgrounded or closed\n", av_err2str(ret));
            return ret;
        }
    } else if (!s->eos_sent) {
        ret = sb_write_i32(s->sock, -1);
        if (ret < 0)
            return ret;
        s->eos_sent = 1;
    }

    ret = sb_collect_frame(avctx, frame, got_frame, avpkt->size == 0);
    if (ret < 0)
        return ret;
    return avpkt->size;
}

/* ------------------------------------------------------------ registry */

#define OFFSET(x) offsetof(SBContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM

#define SB_COMMON_OPTIONS(flags)                                             \
    { "bridge_port", "TCP port the SELinux Hardware Bridge app listens on",  \
      OFFSET(port), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 65535, flags },        \
    { "bridge_timeout", "seconds of bridge inactivity before failing",       \
      OFFSET(timeout), AV_OPT_TYPE_INT, { .i64 = SB_DEFAULT_TIMEOUT }, 1, 3600, flags }

static const AVOption sb_options[] = {
    SB_COMMON_OPTIONS(VE),
    { NULL }
};

static const AVOption sb_dec_options[] = {
    SB_COMMON_OPTIONS(VD),
    { NULL }
};

static const enum AVPixelFormat sb_pix_fmts[] = {
    AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE
};

#define SB_ENCODER(ctype, CTYPE, desc)                                       \
static const AVClass ctype##_selinuxbridge_class = {                         \
    .class_name = #ctype "_selinuxbridge",                                   \
    .item_name  = av_default_item_name,                                      \
    .option     = sb_options,                                                \
    .version    = LIBAVUTIL_VERSION_INT,                                     \
};                                                                           \
const FFCodec ff_##ctype##_selinuxbridge_encoder = {                         \
    .p.name         = #ctype "_selinuxbridge",                               \
    .p.long_name    = NULL_IF_CONFIG_SMALL(desc),                            \
    .p.type         = AVMEDIA_TYPE_VIDEO,                                    \
    .p.id           = AV_CODEC_ID_##CTYPE,                                   \
    .p.capabilities = AV_CODEC_CAP_DELAY,                 \
    .p.pix_fmts     = sb_pix_fmts,                                           \
    .p.priv_class   = &ctype##_selinuxbridge_class,                          \
    .p.wrapper_name = "selinuxbridge",                                       \
    .priv_data_size = sizeof(SBContext),                                     \
    .init           = sb_encode_init,                                        \
    FF_CODEC_ENCODE_CB(sb_encode_frame),                                     \
    .close          = sb_close,                                       \
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,                             \
};

SB_ENCODER(h264, H264, "H.264 via Qualcomm Codec2 (SELinux Hardware Bridge)")
SB_ENCODER(hevc, HEVC, "HEVC via Qualcomm Codec2 (SELinux Hardware Bridge)")

#define SB_DECODER(ctype, CTYPE, desc, bsf)                                  \
static const AVClass ctype##_selinuxbridge_dec_class = {                     \
    .class_name = #ctype "_selinuxbridge",                                   \
    .item_name  = av_default_item_name,                                      \
    .option     = sb_dec_options,                                            \
    .version    = LIBAVUTIL_VERSION_INT,                                     \
};                                                                           \
const FFCodec ff_##ctype##_selinuxbridge_decoder = {                         \
    .p.name         = #ctype "_selinuxbridge",                               \
    .p.long_name    = NULL_IF_CONFIG_SMALL(desc),                            \
    .p.type         = AVMEDIA_TYPE_VIDEO,                                    \
    .p.id           = AV_CODEC_ID_##CTYPE,                                   \
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING |      \
                      AV_CODEC_CAP_HARDWARE,                                 \
    .p.priv_class   = &ctype##_selinuxbridge_dec_class,                      \
    .p.wrapper_name = "selinuxbridge",                                       \
    .priv_data_size = sizeof(SBContext),                                     \
    .init           = sb_decode_init,                                        \
    FF_CODEC_DECODE_CB(sb_decode_frame),                                     \
    .close          = sb_close,                                              \
    .bsfs           = bsf,                                                   \
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,                             \
};

SB_DECODER(h264, H264, "H.264 via Qualcomm Codec2 (SELinux Hardware Bridge)",
           "h264_mp4toannexb")
SB_DECODER(hevc, HEVC, "HEVC via Qualcomm Codec2 (SELinux Hardware Bridge)",
           "hevc_mp4toannexb")
