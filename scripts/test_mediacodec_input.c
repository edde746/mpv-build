// Behavioral regressions for the production MediaCodec input sizing and packet
// submission extracted by test_mediacodec_input.sh: the max-input-size rule
// patch 0004 configures, and what ff_mediacodec_dec_send does with a packet
// that fits, one that does not, end of stream, and back-pressure. The
// MediaCodec wrapper is a recording fake.
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define AVERROR(e) (-(e))
#define AVERROR_EXTERNAL (-0x6C787445)
#define AVERROR_EOF (-0x20464F45)
#define AV_NOPTS_VALUE INT64_MIN
#define AV_LOG_ERROR 16
#define AV_LOG_WARNING 24
#define AV_LOG_DEBUG 48
#define AV_LOG_TRACE 56
#define FFMAX(a, b) ((a) > (b) ? (a) : (b))
#define FFMIN(a, b) ((a) > (b) ? (b) : (a))
#define FFALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define INPUT_DEQUEUE_TIMEOUT_US 8000
#define MEDIACODEC_GENERATION_SHIFT 44

enum AVCodecID {
    AV_CODEC_ID_NONE, AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_MPEG4,
    AV_CODEC_ID_VP8, AV_CODEC_ID_VP9, AV_CODEC_ID_AV1, AV_CODEC_ID_MPEG2VIDEO,
};

typedef struct AVRational { int num, den; } AVRational;
static const AVRational AV_TIME_BASE_Q = { 1, 1000000 };

typedef struct AVCodecContext {
    enum AVCodecID codec_id;
    int width, height;
    AVRational pkt_timebase;
} AVCodecContext;

typedef struct AVPacket {
    uint8_t *data;
    int size;
    int64_t pts;
} AVPacket;

typedef struct FFAMediaCodec FFAMediaCodec;
typedef struct AVFifo AVFifo;
typedef int AVMutex;

typedef struct MediaCodecDecContext {
    FFAMediaCodec *codec;
    int draining;
    int flushing;
    int eos;
    ssize_t current_input_buffer;
    int async_mode;
    AVMutex async_lock;
    AVFifo *async_input;
    int async_generation;
} MediaCodecDecContext;

static int64_t av_rescale_q(int64_t a, AVRational bq, AVRational cq)
{
    return a * bq.num * cq.den / ((int64_t)bq.den * cq.num);
}

static int errors;
static void av_log(void *avctx, int level, const char *fmt, ...)
{
    if (level <= AV_LOG_ERROR)
        errors++;
}

// --- MediaCodec fake: one input buffer of a configurable size, with a
// recording of what was queued.

enum { TRY_AGAIN_LATER = -11, EOS_FLAG = 4 };

static struct {
    size_t buffer_size;
    ssize_t next_index;      // what dequeueInputBuffer returns
    int dequeued;
    int queued;
    size_t queued_size;
    int64_t queued_pts;
    uint32_t queued_flags;
    int flushed;
    uint8_t buffer[4 << 20];
} codec;

static ssize_t ff_AMediaCodec_dequeueInputBuffer(FFAMediaCodec *c, int64_t timeout)
{
    codec.dequeued++;
    return codec.next_index;
}

static int ff_AMediaCodec_infoTryAgainLater(FFAMediaCodec *c, ssize_t index)
{
    return index == TRY_AGAIN_LATER;
}

// Asynchronous mode: the codec's offered input indices, as the callback
// thread would have queued them, and which of them the codec still owns
// (a stale offer from before a flush).
static struct {
    int32_t offers[8];
    int count, next;
    uint32_t codec_owned; // bit per index: getInputBuffer answers NULL
} async;

static void ff_mutex_lock(AVMutex *m) { (void)m; }
static void ff_mutex_unlock(AVMutex *m) { (void)m; }
static int mediacodec_dec_async_wait(MediaCodecDecContext *s, AVFifo *fifo, int64_t timeout_us)
{
    (void)s; (void)fifo; (void)timeout_us;
    return 0;
}
static int av_fifo_read(AVFifo *fifo, void *buf, size_t nb_elems)
{
    (void)fifo; (void)nb_elems;
    if (async.next >= async.count)
        return -1;
    *(int32_t *)buf = async.offers[async.next++];
    return 0;
}

static uint8_t *ff_AMediaCodec_getInputBuffer(FFAMediaCodec *c, size_t index, size_t *size)
{
    if (async.codec_owned & (1u << index))
        return NULL;
    *size = codec.buffer_size;
    return codec.buffer;
}

