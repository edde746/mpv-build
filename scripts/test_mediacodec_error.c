// Behavioral regressions for codec errors across a flush, on the production
// MediaCodec decode path extracted by test_mediacodec_error.sh. A flush used
// to clear the error the codec's callback had reported, and a flush or
// restart the codec failed went nowhere, since avcodec_flush_buffers()
// returns nothing; MediaCodec answers an error that reaches it during a flush
// through the flush alone. Either way the decoder could answer EAGAIN on a
// codec that would never answer again, so the host's hardware-decoding
// fallback never ran. The codec's error is now sticky: every later decoding
// call answers it.
//
// Every case drives what avcodec_receive_frame() reaches
// (mediacodec_receive_frame) for a host like mpv, which registers a wakeup;
// in polled mode the same registration makes the decoder answer EAGAIN
// rather than block. The MediaCodec wrapper is a fake the test plays the
// codec's part through: its callbacks are the production ones, called from
// the test.
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
#define ff_cond_timedwait(c, m, t)  pthread_cond_timedwait(c, m, t)

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
    uint64_t progress;
    uint64_t released;
    uint64_t stall_mark;
    int64_t stall_start;
    bool stalled;
    bool parked;
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

// The waits never block: the host registered a wakeup and nothing drains.
static int64_t av_gettime_relative(void)
{
    return 0;
}

static int errors;
static void av_log(void *avctx, int level, const char *fmt, ...)
{
    if (level <= AV_LOG_ERROR)
        errors++;
}

// --- MediaCodec fake. The codec's own thread is the test: it offers slots,
// reports output and errors through the production callbacks (asynchronous
// mode) or lines them up for the polled dequeues.

enum {
    INFO_TRY_AGAIN_LATER = -1,
    INFO_OUTPUT_FORMAT_CHANGED = -2,
    INFO_OUTPUT_BUFFERS_CHANGED = -3,
    EOS_FLAG = 4,
    SLOTS = 4,
};

static struct {
    int queued;              // access units queued since the case began
    int flushes, starts;
    int flush_status;        // what the next flush answers
    int start_status;        // what the next start answers
    int sync_free_slots;     // polled mode: slots dequeueInputBuffer hands out
    int sync_next_slot;
    MediaCodecAsyncOutput sync_outputs[8];
    int sync_count, sync_head;
    int frames;              // frames the decoder handed on
    uint8_t buffer[4096];
} codec;

static ssize_t ff_AMediaCodec_dequeueInputBuffer(FFAMediaCodec *c, int64_t timeout)
{
    if (!codec.sync_free_slots)
        return INFO_TRY_AGAIN_LATER;
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
    return 0;
}

static int ff_AMediaCodec_getBufferFlagEndOfStream(FFAMediaCodec *c)
{
    return EOS_FLAG;
}

static int ff_AMediaCodec_flush(FFAMediaCodec *c)
{
    codec.flushes++;
    return codec.flush_status;
}

static int ff_AMediaCodec_start(FFAMediaCodec *c)
{
    codec.starts++;
    return codec.start_status;
}

static uint8_t *ff_AMediaCodec_getOutputBuffer(FFAMediaCodec *c, size_t index, size_t *size)
{
    *size = sizeof(codec.buffer);
    return codec.buffer;
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

// The host's packet source: the demuxer has `available` access units ready.
static struct {
    int available;
    int64_t next_pts;
    uint8_t payload[64];
} host;

static int ff_decode_get_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    if (!host.available)
        return AVERROR(EAGAIN);
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

#include "mediacodec_error.inc"

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
static MediaCodecH264DecContext priv;
static AVFrame frame;
static int surface_token;
static int wakeups;

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
    check(ctx.async_mode == async_mode, "the codec opens in the mode asked for");
    ctx.wakeup = host_wakeup;
}

static void close_codec(void)
{
    mediacodec_dec_uninit_async(&ctx);
    pthread_mutex_destroy(&ctx.async_lock);
}

static const char *mode(int async_mode)
{
    return async_mode ? "asynchronous" : "polled";
}

