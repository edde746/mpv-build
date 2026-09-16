// Behavioral regressions for the production asynchronous MediaCodec output
// path extracted by test_mediacodec_eos.sh: the flush-generation filter that
// drops output the codec finished before a flush, and end of stream, which
// the codec reports by a buffer flag and whose timestamp it need not echo. A
// codec that answered a drain with an EOS carrying timestamp 0 used to have
// that answer discarded as stale once the session had flushed even once
// (every resume and seek does), so the decoder never returned EOF, mpv never
// ended the file, and the player sat on the last frame. The MediaCodec
// wrapper is a recording fake; the input side is driven through the
// production send so the EOS is queued the way a real drain queues it.
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
#define AV_LOG_INFO 32
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

typedef struct AVFrame { int unused; } AVFrame;

typedef struct FFAMediaCodec FFAMediaCodec;
typedef struct FFAMediaFormat FFAMediaFormat;

typedef struct FFAMediaCodecBufferInfo {
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
} FFAMediaCodecBufferInfo;

typedef pthread_mutex_t AVMutex;
typedef pthread_cond_t AVCond;
#define ff_mutex_lock(m)   pthread_mutex_lock(m)
#define ff_mutex_unlock(m) pthread_mutex_unlock(m)

// The two queues the codec's callbacks fill in production; here the test
// fills them, standing in for the codec thread.
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
    atomic_int hw_buffer_count;
    FFAMediaCodec *codec;
    FFAMediaFormat *format;
    void *surface;
    int draining;
    int flushing;
    int eos;
    uint64_t output_buffer_count;
    ssize_t current_input_buffer;
    atomic_int serial;
    int async_mode;
    AVMutex async_lock;
    AVCond async_cond;
    AVFifo *async_input;
    AVFifo *async_output;
    int async_error;
    int async_generation;
    int64_t pending_drop_pts[8];
    int pending_drops;
} MediaCodecDecContext;

typedef struct MediaCodecAsyncOutput {
    int32_t index;
    FFAMediaCodecBufferInfo info;
} MediaCodecAsyncOutput;

// The wait on a notification: the test queues every notification ahead of
// the call it answers, so there is never anything to wait for.
static int mediacodec_dec_async_wait(MediaCodecDecContext *s, AVFifo *fifo,
                                     int64_t timeout_us)
{
    return s->async_error;
}

static int64_t av_rescale_q(int64_t a, AVRational bq, AVRational cq)
{
    return a * bq.num * cq.den / ((int64_t)bq.den * cq.num);
}

static void av_freep(void *p)
{
    void **pp = p;
    free(*pp);
    *pp = NULL;
}

static int errors;
static void av_log(void *avctx, int level, const char *fmt, ...)
{
    if (level <= AV_LOG_ERROR)
        errors++;
}

// --- MediaCodec fake: records what is queued and released, hands out the
// output events the test lined up (synchronous mode) and the frames the
// decoder wrapped.

enum {
    INFO_TRY_AGAIN_LATER = -1,
    INFO_OUTPUT_FORMAT_CHANGED = -2,
    INFO_OUTPUT_BUFFERS_CHANGED = -3,
    EOS_FLAG = 4,
};

static struct {
    // input side
    int queued;
    int64_t queued_pts;
    uint32_t queued_flags;
    size_t queued_size;
    int flushes, starts;
    uint8_t buffer[4096];
    // synchronous-mode output events, in delivery order
    MediaCodecAsyncOutput sync_outputs[8];
    int sync_count, sync_head;
    // releases
    int releases;
    int32_t released_index[8];
    int released_render[8];
    // frames the decoder wrapped
    int frames;
    int64_t frame_pts[8];
    uint32_t frame_flags[8];
    int sw_wraps;
} codec;

static ssize_t ff_AMediaCodec_dequeueInputBuffer(FFAMediaCodec *c, int64_t timeout)
{
    return 0;
}

static int ff_AMediaCodec_infoTryAgainLater(FFAMediaCodec *c, ssize_t index)
{
    return index == INFO_TRY_AGAIN_LATER;
}

static int ff_AMediaCodec_infoOutputFormatChanged(FFAMediaCodec *c, ssize_t index)
{
    return index == INFO_OUTPUT_FORMAT_CHANGED;
}

static int ff_AMediaCodec_infoOutputBuffersChanged(FFAMediaCodec *c, ssize_t index)
{
    return index == INFO_OUTPUT_BUFFERS_CHANGED;
}

