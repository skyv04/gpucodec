/*
 * AGC-1 GPU compute-shader intra codec, ffmpeg decoder wrapper.
 * See agc1enc.c for the packet layout this reads.
 */
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "agc_core.h"

typedef struct AGC1DecContext {
    AVClass *avclass;
    Progs pg;
    Plane planes[3];
    int nplanes;
    int booted;
} AGC1DecContext;

static av_cold int agc1_decode_init(AVCodecContext *avctx)
{
    AGC1DecContext *s = avctx->priv_data;
    char err[256];

    // The encoder above only ever emits yuv420p today; gray8 support in
    // agc_core.h is unused on the decode side until a fourth plane-count
    // variant is needed.
    avctx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (avctx->width < 8 || avctx->height < 8) {
        av_log(avctx, AV_LOG_ERROR, "agc1: invalid frame size\n");
        return AVERROR_INVALIDDATA;
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

    s->nplanes = 3;
    agc_plane_init(&s->planes[0], avctx->width, avctx->height);
    int cw = (avctx->width + 1) / 2, ch = (avctx->height + 1) / 2;
    agc_plane_init(&s->planes[1], cw, ch);
    agc_plane_init(&s->planes[2], cw, ch);
    agc_gl_unlock();

    s->booted = 1;
    return 0;
}

static av_cold int agc1_decode_close(AVCodecContext *avctx)
{
    AGC1DecContext *s = avctx->priv_data;
    char err[256];
    if (!s->booted) return 0;
    if (!agc_gl_lock(err, sizeof err)) {
        for (int i = 0; i < s->nplanes; ++i) agc_plane_free(&s->planes[i]);
        agc_gl_unlock();
    }
    return 0;
}

// Reads one plane's framing (see agc1enc.c) starting at *pp, advancing it.
// Returns 0 on success, negative AVERROR on a truncated/corrupt packet.
static int read_plane(const uint8_t *end, const uint8_t **pp, Bits *out)
{
    const uint8_t *p = *pp;
    if (p + 4 > end) return AVERROR_INVALIDDATA;
    out->nTiles = AV_RL32(p); p += 4;
    if (out->nTiles < 0 || (size_t)(end - p) < (size_t)out->nTiles * 2 + 4)
        return AVERROR_INVALIDDATA;
    out->tileBits = av_malloc((size_t)out->nTiles * 2 + 2);
    if (!out->tileBits) return AVERROR(ENOMEM);
    for (int i = 0; i < out->nTiles; ++i) { out->tileBits[i] = AV_RL16(p); p += 2; }
    out->nWords = AV_RL32(p); p += 4;
    if (out->nWords < 0 || (size_t)(end - p) < (size_t)out->nWords * 4)
        return AVERROR_INVALIDDATA;
    out->words = av_malloc((size_t)out->nWords * 4 + 4);
    if (!out->words) return AVERROR(ENOMEM);
    memcpy(out->words, p, (size_t)out->nWords * 4);
    p += (size_t)out->nWords * 4;
    *pp = p;
    return 0;
}

static int agc1_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                             int *got_frame, AVPacket *avpkt)
{
    AGC1DecContext *s = avctx->priv_data;
    const uint8_t *p = avpkt->data, *end = avpkt->data + avpkt->size;
    Bits bits[3] = {{0}};
    char err[256];
    int ret;

    for (int i = 0; i < s->nplanes; ++i) {
        ret = read_plane(end, &p, &bits[i]);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "agc1: malformed packet (plane %d)\n", i);
            goto done;
        }
    }

    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0) goto done;

    int qscale = agc_qscale_for(80);   // quality only affects encode; decode is quality-agnostic
    int cqscale = agc_chroma_qscale(qscale);

    if (agc_gl_lock(err, sizeof err)) {
        av_log(avctx, AV_LOG_ERROR, "agc1: GPU lock failed: %s\n", err);
        ret = AVERROR_EXTERNAL;
        goto done;
    }
    for (int i = 0; i < s->nplanes; ++i) {
        Plane *pl = &s->planes[i];
        uint8_t *padded = av_malloc((size_t)pl->W * pl->H);
        if (!padded) { ret = AVERROR(ENOMEM); agc_gl_unlock(); goto done; }
        int q = (i == 0) ? qscale : cqscale;
        if (agc_decode_plane(&s->pg, pl, &bits[i], q, padded)) {
            av_log(avctx, AV_LOG_ERROR, "agc1: decode_plane failed for plane %d\n", i);
            av_free(padded); ret = AVERROR_EXTERNAL; agc_gl_unlock(); goto done;
        }
        for (int y = 0; y < pl->rh; ++y)
            memcpy(frame->data[i] + (size_t)y * frame->linesize[i],
                   padded + (size_t)y * pl->W, pl->rw);
        av_free(padded);
    }
    agc_gl_unlock();

    frame->key_frame = 1;
    frame->pict_type = AV_PICTURE_TYPE_I;
    *got_frame = 1;
    ret = avpkt->size;

done:
    for (int i = 0; i < s->nplanes; ++i) bits_free(&bits[i]);
    return ret;
}

const FFCodec ff_agc1_decoder = {
    .p.name         = "agc1",
    .p.long_name    = NULL_IF_CONFIG_SMALL("AGC-1 GPU compute-shader intra codec"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_AGC1,
    .priv_data_size = sizeof(AGC1DecContext),
    .init           = agc1_decode_init,
    .close          = agc1_decode_close,
    FF_CODEC_DECODE_CB(agc1_decode_frame),
    .p.capabilities = AV_CODEC_CAP_DR1,
};
