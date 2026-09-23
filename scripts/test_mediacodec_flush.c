// Behavioral regressions for when the production MediaCodec flush reaches the
// codec, extracted by test_mediacodec_flush.sh. A player that resumes
// mid-file opens the decoder and seeks at once, which flushed a codec started
// milliseconds earlier and not yet given any input. Sessions on Rockchip's
// c2.rk.hevc.decoder that began that way later failed an ordinary seek flush
// fatally and fell back to software decoding (Plezy #2431). A codec that has
// handed out no input holds nothing to discard, so that flush must not reach
// it; once input has left the codec, every flush must, and must give every
// slot back. The MediaCodec wrapper is a recording fake; the polled and the
// asynchronous decoder are both driven.
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
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
#define FFMIN(a, b) ((a) > (b) ? (b) : (a))

typedef struct AVRational { int num, den; } AVRational;
static const AVRational AV_TIME_BASE_Q = { 1, 1000000 };

typedef struct AVCodecContext {
    AVRational pkt_timebase;
} AVCodecContext;

typedef struct AVPacket {
    uint8_t *data;
    int size;
    int64_t pts;
} AVPacket;

typedef struct FFAMediaCodec FFAMediaCodec;

typedef pthread_mutex_t AVMutex;
#define ff_mutex_lock(m)   pthread_mutex_lock(m)
#define ff_mutex_unlock(m) pthread_mutex_unlock(m)

// The queue the codec's input callback fills in production; here the test
// fills it, standing in for the codec thread.
typedef struct AVFifo {
    size_t elem_size, capacity, head, count;
    uint8_t *buf;
} AVFifo;

static AVFifo *fifo_new(size_t elem_size)
{
    AVFifo *f = calloc(1, sizeof(*f));
    f->elem_size = elem_size;
    f->capacity = 64;
    f->buf = calloc(f->capacity, elem_size);
    return f;
}

static void fifo_free(AVFifo *f)
{
    if (f)
        free(f->buf);
    free(f);
}

static void fifo_push(AVFifo *f, const void *elem)
{
    if (f->count == f->capacity) {
        fprintf(stderr, "FAIL: fake fifo overflow\n");
        exit(1);
    }
    size_t at = (f->head + f->count) % f->capacity;
    memcpy(f->buf + at * f->elem_size, elem, f->elem_size);
    f->count++;
}

static int av_fifo_read(AVFifo *f, void *buf, size_t nb_elems)
{
    if (nb_elems != 1 || !f->count)
        return AVERROR(EINVAL);
    memcpy(buf, f->buf + f->head * f->elem_size, f->elem_size);
    f->head = (f->head + 1) % f->capacity;
    f->count--;
    return 0;
}

static void av_fifo_reset2(AVFifo *f)
{
    f->head = f->count = 0;
}

typedef struct MediaCodecDecContext {
    atomic_int refcount;
    atomic_int hw_buffer_count;
    FFAMediaCodec *codec;
    void *surface;
    int draining;
    int flushing;
    int eos;
    uint64_t output_buffer_count;
    ssize_t current_input_buffer;
    int input_since_flush;
    bool delay_flush;
    atomic_int serial;
    int async_mode;
    AVMutex async_lock;
    AVFifo *async_input;
    AVFifo *async_output;
    int async_error;
    int async_generation;
    int pending_drops;
} MediaCodecDecContext;

// The wait on a notification: the test offers every slot ahead of the call
// that takes it, so there is never anything to wait for.
static int mediacodec_dec_async_wait(MediaCodecDecContext *s, AVFifo *fifo,
                                     int64_t timeout_us)
{
    return s->async_error;
}

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

// --- MediaCodec fake: a fixed set of input slots the polled decoder
// dequeues, a flush that hands every slot back to the codec, and a recording
// of what reached the codec.

enum { TRY_AGAIN_LATER = -11, EOS_FLAG = 4, SLOTS = 4 };

static struct {
    int free_slots;   // polled decoder: slots dequeueInputBuffer can hand out
    int next_slot;
    size_t buffer_size;
    uint8_t buffer[4096];
    int queued;
    int64_t queued_pts;
    int flushes, starts;
} codec;

static ssize_t ff_AMediaCodec_dequeueInputBuffer(FFAMediaCodec *c, int64_t timeout)
{
    if (!codec.free_slots)
        return TRY_AGAIN_LATER;
    codec.free_slots--;
    return codec.next_slot++ % SLOTS;
}

static int ff_AMediaCodec_infoTryAgainLater(FFAMediaCodec *c, ssize_t index)
{
    return index == TRY_AGAIN_LATER;
}

static uint8_t *ff_AMediaCodec_getInputBuffer(FFAMediaCodec *c, size_t index, size_t *size)
{
    *size = codec.buffer_size;
    return codec.buffer;
}

static int ff_AMediaCodec_queueInputBuffer(FFAMediaCodec *c, size_t index, off_t offset,
                                          size_t size, uint64_t pts, uint32_t flags)
{
    codec.queued++;
    codec.queued_pts = (int64_t)pts;
    return 0;
}

static uint32_t ff_AMediaCodec_getBufferFlagEndOfStream(FFAMediaCodec *c)
{
    return EOS_FLAG;
}

static int ff_AMediaCodec_flush(FFAMediaCodec *c)
{
    codec.flushes++;
    // Every input slot is the codec's again, queued or merely dequeued.
    codec.free_slots = SLOTS;
    return 0;
}