static void checkf(bool ok, int async_mode, const char *scenario)
{
    char line[256];
    snprintf(line, sizeof(line), "%s (%s)", scenario, mode(async_mode));
    check(ok, line);
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

// The codec reporting a decoded frame of the current generation.
static void deliver(int32_t index)
{
    FFAMediaCodecBufferInfo info = {
        .size = 1, .presentationTimeUs = mediacodec_dec_tag_pts(&ctx, 40000),
    };
    if (ctx.async_mode)
        mediacodec_dec_on_output_available(ctx.codec, &ctx, index, &info);
    else
        codec.sync_outputs[codec.sync_count++] = (MediaCodecAsyncOutput){ index, info, 0 };
}

// One avcodec_receive_frame() from the host.
static int decode(void)
{
    return mediacodec_receive_frame(&avctx, &frame);
}

// What a seek or track switch does to the decoder: avcodec_flush_buffers()
// reaches mediacodec_decode_flush, which drops the buffered packet, calls the
// production flush entry and ignores what it returns.
static void seek(void)
{
    av_packet_unref(&priv.buffered_pkt);
    ff_mediacodec_dec_flush(&avctx, &ctx);
}

static bool is_error(int ret)
{
    return ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF;
}

// mpv falls back to software decoding after three errors in a row
// (hwdec-software-fallback), so one error is not enough: the decoder has to
// keep answering it.
static bool answers_error_three_times(void)
{
    return is_error(decode()) && is_error(decode()) && is_error(decode());
}

// Every slot the codec offers taken with an access unit, the way the host
// feeds a codec that has not answered yet: both ports then answer EAGAIN.
static void fill_codec(int async_mode)
{
    int queued = codec.queued;
    offer(SLOTS);
    host.available = SLOTS + 2;
    checkf(decode() == AVERROR(EAGAIN) && codec.queued == queued + SLOTS,
           async_mode, "the codec takes an access unit in every slot it offers");
    wakeups = 0; // each offer woke the host
}

// --- cases

static void test_reported_error_survives_a_seek(void)
{
    // The codec's error callback fires, then the host seeks before it
    // decodes again: the flush used to clear the error, and the restarted
    // codec, still failed, would answer nothing.
    open_codec(1);
    fill_codec(1);
    mediacodec_dec_on_error(ctx.codec, &ctx, AVERROR(EFAULT), "work failed");
    check(wakeups == 1, "the codec's error wakes the host");
    seek();
    check(codec.flushes == 1 && codec.starts == 1, "the seek flushed and restarted the codec");
    offer(SLOTS);
    host.available = SLOTS;
    check(answers_error_three_times(),
          "an error the codec reported before a seek is answered after it, every time");
    close_codec();
}

static void test_failed_flush_is_answered(int async_mode)
{
    // MediaCodec answers an error that reaches it during a flush through the
    // flush alone; avcodec_flush_buffers() returns nothing, so the next
    // decoding call has to. A frame the codec finished before the failed
    // flush is not decoded on from the failed codec.
    open_codec(async_mode);
    fill_codec(async_mode);
    deliver(0);
    wakeups = 0;
    codec.flush_status = AVERROR(EINVAL);
    seek();
    checkf(codec.flushes == 1, async_mode, "the seek reached the codec");
    checkf(answers_error_three_times() && !codec.frames, async_mode,
           "a flush the codec failed is answered as an error, every time");
    if (async_mode)
        checkf(wakeups == 1, async_mode, "a failed flush wakes the host");

    // A second seek does not clear it.
    codec.flush_status = 0;
    seek();
    offer(SLOTS);
    checkf(is_error(decode()), async_mode, "and a later seek does not clear it");
    close_codec();
}

static void test_failed_restart_is_answered(void)
{
    // Asynchronous mode restarts the codec after a flush; a restart that
    // fails leaves a codec that will offer nothing.
    open_codec(1);
    fill_codec(1);
    codec.start_status = AVERROR(EINVAL);
    seek();
    check(codec.starts == 1, "the seek restarted the codec");
    check(answers_error_three_times(), "a restart the codec failed is answered as an error");
    close_codec();
}

static void test_clean_seek_decodes_on(int async_mode)
{
    // No error anywhere: the flush leaves nothing behind, and the codec's
    // first frame after it is handed on.
    open_codec(async_mode);
    fill_codec(async_mode);
    seek();
    fill_codec(async_mode);
    deliver(0);
    checkf(decode() == 0 && codec.frames == 1 && !errors, async_mode,
           "a seek without an error decodes on");
    close_codec();
}

int main(void)
{
    test_reported_error_survives_a_seek();
    test_failed_flush_is_answered(1);
    test_failed_flush_is_answered(0);
    test_failed_restart_is_answered();
    test_clean_seek_decodes_on(1);
    test_clean_seek_decodes_on(0);
    puts("PASS: a MediaCodec error is never lost across a flush: an error the codec reported "
         "before a seek, a flush it failed and a restart it failed are each the decoder's "
         "answer from then on, and a clean seek decodes on");
    return 0;
}
