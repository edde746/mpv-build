// Behavioral regressions for a MediaCodec drain the codec cannot finish, on
// the production decode path extracted by test_mediacodec_drain.sh. A player
// paused within the last second of a file has sent the end of stream and
// holds the codec's output buffers, so the codec's last frames never come.
// The decoder used to wait a full second for them on every call, in both
// modes: mpv's decoding thread sat in that wait, and everything that takes
// that thread's lock -- a seek, a property read, the GL output's chain
// rebuild on a window change, the teardown -- waited it out. A host that
// registered a wakeup is now held for one poll at most: the polled codec
// gets the normal output dequeue, the asynchronous one does not wait at all
// and wakes the host when its output comes. The polled host, which asks
// again after every EAGAIN, is still paced by that poll rather than spinning.
//
// Every case drives what avcodec_receive_frame() reaches
// (mediacodec_receive_frame) for a host like mpv, which registers a wakeup.
// The MediaCodec wrapper is a fake the test plays the codec's part through:
// its callbacks are the production ones, called from the test. Time is a
// virtual clock: a wait the decoder makes runs to its deadline (nothing
// arrives while the host holds the buffers) and advances the clock by
// exactly that much, so how long one call holds the decoding thread is
// measured, not sampled.
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
#include <time.h>

#define AVERROR(e) (-(e))
#define AVERROR_EXTERNAL (-0x6C787445)
#define AVERROR_EOF (-0x20464F45)
#define AV_NOPTS_VALUE INT64_MIN
#define AV_LOG_ERROR 16
#define AV_LOG_WARNING 24
#define AV_LOG_INFO 32
#define AV_LOG_VERBOSE 40
#define AV_LOG_DEBUG 48
#define AV_LOG_TRACE 56
#define FFMIN(a, b) ((a) > (b) ? (b) : (a))
#define av_err2str(e) "error"
#define AV_FIFO_FLAG_AUTO_GROW 1
#define AVDISCARD_NONREF 8
// The Dolby Vision packet filter is not what this harness exercises.
#define CONFIG_HEVC_MEDIACODEC_DECODER 0

// How long one call may hold the decoding thread while the drain is stalled:
// whoever waits on that thread waits this out at most.
#define HOLD_BOUND_US 10000
// How often a polled codec may be asked while the drain is stalled: once per
// 8 ms poll, against the host asking again after every EAGAIN.
#define MAX_POLLS_PER_SECOND 125

// --- virtual clock
static int64_t now_us;

static int64_t av_gettime_relative(void)
{
    return now_us;
}

// The asynchronous wait turns its deadline into a CLOCK_REALTIME instant.
static int fake_clock_gettime(struct timespec *ts)
{
    ts->tv_sec = now_us / 1000000;
    ts->tv_nsec = now_us % 1000000 * 1000;
    return 0;
}
#define clock_gettime(id, ts) fake_clock_gettime(ts)

// Nothing the codec reports arrives during a wait: the host holds its output
// buffers. The wait runs to its deadline.
static int fake_cond_timedwait(const struct timespec *until)
{
    int64_t deadline = (int64_t)until->tv_sec * 1000000 + until->tv_nsec / 1000;
    if (deadline > now_us)
        now_us = deadline;
    return ETIMEDOUT;
}

typedef struct AVRational { int num, den; } AVRational;
static const AVRational AV_TIME_BASE_Q = { 1, 1000000 };