static int ff_AMediaCodec_start(FFAMediaCodec *c)
{
    codec.starts++;
    return 0;
}

#include "mediacodec_flush.inc"

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
static uint8_t payload[8192];
static int surface_token;

// A codec just configured and started, the moment mpv opens the decoder: the
// asynchronous one has already offered two input slots through its callback.
static void open_codec(int async_mode)
{
    fifo_free(ctx.async_input);
    fifo_free(ctx.async_output);
    memset(&codec, 0, sizeof(codec));
    memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_init(&ctx.async_lock, NULL);
    atomic_init(&ctx.refcount, 1);
    atomic_init(&ctx.serial, 1);
    ctx.surface = &surface_token;
    ctx.current_input_buffer = -1;
    ctx.async_mode = async_mode;
    ctx.async_input = fifo_new(sizeof(int32_t));
    ctx.async_output = fifo_new(sizeof(int32_t));
    avctx.pkt_timebase = (AVRational){ 1, 90000 };
    codec.free_slots = SLOTS;
    codec.buffer_size = sizeof(codec.buffer);
    errors = 0;
    if (async_mode)
        for (int32_t slot = 0; slot < 2; slot++)
            fifo_push(ctx.async_input, &slot);
}

// The asynchronous codec re-offering a slot after it was restarted.
static void offer(int32_t slot)
{
    fifo_push(ctx.async_input, &slot);
}

// What a seek does to the decoder: avcodec_flush_buffers reaches the
// production flush entry.
static void seek(void)
{
    check(ff_mediacodec_dec_flush(&avctx, &ctx) == 1, "a seek completes the flush");
}

static int send(int size, int64_t pts)
{
    AVPacket pkt = { .data = payload, .size = size, .pts = pts };
    return ff_mediacodec_dec_send(&avctx, &ctx, &pkt, false);
}

static const char *mode(int async_mode)
{
    return async_mode ? "asynchronous" : "polled";
}

static void checkf(bool ok, int async_mode, const char *scenario)
{
    char msg[256];
    snprintf(msg, sizeof(msg), "%s decoder: %s", mode(async_mode), scenario);
    check(ok, msg);
}

// --- cases

static void test_resume_seek_leaves_a_fresh_codec_alone(int async_mode)
{
    // The reported sequence: open, seek to the resume position before any
    // packet, then decode from there.
    open_codec(async_mode);
    seek();
    checkf(codec.flushes == 0 && codec.starts == 0, async_mode,
           "a codec that has handed out no input is not flushed");
    checkf(ctx.async_generation == 0, async_mode,
           "nothing was flushed, so no output generation is retired");

    // The asynchronous codec's slots offered before the seek are still its
    // offers; the packet goes out on one and keeps the session's timestamps.
    checkf(send(1024, 90000) == 1024 && codec.queued == 1 && codec.queued_pts == 1000000,
           async_mode, "the first packet after the seek is queued as usual");
    checkf(!errors, async_mode, "nothing was logged as an error");

    // From here the codec has input to discard: the next seek flushes it.
    seek();
    checkf(codec.flushes == 1 && codec.starts == (async_mode ? 1 : 0), async_mode,
           "a seek after input flushes the codec (and restarts it when asynchronous)");
    checkf(ctx.async_generation == (async_mode ? 1 : 0), async_mode,
           "the flush retires the output generation queued before it");
}

static void test_back_to_back_seeks_flush_once(int async_mode)
{
    // Two seeks with no packet between them: the first gives every slot
    // back, the second finds nothing to discard.
    open_codec(async_mode);
    checkf(send(1024, 90000) == 1024 && codec.queued == 1, async_mode, "a packet is queued");
    seek();
    seek();
    checkf(codec.flushes == 1, async_mode, "a second seek without input does not flush again");

    // Input after the flush makes the next seek reach the codec again.
    if (async_mode)
        offer(0);
    checkf(send(1024, 180000) == 1024 && codec.queued == 2, async_mode,
           "the packet after the seeks is queued");
    seek();
    checkf(codec.flushes == 2 && !errors, async_mode, "the seek after it flushes");
}

static void test_dropped_access_unit_gives_its_slot_back(int async_mode)
{
    // The first packet does not fit its input buffer: send takes a slot,
    // drops the access unit and flushes to give the slot back. Nothing has
    // been queued yet, but the slot has left the codec, so this flush must
    // reach it or the slot is lost.
    open_codec(async_mode);
    codec.buffer_size = 512;
    checkf(send(1024, 90000) == 1024 && codec.queued == 0, async_mode,
           "the oversized access unit is consumed without reaching the codec");
    checkf(codec.flushes == 1, async_mode, "the slot it was dropped from goes back through a flush");
    checkf(ctx.current_input_buffer == -1 && codec.free_slots == SLOTS, async_mode,
           "no input slot stays out after the flush");
}

int main(void)
{
    for (int async_mode = 0; async_mode <= 1; async_mode++) {
        test_resume_seek_leaves_a_fresh_codec_alone(async_mode);
        test_back_to_back_seeks_flush_once(async_mode);
        test_dropped_access_unit_gives_its_slot_back(async_mode);
    }
    puts("PASS: a MediaCodec flush reaches the codec only once an input slot has "
         "left it, and then gives every slot back");
    return 0;
}