static int ff_AMediaCodec_queueInputBuffer(FFAMediaCodec *c, size_t index, off_t offset,
                                          size_t size, uint64_t pts, uint32_t flags)
{
    codec.queued++;
    codec.queued_size = size;
    codec.queued_pts = (int64_t)pts;
    codec.queued_flags = flags;
    return 0;
}

static uint32_t ff_AMediaCodec_getBufferFlagEndOfStream(FFAMediaCodec *c)
{
    return EOS_FLAG;
}

static int ff_mediacodec_dec_flush(AVCodecContext *avctx, MediaCodecDecContext *s)
{
    codec.flushed++;
    s->draining = s->flushing = s->eos = 0;
    s->current_input_buffer = -1;
    return 1;
}

#include "mediacodec_input.inc"

// --- harness

static void check(bool ok, const char *scenario)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", scenario);
        exit(1);
    }
}

static AVCodecContext avctx;
static MediaCodecDecContext ctx;
static uint8_t payload[3 << 20];

static void reset(size_t buffer_size)
{
    memset(&codec, 0, sizeof(codec));
    codec.buffer_size = buffer_size;
    codec.next_index = 0;
    memset(&async, 0, sizeof(async));
    memset(&ctx, 0, sizeof(ctx));
    ctx.current_input_buffer = -1;
    avctx.pkt_timebase = (AVRational){ 1, 90000 };
    errors = 0;
}

static AVPacket packet(int size, int64_t pts)
{
    for (int i = 0; i < size; i++)
        payload[i] = (uint8_t)(i * 31 + 7);
    return (AVPacket){ .data = payload, .size = size, .pts = pts };
}

// --- cases