typedef struct AVCodecContext {
    AVRational pkt_timebase;
    void *priv_data;
    int skip_frame;
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

typedef struct FFAMediaCodecOnAsyncNotifyCallback {
    void (*onAsyncInputAvailable)(FFAMediaCodec *codec, void *userdata, int32_t index);
    void (*onAsyncOutputAvailable)(FFAMediaCodec *codec, void *userdata, int32_t index,
                                   FFAMediaCodecBufferInfo *buffer_info);
    void (*onAsyncFormatChanged)(FFAMediaCodec *codec, void *userdata, FFAMediaFormat *format);
    void (*onAsyncError)(FFAMediaCodec *codec, void *userdata, int error, const char *detail);
} FFAMediaCodecOnAsyncNotifyCallback;

typedef pthread_mutex_t AVMutex;
typedef pthread_cond_t AVCond;
#define ff_mutex_lock(m)            pthread_mutex_lock(m)
#define ff_mutex_unlock(m)          pthread_mutex_unlock(m)
#define ff_cond_init(c, a)          pthread_cond_init(c, a)
#define ff_cond_destroy(c)          pthread_cond_destroy(c)
#define ff_cond_signal(c)           pthread_cond_signal(c)
#define ff_cond_broadcast(c)        pthread_cond_broadcast(c)
#define ff_cond_timedwait(c, m, t)  fake_cond_timedwait(t)

// The queues the codec's callbacks fill.
typedef struct AVFifo {
    size_t elem_size, capacity, head, count;
    uint8_t *buf;
} AVFifo;

static AVFifo *av_fifo_alloc2(size_t nb_elems, size_t elem_size, unsigned flags)
{
    AVFifo *f = calloc(1, sizeof(*f));
    f->elem_size = elem_size;
    f->capacity = 64;
    f->buf = calloc(f->capacity, elem_size);
    return f;
}

static void av_fifo_freep2(AVFifo **f)
{
    if (*f)
        free((*f)->buf);
    free(*f);
    *f = NULL;
}

static int av_fifo_write(AVFifo *f, const void *buf, size_t nb_elems)
{
    if (nb_elems != 1 || f->count == f->capacity)
        return AVERROR(ENOSPC);
    memcpy(f->buf + (f->head + f->count) % f->capacity * f->elem_size, buf, f->elem_size);
    f->count++;
    return 0;
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

static size_t av_fifo_can_read(const AVFifo *f)
{
    return f->count;
}

typedef struct MediaCodecDecContext {
    AVCodecContext *avctx;
    atomic_int refcount;
    atomic_int hw_buffer_count;
    FFAMediaCodec *codec;
    FFAMediaFormat *format;
    void *surface;
    int draining;
    int flushing;
    int eos;
    uint64_t output_buffer_count;
    ssize_t current_input_buffer;
    int input_since_flush;
    bool delay_flush;
    atomic_int serial;
    bool use_async;
    int async_mode;
    AVMutex async_lock;
    AVCond async_cond;
    AVFifo *async_input;
    AVFifo *async_output;
    int async_error;
    int async_generation;
    void (*wakeup)(void *opaque);
    void *wakeup_opaque;
    int wakeup_in_flight;
    int64_t pending_drop_pts[8];
    int pending_drops;
    // Presentation-order recovery (mediacodec_reorder.h) and the input-time
    // ring (0028) are not what this harness exercises.
    struct MediaCodecReorder { int unused; } reorder;
    unsigned input_times_next;
} MediaCodecDecContext;

typedef struct MediaCodecAsyncOutput {
    int32_t index;
    FFAMediaCodecBufferInfo info;
    int64_t output_ns;
} MediaCodecAsyncOutput;

typedef struct AVMediaCodecBufferTiming {
    int64_t input_ns, output_ns, dequeue_ns;
    int queued_outputs, held_buffers;
} AVMediaCodecBufferTiming;

// Pre-decode frame shedding (mediacodec_shed.h) is off: the host never asks
// for AVDISCARD_NONREF here.
typedef struct MediaCodecShedContext { int unused; } MediaCodecShedContext;

typedef struct MediaCodecH264DecContext {
    MediaCodecDecContext *ctx;
    AVPacket buffered_pkt;
    int delay_flush;
    int amlogic_mpeg2_api23_workaround;
    MediaCodecShedContext shed;
    int shed_inited;
} MediaCodecH264DecContext;

static int ff_mediacodec_shed_packet(MediaCodecShedContext *s, AVCodecContext *avctx, AVPacket *pkt)
{
    return 0;
}

static void ff_mediacodec_shed_observe(MediaCodecShedContext *s, AVCodecContext *avctx,
                                       const AVPacket *pkt)
{
}

static void ff_mediacodec_dec_input_dropped(AVCodecContext *avctx, MediaCodecDecContext *s,
                                            int64_t pts)
{
}

static void ff_mediacodec_reorder_flush(struct MediaCodecReorder *r)
{
}

static void mediacodec_dec_reorder_input(AVCodecContext *avctx, MediaCodecDecContext *s,
                                         int64_t pts, const uint8_t *data, int size)
{
}

static int64_t mediacodec_monotonic_ns(void)
{
    return 0;
}

static void mediacodec_dec_input_time_record(MediaCodecDecContext *s, int64_t pts)
{
}

static int64_t mediacodec_dec_input_time_find(const MediaCodecDecContext *s, int64_t pts)
{
    return 0;
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

// Error lines the decoder logged: what fills the host's log.
static int errors;
static void av_log(void *avctx, int level, const char *fmt, ...)
{
    if (level <= AV_LOG_ERROR)
        errors++;
}

// --- MediaCodec fake. The codec's own thread is the test: it offers slots,
// reports output through the production callbacks (asynchronous mode) or
// lines it up for the polled dequeues. Between those, the codec is starved:
// a polled dequeue waits out its whole timeout and finds nothing.

enum {
    INFO_TRY_AGAIN_LATER = -1,
    INFO_OUTPUT_FORMAT_CHANGED = -2,
    INFO_OUTPUT_BUFFERS_CHANGED = -3,
    EOS_FLAG = 4,
    SLOTS = 4,
};

static struct {
    int queued;              // input buffers queued, the EOS included
    int eos_queued;
    int sync_free_slots;     // polled mode: slots dequeueInputBuffer hands out
    int sync_next_slot;
    MediaCodecAsyncOutput sync_outputs[8];
    int sync_count, sync_head;
    int output_dequeues;     // polled mode: dequeueOutputBuffer calls
    int frames;              // frames the decoder handed on
    uint8_t buffer[4096];
} codec;

static ssize_t ff_AMediaCodec_dequeueInputBuffer(FFAMediaCodec *c, int64_t timeout)
{
    if (!codec.sync_free_slots) {
        now_us += timeout;
        return INFO_TRY_AGAIN_LATER;
    }
    codec.sync_free_slots--;
    return codec.sync_next_slot++ % SLOTS;
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
    if (flags & EOS_FLAG)
        codec.eos_queued++;
    return 0;
}

static ssize_t ff_AMediaCodec_dequeueOutputBuffer(FFAMediaCodec *c, FFAMediaCodecBufferInfo *info,
                                                  int64_t timeout)
{
    codec.output_dequeues++;
    if (codec.sync_head == codec.sync_count) {
        now_us += timeout;
        return INFO_TRY_AGAIN_LATER;
    }
    MediaCodecAsyncOutput *o = &codec.sync_outputs[codec.sync_head++];
    *info = o->info;
    return o->index;
}

static int ff_AMediaCodec_releaseOutputBuffer(FFAMediaCodec *c, size_t index, int render)
{
    return 0;
}

static int ff_AMediaCodec_getBufferFlagEndOfStream(FFAMediaCodec *c)
{
    return EOS_FLAG;
}

static int ff_AMediaCodec_flush(FFAMediaCodec *c)
{
    return 0;
}

static int ff_AMediaCodec_start(FFAMediaCodec *c)
{
    return 0;
}

static uint8_t *ff_AMediaCodec_getOutputBuffer(FFAMediaCodec *c, size_t index, size_t *size)
{
    *size = 0;
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

static int ff_AMediaCodec_setAsyncNotifyCallback(FFAMediaCodec *c,
                                                 const FFAMediaCodecOnAsyncNotifyCallback *cb,
                                                 void *userdata)
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

static int mediacodec_wrap_hw_buffer(AVCodecContext *avctx, MediaCodecDecContext *s,
                                     ssize_t index, FFAMediaCodecBufferInfo *info,
                                     const AVMediaCodecBufferTiming *timing,
                                     AVFrame *frame)
{
    atomic_fetch_add(&s->hw_buffer_count, 1);
    codec.frames++;
    return 0;
}

static int mediacodec_wrap_sw_buffer(AVCodecContext *avctx, MediaCodecDecContext *s,
                                     uint8_t *data, size_t size, ssize_t index,
                                     FFAMediaCodecBufferInfo *info, AVFrame *frame)
{
    codec.frames++;
    return 0;
}

// The host's packet source: `available` access units, then the end of the
// file.
static struct {
    int available;
    int64_t next_pts;
    uint8_t payload[64];
} host;

static int ff_decode_get_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    if (!host.available)
        return AVERROR_EOF;
    host.available--;
    *pkt = (AVPacket){ .data = host.payload, .size = sizeof(host.payload),
                       .pts = host.next_pts };
    host.next_pts += 3600;
    return 0;
}

static void av_packet_unref(AVPacket *pkt)
{
    *pkt = (AVPacket){ 0 };
}

#include "mediacodec_drain.inc"

// --- harness

static AVCodecContext avctx;
static MediaCodecDecContext ctx;
static MediaCodecH264DecContext priv;
static AVFrame frame;
static int surface_token;
static int wakeups;

static const char *mode(int async_mode)
{
    return async_mode ? "asynchronous" : "polled";
}

static void checkf(bool ok, int async_mode, const char *fmt, ...)
{
    va_list ap;
    if (ok)
        return;
    fprintf(stderr, "FAIL (%s): ", mode(async_mode));
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

// mpv's decoder_wakeup: the decoder filter runs again.
static void host_wakeup(void *opaque)
{
    wakeups++;
}

// A codec configured and started, with the decoder open on it through the
// production asynchronous setup, and the host's wakeup registered as mpv
// registers it.
static void open_codec(int async_mode)
{
    memset(&codec, 0, sizeof(codec));
    memset(&ctx, 0, sizeof(ctx));
    memset(&priv, 0, sizeof(priv));
    memset(&host, 0, sizeof(host));
    errors = 0;
    wakeups = 0;
    now_us = 1000000;
    pthread_mutex_init(&ctx.async_lock, NULL);
    ctx.avctx = &avctx;
    ctx.codec = (FFAMediaCodec *)&codec;
    ctx.surface = &surface_token;
    ctx.current_input_buffer = -1;
    atomic_init(&ctx.refcount, 1);
    ctx.use_async = async_mode;
    priv.ctx = &ctx;
    avctx = (AVCodecContext){ .pkt_timebase = { 1, 90000 }, .priv_data = &priv };
    mediacodec_dec_init_async(&avctx, &ctx);
    checkf(ctx.async_mode == async_mode, async_mode, "the codec opens in the mode asked for");
    ctx.wakeup = host_wakeup;
}

static void close_codec(void)
{
    mediacodec_dec_uninit_async(&ctx);
    pthread_mutex_destroy(&ctx.async_lock);
}

// The codec offering input slots: its callback, or room for the polled
// dequeue.
static void offer(int n)
{
    for (int i = 0; i < n; i++) {
        if (ctx.async_mode)
            mediacodec_dec_on_input_available(ctx.codec, &ctx, i);
        else
            codec.sync_free_slots++;
    }
}

// The codec reporting an output buffer of the current generation: a frame,
// or the empty buffer flagged with the end of stream.
static void deliver(int32_t index, int32_t size, uint32_t flags)
{
    FFAMediaCodecBufferInfo info = {
        .size = size, .flags = flags,
        .presentationTimeUs = mediacodec_dec_tag_pts(&ctx, 40000 * (index + 1)),
    };
    if (ctx.async_mode)
        mediacodec_dec_on_output_available(ctx.codec, &ctx, index, &info);
    else
        codec.sync_outputs[codec.sync_count++] = (MediaCodecAsyncOutput){ index, info, 0 };
}

// One avcodec_receive_frame() from the host, and how long it held the
// decoding thread.
static int decode(int64_t *held_us)
{
    int64_t start = now_us;
    int ret = mediacodec_receive_frame(&avctx, &frame);
    *held_us = now_us - start;
    return ret;
}

// The last access units of the file, then its end: the decoder queues the
// end of stream and starts draining. The codec decodes nothing more while
// the host holds its output buffers.
static void reach_the_end(int async_mode)
{
    int64_t held;
    offer(SLOTS);
    host.available = 2;
    wakeups = 0; // each offer woke the host
    int ret = decode(&held);
    checkf(ret == AVERROR(EAGAIN) && codec.eos_queued == 1 && ctx.draining, async_mode,
           "the end of the file starts the drain (ret %d, eos queued %d)", ret, codec.eos_queued);
    checkf(held <= HOLD_BOUND_US, async_mode,
           "the call that starts a drain the codec cannot finish holds the decoding thread "
           "%" PRId64 " ms, more than %d ms", held / 1000, HOLD_BOUND_US / 1000);
}

// --- cases

static void test_paused_at_the_end_polled(void)
{
    // mpv polls a synchronous codec: past its end of stream it asks again
    // after every EAGAIN (lavc_busy_policy, LAVC_BUSY_POLL). Paused for a
    // minute at the end, every call must give the decoding thread back
    // within one poll, and the polls must stay paced, not spin.
    int64_t held, worst = 0, start;
    int calls = 0;

    open_codec(0);
    reach_the_end(0);
    start = now_us;
    codec.output_dequeues = 0;
    errors = 0;
    while (now_us - start < 60 * 1000000LL) {
        int ret = decode(&held);
        checkf(ret == AVERROR(EAGAIN), 0, "a stalled drain answers EAGAIN (ret %d)", ret);
        if (held > worst)
            worst = held;
        calls++;
        checkf(calls <= 60 * 1000, 0, "the host spins: %d calls without a minute passing",
               calls);
    }
    checkf(worst <= HOLD_BOUND_US, 0,
           "a stalled drain holds the decoding thread %" PRId64 " ms, more than %d ms",
           worst / 1000, HOLD_BOUND_US / 1000);
    checkf(codec.output_dequeues <= 60 * MAX_POLLS_PER_SECOND, 0,
           "paused at the end, the codec is asked %d times a second, more than %d",
           codec.output_dequeues / 60, MAX_POLLS_PER_SECOND);
    checkf(!errors, 0, "paused at the end for a minute, the decoder logged %d errors", errors);

    // Resumed: the host lets the codec's buffers go and the drain ends.
    deliver(0, 1, 0);
    deliver(1, 0, EOS_FLAG);
    checkf(decode(&held) == 0 && codec.frames == 1, 0, "the last frame comes once resumed");
    checkf(decode(&held) == AVERROR_EOF, 0, "and the end of stream after it");
    close_codec();
}

static void test_paused_at_the_end_asynchronous(void)
{
    // mpv leaves an asynchronous codec alone until the codec wakes it
    // (LAVC_BUSY_WAKEUP). A call made meanwhile -- the decoder filter run for
    // any other reason -- returns at once, and nothing wakes the host while
    // the codec has nothing to report.
    int64_t held;

    open_codec(1);
    reach_the_end(1);
    for (int i = 0; i < 3; i++) {
        int ret = decode(&held);
        checkf(ret == AVERROR(EAGAIN), 1, "a stalled drain answers EAGAIN (ret %d)", ret);
        checkf(held <= HOLD_BOUND_US, 1,
               "a stalled drain holds the decoding thread %" PRId64 " ms, more than %d ms",
               held / 1000, HOLD_BOUND_US / 1000);
    }
    checkf(!wakeups, 1, "nothing wakes the host while the codec reports nothing (%d wakeups)",
           wakeups);
    checkf(!errors, 1, "a stalled drain logged %d errors", errors);

    // Resumed: the codec's last frame and its end of stream each wake the
    // host, which then takes them.
    deliver(0, 1, 0);
    checkf(wakeups == 1, 1, "the last frame wakes the host");
    checkf(decode(&held) == 0 && codec.frames == 1, 1, "and is handed on");
    deliver(1, 0, EOS_FLAG);
    checkf(wakeups == 2, 1, "the end of stream wakes the host");
    checkf(decode(&held) == AVERROR_EOF, 1, "and ends the drain");
    close_codec();
}

int main(void)
{
    test_paused_at_the_end_polled();
    test_paused_at_the_end_asynchronous();
    printf("PASS: a drain the codec cannot finish holds the decoding thread at most %d ms a "
           "call, a polled codec paused at the end is asked at most %d times a second without "
           "an error line, and the drain ends once the codec's last output comes\n",
           HOLD_BOUND_US / 1000, MAX_POLLS_PER_SECOND);
    return 0;
}