static uint8_t *ff_AMediaCodec_getInputBuffer(FFAMediaCodec *c, size_t index, size_t *size)
{
    *size = sizeof(codec.buffer);
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

static ssize_t ff_AMediaCodec_dequeueOutputBuffer(FFAMediaCodec *c, FFAMediaCodecBufferInfo *info,
                                                  int64_t timeout)
{
    if (codec.sync_head == codec.sync_count)
        return INFO_TRY_AGAIN_LATER;
    MediaCodecAsyncOutput *o = &codec.sync_outputs[codec.sync_head++];
    *info = o->info;
    return o->index;
}

static int ff_AMediaCodec_releaseOutputBuffer(FFAMediaCodec *c, size_t index, int render)
{
    if (codec.releases == 8) {
        fprintf(stderr, "FAIL: too many releases\n");
        exit(1);
    }
    codec.released_index[codec.releases] = (int32_t)index;
    codec.released_render[codec.releases] = render;
    codec.releases++;
    return 0;
}

static int ff_AMediaCodec_getBufferFlagEndOfStream(FFAMediaCodec *c)
{
    return EOS_FLAG;
}

static int ff_AMediaCodec_flush(FFAMediaCodec *c)
{
    codec.flushes++;
    return 0;
}

static int ff_AMediaCodec_start(FFAMediaCodec *c)
{
    codec.starts++;
    return 0;
}

static uint8_t *ff_AMediaCodec_getOutputBuffer(FFAMediaCodec *c, size_t index, size_t *size)
{
    return NULL;
}

static FFAMediaFormat *ff_AMediaCodec_getOutputFormat(FFAMediaCodec *c)
{
    return NULL;
}

static int ff_AMediaCodec_cleanOutputBuffers(FFAMediaCodec *c)
{
    return 0;
}

static int ff_AMediaFormat_delete(FFAMediaFormat *format)
{
    return 0;
}

static char *ff_AMediaFormat_toString(FFAMediaFormat *format)
{
    return NULL;
}

static int mediacodec_dec_parse_format(AVCodecContext *avctx, MediaCodecDecContext *s,
                                       int output_format_changed)
{
    return 0;
}

static int ff_mediacodec_dec_flush(AVCodecContext *avctx, MediaCodecDecContext *s)
{
    return 1;
}

static int64_t mediacodec_dec_untag_pts(const MediaCodecDecContext *s, int64_t tagged);

static int mediacodec_wrap_hw_buffer(AVCodecContext *avctx, MediaCodecDecContext *s,
                                     ssize_t index, FFAMediaCodecBufferInfo *info,
                                     AVFrame *frame)
{
    if (codec.frames == 8) {
        fprintf(stderr, "FAIL: too many frames\n");
        exit(1);
    }
    // What the production wrapper stamps on the frame: the timestamp with the
    // generation tag cleared.
    codec.frame_pts[codec.frames] = mediacodec_dec_untag_pts(s, info->presentationTimeUs);
    codec.frame_flags[codec.frames] = info->flags;
    codec.frames++;
    return 0;
}

static int mediacodec_wrap_sw_buffer(AVCodecContext *avctx, MediaCodecDecContext *s,
                                     uint8_t *data, size_t size, ssize_t index,
                                     FFAMediaCodecBufferInfo *info, AVFrame *frame)
{
    codec.sw_wraps++;
    return AVERROR_EXTERNAL;
}

#include "mediacodec_eos.inc"

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
static AVFrame frame;
static int surface_token;

static void reset(int async_mode)
{
    memset(&codec, 0, sizeof(codec));
    memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_init(&ctx.async_lock, NULL);
    ctx.surface = &surface_token;
    ctx.current_input_buffer = -1;
    ctx.async_mode = async_mode;
    ctx.async_input = fifo_new(sizeof(int32_t));
    ctx.async_output = fifo_new(sizeof(MediaCodecAsyncOutput));
    avctx.pkt_timebase = (AVRational){ 1, 90000 };
    errors = 0;
}

// A seek or resume: the production flush, which opens the next generation.
static void flush(void)
{
    check(mediacodec_dec_flush_codec(&avctx, &ctx) == 0 && !ctx.draining && !ctx.eos,
          "a flush leaves the decoder idle");
}

// The codec offering an input slot, then the decoder's own end-of-stream
// submission on it: what the wrapper does when its packet source hits EOF.
static void queue_eos(void)
{
    int32_t slot = 0;
    if (ctx.async_mode)
        fifo_push(ctx.async_input, &slot);
    AVPacket eos = { .data = NULL, .size = 0, .pts = 0 };
    int before = codec.queued;
    int ret = ff_mediacodec_dec_send(&avctx, &ctx, &eos, true);
    check(ret == 0 && codec.queued == before + 1 && codec.queued_size == 0 &&
          codec.queued_flags == EOS_FLAG && ctx.draining,
          "the drain queues an empty end-of-stream buffer");
}

// The codec reporting a finished output buffer.
static void deliver(int32_t index, int64_t pts, int32_t size, uint32_t flags)
{
    MediaCodecAsyncOutput o = {
        .index = index,
        .info = { .offset = 0, .size = size, .presentationTimeUs = pts, .flags = flags },
    };
    if (ctx.async_mode) {
        fifo_push(ctx.async_output, &o);
    } else {
        check(codec.sync_count < 8, "fake output queue has room");
        codec.sync_outputs[codec.sync_count++] = o;
    }
}

static int receive(bool wait)
{
    return ff_mediacodec_dec_receive(&avctx, &ctx, &frame, wait);
}

// A timestamp as the codec would echo it for a frame queued this generation.
static int64_t tagged(int64_t pts)
{
    return mediacodec_dec_tag_pts(&ctx, pts);
}

static bool released_unrendered(int nth, int32_t index)
{
    return codec.releases > nth && codec.released_index[nth] == index && !codec.released_render[nth];
}

// --- cases

static void test_drain_after_a_flush_ends_on_an_unechoed_eos(void)
{
    // The reported failure: one flush earlier in the session, then the file
    // runs out. The codec hands over the frame it still held and reports EOS
    // on an empty buffer whose timestamp is its own (0 here).
    reset(1);
    flush();
    queue_eos();
    deliver(3, tagged(40000), 1, 0);
    deliver(4, 0, 0, EOS_FLAG);

    int ret = receive(true);
    check(ret == 0 && codec.frames == 1 && codec.frame_pts[0] == 40000 && !codec.frame_flags[0],
          "the frame the codec still held comes out first, with its own timestamp");
    ret = receive(true);
    check(ret == AVERROR_EOF, "an EOS whose timestamp the codec did not echo ends the drain");
    check(released_unrendered(0, 4) && codec.releases == 1, "the EOS buffer goes back unrendered");
    check(receive(true) == AVERROR_EOF, "the decoder stays at EOF");
    check(!errors, "nothing was logged as an error");

    // The same on a codec that reports EOS with a timestamp of -1.
    reset(1);
    flush();
    queue_eos();
    deliver(5, -1, 0, EOS_FLAG);
    check(receive(true) == AVERROR_EOF && released_unrendered(0, 5) && !errors,
          "an EOS at timestamp -1 ends the drain too");
}

static void test_drain_without_a_flush_is_unchanged(void)
{
    // A fresh session (no seek, no resume) always worked; it must still.
    reset(1);
    queue_eos();
    deliver(2, tagged(40000), 1, 0);
    deliver(6, 0, 0, EOS_FLAG);
    check(receive(true) == 0 && codec.frames == 1 && codec.frame_pts[0] == 40000,
          "the last frame is delivered in a session that never flushed");
    check(receive(true) == AVERROR_EOF && released_unrendered(0, 6) && !errors,
          "the EOS ends the drain in a session that never flushed");
}

static void test_echoed_eos_keeps_its_frame(void)
{
    // A codec that flags EOS on its last real frame, timestamp echoed: the
    // frame must reach the caller before EOF does.
    reset(1);
    flush();
    queue_eos();
    deliver(7, tagged(80000), 1, EOS_FLAG);
    check(receive(true) == 0 && codec.frames == 1 && codec.frame_pts[0] == 80000 &&
          (codec.frame_flags[0] & EOS_FLAG) && !codec.releases,
          "a last frame flagged EOS is delivered as a frame");
    check(receive(true) == AVERROR_EOF && !errors, "and the call after it reports EOF");
}

static void test_stale_output_is_still_dropped_outside_a_drain(void)
{
    // A flush cancelled a drain: the frame and the EOS the codec had already
    // finished for it may still be reported. Neither belongs to the new
    // generation, and the EOS must not pre-empt the drain that has not
    // started.
    reset(1);
    flush();
    deliver(1, tagged(40000) - ((int64_t)1 << MEDIACODEC_GENERATION_SHIFT), 1, 0);
    deliver(2, 0, 0, EOS_FLAG);
    int ret = receive(false);
    check(ret == AVERROR(EAGAIN) && !codec.frames && !ctx.eos,
          "output from before the flush yields neither a frame nor EOF");
    check(codec.releases == 2 && released_unrendered(0, 1) && released_unrendered(1, 2),
          "both stale buffers go back unrendered");

    // The new generation's own drain then ends on its own EOS.
    queue_eos();
    deliver(3, tagged(0), 0, EOS_FLAG);
    check(receive(true) == AVERROR_EOF && released_unrendered(2, 3) && !errors,
          "the drain that follows ends on the EOS it asked for");
}

static void test_synchronous_mode_reads_only_the_flag(void)
{
    // The polled decoder never tagged timestamps, so the generation means
    // nothing to it; an EOS at timestamp 0 ends its drain whatever the
    // count says.
    reset(0);
    ctx.async_generation = 3;
    queue_eos();
    deliver(9, 0, 0, EOS_FLAG);
    check(receive(true) == AVERROR_EOF && released_unrendered(0, 9) && !errors,
          "the synchronous decoder ends its drain on the flag alone");
}

int main(void)
{
    test_drain_after_a_flush_ends_on_an_unechoed_eos();
    test_drain_without_a_flush_is_unchanged();
    test_echoed_eos_keeps_its_frame();
    test_stale_output_is_still_dropped_outside_a_drain();
    test_synchronous_mode_reads_only_the_flag();
    puts("PASS: a MediaCodec drain ends on the end-of-stream flag whatever "
         "timestamp the codec put on it, and stale output is still dropped");
    return 0;
}