static void test_max_input_size_matches_media3(void)
{
    const struct {
        enum AVCodecID codec_id;
        int width, height, expected;
        const char *name;
    } cases[] = {
        {AV_CODEC_ID_H264, 1920, 1080, 1920 * 1088 * 3 / 4, "H.264 rounds to macroblocks"},
        {AV_CODEC_ID_HEVC, 1920, 1080, 2 * 1024 * 1024, "HEVC keeps a 2 MiB floor"},
        {AV_CODEC_ID_HEVC, 3840, 2160, 3840 * 2160 * 3 / 4, "4K HEVC sizes from pixels"},
        {AV_CODEC_ID_VP9, 1920, 1080, 1920 * 1080 * 3 / 8, "VP9 halves the ratio"},
        {AV_CODEC_ID_AV1, 1920, 1080, 1920 * 1080 * 3 / 4, "AV1 sizes from pixels"},
        {AV_CODEC_ID_MPEG2VIDEO, 1920, 1080, 0, "unlisted codecs keep the decoder default"},
        {AV_CODEC_ID_HEVC, 0, 1080, 0, "unknown dimensions keep the decoder default"},
        {AV_CODEC_ID_HEVC, 65536, 65536, 0, "an int overflow keeps the decoder default"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        AVCodecContext c = { .codec_id = cases[i].codec_id,
                             .width = cases[i].width, .height = cases[i].height };
        int size = video_max_input_size(&c);
        if (size != cases[i].expected) {
            fprintf(stderr, "FAIL: %s: got %d, expected %d\n", cases[i].name,
                    size, cases[i].expected);
            exit(1);
        }
    }
}

static void test_packet_that_fits_is_queued_whole(void)
{
    reset(2 << 20);
    AVPacket pkt = packet(1 << 20, 90000);
    int ret = ff_mediacodec_dec_send(&avctx, &ctx, &pkt, false);
    check(ret == pkt.size, "a fitting packet is consumed");
    check(codec.queued == 1 && codec.queued_size == (size_t)pkt.size,
          "a fitting packet is one input buffer");
    check(!memcmp(codec.buffer, payload, pkt.size), "the payload is copied");
    check(codec.queued_pts == 1000000 && codec.queued_flags == 0,
          "pts is rescaled to microseconds with no flags");
    check(!codec.flushed && !errors, "a fitting packet neither flushes nor errors");
    check(ctx.current_input_buffer == -1, "the input buffer was used up");

    // A pre-dequeued buffer from the caller's poll is used without dequeuing.
    reset(2 << 20);
    ctx.current_input_buffer = 3;
    ret = ff_mediacodec_dec_send(&avctx, &ctx, &pkt, false);
    check(ret == pkt.size && codec.queued == 1 && !codec.dequeued,
          "a pre-dequeued input buffer is used");
}

static void test_oversized_access_unit_is_dropped_whole(void)
{
    reset(1 << 20);
    AVPacket pkt = packet(3 << 20, 180000);
    int ret = ff_mediacodec_dec_send(&avctx, &ctx, &pkt, false);
    check(ret == pkt.size, "an oversized access unit is reported consumed");
    check(codec.queued == 0, "no fragment of it reaches the decoder");
    check(codec.flushed == 1, "the decoder is flushed to resync on a keyframe");
    check(errors == 1, "the drop is logged as an error");
    check(ctx.current_input_buffer == -1, "the flush reclaimed the input buffer");

    // The packet after it decodes normally.
    pkt = packet(1 << 19, 270000);
    ret = ff_mediacodec_dec_send(&avctx, &ctx, &pkt, false);
    check(ret == pkt.size && codec.queued == 1 && codec.flushed == 1,
          "the following packet is queued whole");

    // Exactly filling the buffer is not oversized.
    reset(1 << 20);
    pkt = packet(1 << 20, 90000);
    ret = ff_mediacodec_dec_send(&avctx, &ctx, &pkt, false);
    check(ret == pkt.size && codec.queued == 1 && !codec.flushed,
          "a packet the size of the buffer is queued whole");
}

static void test_end_of_stream_and_back_pressure(void)
{
    reset(1 << 20);
    AVPacket eos = { .data = NULL, .size = 0, .pts = AV_NOPTS_VALUE };
    int ret = ff_mediacodec_dec_send(&avctx, &ctx, &eos, true);
    check(ret == 0 && codec.queued == 1 && codec.queued_size == 0 &&
          codec.queued_flags == EOS_FLAG && ctx.draining,
          "an empty packet queues end of stream");
    check(!codec.flushed, "end of stream is never mistaken for an oversized unit");

    reset(1 << 20);
    codec.next_index = TRY_AGAIN_LATER;
    AVPacket pkt = packet(1 << 19, 90000);
    ret = ff_mediacodec_dec_send(&avctx, &ctx, &pkt, false);
    check(ret == AVERROR(EAGAIN) && !codec.queued,
          "no free input buffer means try again with the whole packet");

    reset(1 << 20);
    ctx.flushing = 1;
    ret = ff_mediacodec_dec_send(&avctx, &ctx, &pkt, false);
    check(ret == AVERROR_EXTERNAL && !codec.queued,
          "a pending flush refuses input");
}

static void test_asynchronous_offers_and_stale_indices(void)
{
    // Asynchronous mode takes offered indices, never dequeues, and tags the
    // timestamp with the flush generation the codec passes back unchanged.
    reset(1 << 20);
    ctx.async_mode = 1;
    ctx.async_generation = 2;
    async.offers[0] = 5;
    async.count = 1;
    AVPacket pkt = packet(1 << 19, 90000);
    int ret = ff_mediacodec_dec_send(&avctx, &ctx, &pkt, false);
    check(ret == pkt.size && codec.queued == 1 && !codec.dequeued,
          "an offered index carries the packet without a dequeue");
    check(codec.queued_pts == ((int64_t)2 << 44) + 1000000,
          "the timestamp carries the flush generation above the media time");
    check(!errors, "asynchronous submission is not an error");

    // An index offered before a flush that the codec owns again is skipped
    // for the next offer; the packet is neither lost nor an error.
    reset(1 << 20);
    ctx.async_mode = 1;
    async.offers[0] = 3;
    async.offers[1] = 4;
    async.count = 2;
    async.codec_owned = 1u << 3;
    ret = ff_mediacodec_dec_send(&avctx, &ctx, &pkt, false);
    check(ret == pkt.size && codec.queued == 1,
          "a stale offer is skipped and the packet queued on the next one");
    check(!errors, "a stale offer is not an error");

    // No offer at all is back-pressure, as in synchronous mode.
    reset(1 << 20);
    ctx.async_mode = 1;
    ret = ff_mediacodec_dec_send(&avctx, &ctx, &pkt, false);
    check(ret == AVERROR(EAGAIN) && !codec.queued,
          "no offered index means try again with the whole packet");
}

int main(void)
{
    test_max_input_size_matches_media3();
    test_packet_that_fits_is_queued_whole();
    test_oversized_access_unit_is_dropped_whole();
    test_end_of_stream_and_back_pressure();
    test_asynchronous_offers_and_stale_indices();
    puts("PASS: MediaCodec input buffers are sized like Media3 and an access "
         "unit that still does not fit is dropped whole, never split");
    return 0;
}
