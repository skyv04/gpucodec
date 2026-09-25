/*
 * AGC-1 GPU compute-shader intra codec, ffmpeg encoder wrapper.
 *
 * AGC-1's actual compression pipeline lives in agc_core.h, upstream at
 * https://github.com/skyv04/selinux-hardware-bridge (see the repo README,
 * "Embedding AGC-1").
 * This file is just the libavcodec glue: it turns one AVFrame into one
 * AVPacket by calling into that header, using the packet framing decided
 * for this integration -- a plain concatenation of each plane's compressed
 * bitstream, since AVCodecContext already carries width/height/pixel format
 * so there is no need to replicate agc.c's own on-disk container header.
 *
 * Packet layout (little-endian), repeated once per plane (Y, U, V):
 *   uint32_t nTiles
 *   uint16_t tileBits[nTiles]
 *   uint32_t nWords
 *   uint32_t words[nWords]
 */
#include "avcodec.h"
#include "codec_internal.h"
#include "encode.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "agc_core.h"

typedef struct AGC1EncContext {
    AVClass *avclass;
    Progs pg;
    Plane planes[3];   // Y, U, V (only [0] used for gray8)
    int nplanes;
    int quality;        // 1..100, mirrors agc.c's -q
    int booted;
} AGC1EncContext;

static void put_le32(uint8_t *p, uint32_t v) { AV_WL32(p, v); }
static void put_le16(uint8_t *p, uint16_t v) { AV_WL16(p, v); }

static av_cold int agc1_encode_init(AVCodecContext *avctx)
{
    AGC1EncContext *s = avctx->priv_data;
    char err[256];

    if (avctx->pix_fmt != AV_PIX_FMT_YUV420P && avctx->pix_fmt != AV_PIX_FMT_GRAY8) {
        av_log(avctx, AV_LOG_ERROR, "agc1: only yuv420p and gray8 are supported\n");
        return AVERROR(EINVAL);
    }
    if (avctx->width < 8 || avctx->height < 8) {
        av_log(avctx, AV_LOG_ERROR, "agc1: frames smaller than 8x8 are not supported\n");
        return AVERROR(EINVAL);
    }

    if (agc_gl_lock(err, sizeof err)) {
        av_log(avctx, AV_LOG_ERROR, "agc1: GPU init failed: %s\n", err);
        return AVERROR_EXTERNAL;
    }
    if (agc_progs_init(&s->pg, err, sizeof err)) {
        av_log(avctx, AV_LOG_ERROR, "agc1: shader build failed: %s\n", err);
        agc_gl_unlock();
        return AVERROR_EXTERNAL;
    }

    s->nplanes = (avctx->pix_fmt == AV_PIX_FMT_YUV420P) ? 3 : 1;
    agc_plane_init(&s->planes[0], avctx->width, avctx->height);
    if (s->nplanes == 3) {
        int cw = (avctx->width + 1) / 2, ch = (avctx->height + 1) / 2;
        agc_plane_init(&s->planes[1], cw, ch);
        agc_plane_init(&s->planes[2], cw, ch);
    }
    agc_gl_unlock();

    s->quality = avctx->global_quality > 0
        ? av_clip(avctx->global_quality / FF_QP2LAMBDA, 1, 100)
        : 80;
    s->booted = 1;
    return 0;
}

static av_cold int agc1_encode_close(AVCodecContext *avctx)
{
    AGC1EncContext *s = avctx->priv_data;
    char err[256];
    if (!s->booted) return 0;
    if (!agc_gl_lock(err, sizeof err)) {
        for (int i = 0; i < s->nplanes; ++i) agc_plane_free(&s->planes[i]);
        agc_gl_unlock();
    }
    return 0;
}

static int encode_one_plane(AVCodecContext *avctx, AGC1EncContext *s, int idx,
                            const uint8_t *src, int stride, int qscale, Bits *out)
{
    Plane *pl = &s->planes[idx];
    uint8_t *padded = av_malloc((size_t)pl->W * pl->H);
    if (!padded) return AVERROR(ENOMEM);

    // agc_pad_plane expects a tightly packed rw*rh source; copy row-by-row
    // first in case the AVFrame stride differs from the plane width.
    uint8_t *tight = av_malloc((size_t)pl->rw * pl->rh);
    if (!tight) { av_free(padded); return AVERROR(ENOMEM); }
    for (int y = 0; y < pl->rh; ++y)
        memcpy(tight + (size_t)y * pl->rw, src + (size_t)y * stride, pl->rw);
    agc_pad_plane(tight, pl->rw, pl->rh, padded, pl->W, pl->H);
    av_free(tight);

    int rc = agc_encode_plane(&s->pg, pl, padded, qscale, out);
    av_free(padded);
    if (rc) {
        av_log(avctx, AV_LOG_ERROR, "agc1: encode_plane failed for plane %d\n", idx);
        return AVERROR_EXTERNAL;
    }
    return 0;
}

static int agc1_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                             const AVFrame *pic, int *got_packet)
{
    AGC1EncContext *s = avctx->priv_data;
    Bits bits[3] = {{0}};
    char err[256];
    int ret = 0;

    int qscale = agc_qscale_for(s->quality);
    int cqscale = agc_chroma_qscale(qscale);

    if (agc_gl_lock(err, sizeof err)) {
        av_log(avctx, AV_LOG_ERROR, "agc1: GPU lock failed: %s\n", err);
        return AVERROR_EXTERNAL;
    }

    for (int i = 0; i < s->nplanes && !ret; ++i) {
        int q = (i == 0) ? qscale : cqscale;
        ret = encode_one_plane(avctx, s, i, pic->data[i], pic->linesize[i], q, &bits[i]);
    }
    agc_gl_unlock();
    if (ret) goto done;

    size_t total = 0;
    for (int i = 0; i < s->nplanes; ++i)
        total += 4 + (size_t)bits[i].nTiles * 2 + 4 + (size_t)bits[i].nWords * 4;

    ret = ff_alloc_packet(avctx, pkt, total);
    if (ret < 0) goto done;

    uint8_t *p = pkt->data;
    for (int i = 0; i < s->nplanes; ++i) {
        put_le32(p, bits[i].nTiles); p += 4;
        for (int t = 0; t < bits[i].nTiles; ++t) { put_le16(p, bits[i].tileBits[t]); p += 2; }
        put_le32(p, bits[i].nWords); p += 4;
        memcpy(p, bits[i].words, (size_t)bits[i].nWords * 4);
        p += (size_t)bits[i].nWords * 4;
    }
    pkt->flags |= AV_PKT_FLAG_KEY;   // AGC-1 is intra-only
    *got_packet = 1;

done:
    for (int i = 0; i < s->nplanes; ++i) bits_free(&bits[i]);
    return ret;
}

#define OFFSET(x) offsetof(AGC1EncContext, x)
static const AVOption agc1_options[] = {
    { NULL }
};

static const AVClass agc1_encoder_class = {
    .class_name = "agc1 encoder",
    .item_name  = av_default_item_name,
    .option     = agc1_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_agc1_encoder = {
    .p.name         = "agc1",
    .p.long_name    = NULL_IF_CONFIG_SMALL("AGC-1 GPU compute-shader intra codec"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_AGC1,
    .priv_data_size = sizeof(AGC1EncContext),
    .p.priv_class   = &agc1_encoder_class,
    .init           = agc1_encode_init,
    FF_CODEC_ENCODE_CB(agc1_encode_frame),
    .close          = agc1_encode_close,
    .p.pix_fmts     = (const enum AVPixelFormat[]) {
                          AV_PIX_FMT_YUV420P, AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE
                      },
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
