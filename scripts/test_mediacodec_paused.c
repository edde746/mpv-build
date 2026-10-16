// Behavioral regressions for a paused player on a MediaCodec decoder, on the
// production code test_mediacodec_paused.sh extracts: mpv's decoder filter
// step (lavc_process) and its wakeup registration, driving FFmpeg's
// MediaCodec decode path. A paused player holds the codec's output buffers --
// the frames queued ahead mid-file, the last frames once the end of stream
// is sent -- so the codec can make no progress until the player gives one
// back. mpv used to ask a synchronous codec again after every EAGAIN for as
// long as that lasted, once per 8 ms dequeue: about 120 times a second, 5-14%
// of a core on a SHIELD and a Fire TV, for nothing. Now the decoder wakes the
// filter itself: at once while the codec may still be working, and once it
// has handed out nothing for a second with the player holding its buffers,
// only when the player gives one back. A seek or a close is the player's own
// call and needs no wakeup. Every call still hands the decoding thread back
// within one poll, which is what a seek, a property read or the teardown
// waits on that thread at most.
//
// The MediaCodec wrapper is a fake the test plays the codec's part through: a
// codec with a few output buffers that decodes a queued access unit into a
// free one at once, unless told it is still working. Its asynchronous
// callbacks are the production ones, called from the test. libavcodec's
// send/receive (decode.c) and mpv's vd_lavc glue are reduced to what the
// decoder sees: a one-packet input buffer, and frames the player holds until
// it gives them back through av_mediacodec_release_buffer, as the video
// output does. Time is a virtual clock the codec's waits advance; the filter
// runs only when something woke it, and a stretch in which nothing does
// passes at once.
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

// How long one run of the decoder filter may hold the decoding thread:
// whoever waits on that thread waits this out at most.
#define HOLD_BOUND_US 10000
// Paused with the codec's buffers held, the decoder is not asked at all from
// this long after the codec stalled on.
#define IDLE_AFTER_US 2000000
// The codec's buffers and input slots, and the frames the player's queue
// asks for: more than the codec has, as mpv's decoder queue on a device.
#define OUTPUT_BUFFERS 4
#define SLOTS 4
#define PLAYER_QUEUE 12

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

// Nothing the codec reports arrives during a wait: the test is the codec's
// thread. The wait runs to its deadline.
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

// The frame a decoded buffer is wrapped in: the buffer the player gives back.
typedef struct AVFrame { struct MediaCodecBuffer *buffer; } AVFrame;

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
#define ff_cond_wait(c, m)          pthread_cond_wait(c, m)
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

typedef struct MediaCodecBuffer {
    MediaCodecDecContext *ctx;
    ssize_t index;
    int64_t pts;
    atomic_int released;
    int serial;
} MediaCodecBuffer;
typedef struct MediaCodecBuffer AVMediaCodecBuffer;

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

static MediaCodecDecContext mc;
static MediaCodecDecContext *ff_mediacodec_dec_from_avctx(AVCodecContext *avctx)
{
    return &mc;
}

// --- MediaCodec fake. The codec's own thread is the test. A queued access
// unit becomes a frame in any free output buffer at once, unless the codec is
// still working (working_until), when one with a deep pipeline may still take
// input (intake_us); after the end-of-stream input it reports the end of
// stream once its frames are out. Polled, the dequeues find what is ready or
// wait out their whole timeout; asynchronously, pump() reports it through the
// production callbacks.

enum {
    INFO_TRY_AGAIN_LATER = -1,
    INFO_OUTPUT_FORMAT_CHANGED = -2,
    INFO_OUTPUT_BUFFERS_CHANGED = -3,
    EOS_FLAG = 4,
    EOS_INDEX = 100,
};

static struct {
    int in_codec;            // access units queued and not yet out as frames
    int slots_out;           // input slots handed out and not yet queued
    int held;                // output buffers handed out and not given back
    bool eos_queued, eos_out;
    int64_t working_until;   // nothing comes out before this
    int64_t intake_from;     // while working, one more access unit fits in
    int64_t intake_us;       // every intake_us from intake_from on
    int late_after;          // from this many frames on the codec takes...
    int64_t late_us;         // ...this long for each next output
    int frames_out;
    int64_t out_at;          // when the last frame came out
    int dequeues;            // polled dequeue calls, both ports
    uint8_t buffer[4096];
} codec;

static bool frame_ready(void)
{
    return codec.in_codec && codec.held < OUTPUT_BUFFERS;
}

static bool eos_ready(void)
{
    return codec.eos_queued && !codec.in_codec && !codec.eos_out;
}

// The access units a codec still working has taken into its pipeline beyond
// its slots.
static int swallowed(void)
{
    int64_t until = FFMIN(now_us, codec.working_until);
    if (!codec.intake_us || until <= codec.intake_from)
        return 0;
    return (until - codec.intake_from) / codec.intake_us;
}

static bool slot_free(void)
{
    return !codec.eos_queued && codec.in_codec + codec.slots_out < SLOTS + swallowed();
}

static int64_t mediacodec_dec_tag_pts(const MediaCodecDecContext *s, int64_t pts);

// The next output: a frame, or the empty buffer flagged with the end of
// stream. Only called when one is ready and the codec is not working.
static ssize_t take_output(FFAMediaCodecBufferInfo *info)
{
    if (!frame_ready()) {
        codec.eos_out = true;
        *info = (FFAMediaCodecBufferInfo){ .flags = EOS_FLAG,
                                           .presentationTimeUs = mediacodec_dec_tag_pts(&mc, 0) };
        return EOS_INDEX;
    }
    codec.in_codec--;
    codec.held++;
    codec.frames_out++;
    codec.out_at = now_us;
    if (codec.late_after && codec.frames_out >= codec.late_after)
        codec.working_until = now_us + codec.late_us;
    *info = (FFAMediaCodecBufferInfo){
        .size = 1, .presentationTimeUs = mediacodec_dec_tag_pts(&mc, 40000 * codec.frames_out),
    };
    return codec.frames_out % OUTPUT_BUFFERS;
}

static void mediacodec_dec_on_input_available(FFAMediaCodec *codec, void *userdata,
                                              int32_t index);
static void mediacodec_dec_on_output_available(FFAMediaCodec *codec, void *userdata,
                                               int32_t index,
                                               FFAMediaCodecBufferInfo *info);

// Asynchronous mode: report what is ready.
static void pump(void)
{
    if (!mc.async_mode)
        return;
    while (now_us >= codec.working_until && (frame_ready() || eos_ready())) {
        FFAMediaCodecBufferInfo info;
        ssize_t index = take_output(&info);
        mediacodec_dec_on_output_available(mc.codec, &mc, index, &info);
    }
    while (slot_free()) {
        codec.slots_out++;
        mediacodec_dec_on_input_available(mc.codec, &mc, codec.slots_out);
    }
}

static ssize_t ff_AMediaCodec_dequeueInputBuffer(FFAMediaCodec *c, int64_t timeout)
{
    codec.dequeues++;
    if (!slot_free()) {
        now_us += timeout;
        return INFO_TRY_AGAIN_LATER;
    }
    codec.slots_out++;
    return codec.slots_out;
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
    codec.slots_out--;
    if (flags & EOS_FLAG)
        codec.eos_queued = true;
    else
        codec.in_codec++;
    pump();
    return 0;
}

static ssize_t ff_AMediaCodec_dequeueOutputBuffer(FFAMediaCodec *c, FFAMediaCodecBufferInfo *info,
                                                  int64_t timeout)
{
    codec.dequeues++;
    if ((!frame_ready() && !eos_ready()) || codec.working_until > now_us + timeout) {
        now_us += timeout;
        return INFO_TRY_AGAIN_LATER;
    }
    if (codec.working_until > now_us)
        now_us = codec.working_until;
    return take_output(info);
}

static int ff_AMediaCodec_releaseOutputBuffer(FFAMediaCodec *c, size_t index, int render)
{
    if (index != EOS_INDEX)
        codec.held--;
    pump();
    return 0;
}

static int ff_AMediaCodec_releaseOutputBufferAtTime(FFAMediaCodec *c, size_t index, int64_t time)
{
    return ff_AMediaCodec_releaseOutputBuffer(c, index, 1);
}

static int ff_AMediaCodec_getBufferFlagEndOfStream(FFAMediaCodec *c)
{
    return EOS_FLAG;
}

static int ff_AMediaCodec_flush(FFAMediaCodec *c)
{
    codec.in_codec = codec.slots_out = codec.held = 0;
    codec.eos_queued = codec.eos_out = false;
    codec.working_until = 0;
    return 0;
}

static int ff_AMediaCodec_start(FFAMediaCodec *c)
{
    pump();
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

// What the production wrapper keeps of a decoded buffer: the index to give
// back, the flush generation it belongs to, and its reference on the decoder.
static int mediacodec_wrap_hw_buffer(AVCodecContext *avctx, MediaCodecDecContext *s,
                                     ssize_t index, FFAMediaCodecBufferInfo *info,
                                     const AVMediaCodecBufferTiming *timing,
                                     AVFrame *frame)
{
    MediaCodecBuffer *buffer = calloc(1, sizeof(*buffer));
    buffer->ctx = s;
    buffer->index = index;
    buffer->serial = atomic_load(&s->serial);
    atomic_init(&buffer->released, 0);
    atomic_fetch_add(&s->hw_buffer_count, 1);
    atomic_fetch_add(&s->refcount, 1);
    frame->buffer = buffer;
    return 0;
}

static int mediacodec_wrap_sw_buffer(AVCodecContext *avctx, MediaCodecDecContext *s,
                                     uint8_t *data, size_t size, ssize_t index,
                                     FFAMediaCodecBufferInfo *info, AVFrame *frame)
{
    return AVERROR_EXTERNAL;
}

// --- libavcodec's decode.c, reduced: avcodec_send_packet() fills a
// one-packet buffer the decoder's ff_decode_get_packet() takes from, and a
// NULL packet starts the drain. (The real send also runs the decoder once;
// that only adds calls.)
static struct {
    bool full, draining;
    AVPacket pkt;
    uint8_t payload[64];
} lavc_in;

static int ff_decode_get_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    if (lavc_in.full) {
        lavc_in.full = false;
        *pkt = lavc_in.pkt;
        return 0;
    }
    return lavc_in.draining ? AVERROR_EOF : AVERROR(EAGAIN);
}

static void av_packet_unref(AVPacket *pkt)
{
    *pkt = (AVPacket){ 0 };
}

// A frame's last reference gone (mediacodec_buffer_release); the test owns
// the decoder itself.
static void ff_mediacodec_dec_unref(MediaCodecDecContext *s)
{
    atomic_fetch_sub(&s->refcount, 1);
}

#include "mediacodec_paused_decoder.inc"

// --- mpv's filter graph, reduced to the decoder filter. The input pin hands
// out the demuxer's packets and then, at the end of the file, its EOF; the
// output pin goes to the player, which keeps every frame until it gives the
// buffer back, and whose queue asks for more while it has room.

enum mp_frame_type { MP_FRAME_NONE = 0, MP_FRAME_EOF, MP_FRAME_PACKET, MP_FRAME_VIDEO };
struct mp_frame { enum mp_frame_type type; void *data; };
#define MP_EOF_FRAME ((struct mp_frame){ .type = MP_FRAME_EOF })

struct demux_packet { int64_t pts; };
struct mp_pin { int unused; };
struct mp_filter { struct mp_pin **ppins; void *packet_pool; };

static struct {
    struct mp_pin in, out;
    struct mp_pin *pins[2];
    struct mp_filter f;
    bool woken;
    int warnings, failed;
    struct mp_frame unread;
    int packets;           // the demuxer's access units still to come
    bool at_end, eof_read; // the file ends after them; its EOF was handed out
    struct demux_packet packet;
} graph;

static struct {
    MediaCodecBuffer *held[PLAYER_QUEUE];
    int n;
    bool eof;
} player;

static bool mp_pin_in_needs_data(struct mp_pin *p)
{
    return player.n < PLAYER_QUEUE;
}

static void mp_pin_in_write(struct mp_pin *p, struct mp_frame frame)
{
    if (frame.type == MP_FRAME_EOF) {
        player.eof = true;
        return;
    }
    player.held[player.n++] = frame.data;
    if (player.n < PLAYER_QUEUE)
        graph.woken = true;
}

static struct mp_frame mp_pin_out_read(struct mp_pin *p)
{
    struct mp_frame frame = graph.unread;
    if (frame.type) {
        graph.unread = (struct mp_frame){0};
        return frame;
    }
    if (graph.packets) {
        graph.packets--;
        graph.packet.pts += 3600;
        return (struct mp_frame){ .type = MP_FRAME_PACKET, .data = &graph.packet };
    }
    if (graph.at_end && !graph.eof_read) {
        graph.eof_read = true;
        return MP_EOF_FRAME;
    }
    return (struct mp_frame){0};
}

static void mp_pin_out_unread(struct mp_pin *p, struct mp_frame frame)
{
    graph.unread = frame;
}

static void mp_frame_unref(struct mp_frame *f) { *f = (struct mp_frame){0}; }
static void mp_filter_wakeup(struct mp_filter *f) { graph.woken = true; }
static void mp_filter_internal_mark_progress(struct mp_filter *f) { graph.woken = true; }
static void mp_filter_internal_mark_failed(struct mp_filter *f) { graph.failed++; }
static void demux_packet_pool_push(void *pool, struct demux_packet *pkt) {}
#define MP_ERR(f, ...) (graph.failed++)
#define MP_WARN(f, ...) (graph.warnings++)
#define MP_VERBOSE(f, ...) ((void)0)

#include "mediacodec_paused_host.inc"

// vd_lavc's send and receive, reduced to the libavcodec calls they make.
static int host_send(struct mp_filter *f, struct demux_packet *pkt)
{
    if (lavc_in.draining)
        return AVERROR_EOF;
    if (!pkt) {
        lavc_in.draining = true;
        return 0;
    }
    if (lavc_in.full)
        return AVERROR(EAGAIN);
    lavc_in.full = true;
    lavc_in.pkt = (AVPacket){ .data = lavc_in.payload, .size = sizeof(lavc_in.payload),
                              .pts = pkt->pts };
    return 0;
}

static AVCodecContext lavc;

static int host_receive(struct mp_filter *f, struct mp_frame *res)
{
    AVFrame frame = {0};
    int ret = mediacodec_receive_frame(&lavc, &frame);
    if (ret == 0)
        *res = (struct mp_frame){ .type = MP_FRAME_VIDEO, .data = frame.buffer };
    return ret;
}

// What vd_lavc's init_avctx has in hand when it registers its wakeup.
struct vd_ctx { struct lavc_state state; };
static struct vd_ctx vd_priv;

static void register_wakeup(void)
{
    struct vd_ctx *ctx = &vd_priv;
    AVCodecContext *avctx = &lavc;
    struct mp_filter *vd = &graph.f;
#include "mediacodec_paused_register.inc"
}

// --- harness

static MediaCodecH264DecContext priv;
static int surface_token;
static int64_t worst_hold;

static const char *mode(void)
{
    return mc.async_mode ? "asynchronous" : "polled";
}

static void checkf(bool ok, const char *fmt, ...)
{
    va_list ap;
    if (ok)
        return;
    fprintf(stderr, "FAIL (%s): ", mode());
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

// A codec configured and started with the decoder open on it, the host's
// wakeup registered as vd_lavc registers it, and a file of `packets` access
// units ahead (then its end, if `at_end`) waking the filter.
static void open_codec(int async_mode, int packets, bool at_end)
{
    memset(&codec, 0, sizeof(codec));
    memset(&mc, 0, sizeof(mc));
    memset(&priv, 0, sizeof(priv));
    memset(&lavc_in, 0, sizeof(lavc_in));
    memset(&graph, 0, sizeof(graph));
    memset(&player, 0, sizeof(player));
    memset(&vd_priv, 0, sizeof(vd_priv));
    errors = 0;
    worst_hold = 0;
    now_us = 1000000;
    // As ff_mediacodec_dec_init sets them up.
    pthread_mutex_init(&mc.async_lock, NULL);
    pthread_cond_init(&mc.async_cond, NULL);
    mc.avctx = &lavc;
    mc.codec = (FFAMediaCodec *)&codec;
    mc.surface = &surface_token;
    mc.current_input_buffer = -1;
    atomic_init(&mc.refcount, 1);
    atomic_init(&mc.serial, 1);
    mc.use_async = async_mode;
    priv.ctx = &mc;
    lavc = (AVCodecContext){ .pkt_timebase = { 1, 90000 }, .priv_data = &priv };
    mediacodec_dec_init_async(&lavc, &mc);
    checkf(mc.async_mode == async_mode, "the codec opens in the mode asked for");
    register_wakeup();
    checkf(vd_priv.state.busy_policy != LAVC_BUSY_UNEXPECTED,
           "mpv knows a MediaCodec decoder answers EAGAIN on both ports");

    graph.pins[0] = &graph.in;
    graph.pins[1] = &graph.out;
    graph.f.ppins = graph.pins;
    graph.packets = packets;
    graph.at_end = at_end;
    graph.woken = true;
    pump();
}

static void close_codec(void)
{
    for (int i = 0; i < player.n; i++)
        free(player.held[i]);
    mediacodec_dec_uninit_async(&mc);
    pthread_cond_destroy(&mc.async_cond);
    pthread_mutex_destroy(&mc.async_lock);
}

// mpv's decoder thread: the filter runs whenever something woke it, until
// `until`; a stretch in which nothing does passes at once. Returns the runs.
static int run_until(int64_t until)
{
    int runs = 0;
    while (graph.woken && now_us < until) {
        int64_t start = now_us;
        graph.woken = false;
        lavc_process(&graph.f, &vd_priv.state, host_send, host_receive);
        if (now_us - start > worst_hold)
            worst_hold = now_us - start;
        runs++;
        checkf(runs <= 100000, "the decoder filter spins: %d runs in %" PRId64 " ms",
               runs, (until - start) / 1000);
    }
    if (now_us < until)
        now_us = until;
    return runs;
}

// How the player gives its oldest frame's buffer back: shown at a time, as
// vo_mediacodec renders; shown at once, as the GL output does; or dropped
// unshown, the buffer going back when the frame's last reference does.
enum give_back_how { RENDERED_AT_A_TIME, RENDERED, DROPPED };
static const char *const give_back_names[] = { "rendered at a time", "rendered", "dropped" };

static void give_back_as(enum give_back_how how)
{
    MediaCodecBuffer *buffer = player.held[0];
    checkf(player.n > 0, "the player holds a frame to give back");
    memmove(player.held, player.held + 1, --player.n * sizeof(player.held[0]));
    if (how == RENDERED_AT_A_TIME)
        av_mediacodec_render_buffer_at_time(buffer, now_us * 1000);
    else if (how == RENDERED)
        av_mediacodec_release_buffer(buffer, 1);
    mediacodec_buffer_release(buffer, NULL);
    if (player.n == PLAYER_QUEUE - 1)
        graph.woken = true;
}

static void give_back(void)
{
    give_back_as(RENDERED);
}

// The player paused, for a minute, with the codec's buffers all held: past
// IDLE_AFTER_US the decoder is not asked at all; a run of the decoder filter
// for any other reason meanwhile (mpv runs it whenever something else in its
// graph moves) asks once and leaves it alone again; and no call holds the
// thread longer than one poll, which is what a close waits on that thread.
static void hold_paused(const char *where)
{
    int64_t start = now_us;
    int dequeues = codec.dequeues;
    run_until(start + IDLE_AFTER_US);
    int settle = codec.dequeues - dequeues;
    dequeues = codec.dequeues;
    int idle = run_until(start + 60 * 1000000LL);
    checkf(!idle && codec.dequeues == dequeues,
           "paused %s, the decoder filter ran %d times and the codec was asked %d times "
           "in the %d s after the first %d s, at %.0f a second (%d times before)",
           where, idle, codec.dequeues - dequeues, (60000000 - IDLE_AFTER_US) / 1000000,
           IDLE_AFTER_US / 1000000,
           (codec.dequeues - dequeues) / ((60000000.0 - IDLE_AFTER_US) / 1000000), settle);
    // One run: an output dequeue, an input dequeue, the output dequeue that
    // waits its poll out.
    graph.woken = true;
    int other = run_until(now_us + 10 * 1000000LL);
    checkf(other == 1 && codec.dequeues - dequeues <= 3,
           "paused %s, a run for another reason set the decoder filter running %d times "
           "and the codec was asked %d times", where, other, codec.dequeues - dequeues);
    checkf(worst_hold <= HOLD_BOUND_US,
           "paused %s, one call held the decoding thread %" PRId64 " ms, more than %d ms",
           where, worst_hold / 1000, HOLD_BOUND_US / 1000);
    checkf(!errors && !graph.warnings && !graph.failed,
           "paused %s for a minute, the decoder logged %d errors and mpv %d warnings",
           where, errors, graph.warnings + graph.failed);
}

// --- cases

static void test_paused_mid_file(int async_mode)
{
    open_codec(async_mode, 1000, false);
    run_until(now_us + 100000);
    checkf(player.n == OUTPUT_BUFFERS && codec.in_codec == SLOTS,
           "the codec's buffers end up with the paused player and its input fills "
           "(player %d, queued %d)", player.n, codec.in_codec);

    // Paused, then resumed, once for each way the player gives a buffer
    // back: that buffer wakes the decoder (a polled codec through the
    // release, an asynchronous one through its report), and the codec's next
    // frame, which takes it 50 ms to decode into it, is picked up within one
    // poll of being ready though the codec stalled long ago.
    for (int how = RENDERED_AT_A_TIME; how <= DROPPED; how++) {
        hold_paused("mid-file");
        int frames = codec.frames_out;
        int64_t ready = now_us + 50000;
        codec.working_until = ready;
        give_back_as(how);
        checkf(async_mode || graph.woken, "a buffer %s wakes the decoder", give_back_names[how]);
        run_until(ready);
        pump();  // asynchronous: the codec reports the frame once it has it
        run_until(now_us + 100000);
        checkf(codec.frames_out == frames + 1 && player.n == OUTPUT_BUFFERS &&
               codec.out_at <= ready + OUTPUT_DEQUEUE_TIMEOUT_US,
               "and after a buffer %s, the codec's next frame comes within one poll "
               "(%d frames, player %d, %" PRId64 " ms after it was ready)", give_back_names[how],
               codec.frames_out - frames, player.n, (codec.out_at - ready) / 1000);
    }
    close_codec();
}

static void test_paused_at_the_end(int async_mode)
{
    // Two access units more than the codec has buffers: it sends the end of
    // stream with its last two frames stuck behind the player.
    open_codec(async_mode, OUTPUT_BUFFERS + 2, true);
    run_until(now_us + 100000);
    checkf(player.n == OUTPUT_BUFFERS && codec.eos_queued && mc.draining && !player.eof,
           "the end of the file starts a drain the codec cannot finish");
    hold_paused("at the end");

    // Resumed: each buffer given back brings the codec's next frame, then the
    // end of stream reaches the player.
    for (int i = 0; i < OUTPUT_BUFFERS + 2 && !player.eof; i++) {
        give_back();
        checkf(graph.woken, "giving a buffer back wakes the decoder");
        run_until(now_us + 100000);
    }
    checkf(player.eof && codec.frames_out == OUTPUT_BUFFERS + 2,
           "once resumed the drain ends with every frame (%d)", codec.frames_out);
    close_codec();
}

static void test_codec_finishing_its_last_frames(void)
{
    // A synchronous codec that takes 600 ms for each of its last frames
    // while the player holds what it has: the end of stream has been sent,
    // both of its ports are busy, and nothing but the decoder asking again
    // brings the rest. It is asked until they come, without the player
    // giving anything back, though nothing moves for longer than a second
    // on the input side.
    open_codec(0, 3, true);
    codec.late_after = 1;
    codec.late_us = 600000;
    int64_t start = now_us;
    run_until(start + 2500000);
    checkf(player.eof && codec.frames_out == 3 && player.n == 3,
           "a codec still working on its last frames is asked until its end of stream "
           "comes (frames %d, eof %d)", codec.frames_out, player.eof);
    checkf(worst_hold <= HOLD_BOUND_US,
           "one call held the decoding thread %" PRId64 " ms, more than %d ms",
           worst_hold / 1000, HOLD_BOUND_US / 1000);
    close_codec();
}

static void test_codec_falling_behind_with_no_buffer_held(void)
{
    // A synchronous codec that falls behind while playing: the player has
    // shown every frame and given its buffer back, and the codec takes a
    // second and a half for the next one. With none of its buffers held,
    // nothing but the decoder asking again brings it.
    open_codec(0, 1000, false);
    run_until(now_us + 100000);
    codec.working_until = now_us + 1500000;
    while (player.n)
        give_back();
    graph.woken = true;  // the player's queue asks for more
    int frames = codec.frames_out;
    run_until(now_us + 2000000);
    checkf(codec.frames_out > frames,
           "a codec falling behind with no buffer held is asked until its next frame comes");
    checkf(worst_hold <= HOLD_BOUND_US,
           "one call held the decoding thread %" PRId64 " ms, more than %d ms",
           worst_hold / 1000, HOLD_BOUND_US / 1000);
    close_codec();
}

static void test_codec_taking_input_without_output(void)
{
    // A synchronous codec with a deep pipeline, paused with all but one of
    // its buffers held (one given back as the player showed it): it keeps
    // taking an access unit every 200 ms but hands out no frame for a second
    // and a half. Taking input is progress, so it is asked until its frame
    // comes and the frame is picked up within one poll of being ready; then,
    // with every buffer held, it is left alone like any paused codec.
    open_codec(0, 1000, false);
    run_until(now_us + 100000);
    int64_t ready = now_us + 1500000;
    codec.working_until = ready;
    codec.intake_from = now_us;
    codec.intake_us = 200000;
    give_back();
    graph.woken = true;  // the player's queue asks for more
    int frames = codec.frames_out, queued = codec.in_codec;
    run_until(ready + 100000);
    checkf(codec.in_codec + (codec.frames_out - frames) - queued >= 5,
           "the codec takes input while it works (%d access units)",
           codec.in_codec + (codec.frames_out - frames) - queued);
    checkf(codec.frames_out == frames + 1 && codec.out_at <= ready + OUTPUT_DEQUEUE_TIMEOUT_US,
           "a codec taking input without handing out a frame is asked until its frame comes "
           "(%d frames, %" PRId64 " ms after it was ready)",
           codec.frames_out - frames, (codec.out_at - ready) / 1000);
    hold_paused("after taking input without output");
    close_codec();
}

static void test_seek_while_paused(void)
{
    // Paused long enough for the decoder to be left alone, then a seek: the
    // player drops what it held, mpv flushes the decoder (vd_lavc_reset:
    // avcodec_flush_buffers, then its own state) and the demuxer's packets
    // from the new position wake the filter. The new frames come at once.
    open_codec(0, 1000, false);
    run_until(now_us + 3 * 1000000LL);
    checkf(!run_until(now_us + 1000000), "the paused decoder is left alone");

    for (int i = 0; i < player.n; i++)
        free(player.held[i]);
    player.n = 0;
    lavc_in.full = lavc_in.draining = false;
    checkf(ff_mediacodec_dec_flush(&lavc, &mc) == 1, "the seek flushes the codec");
    av_packet_unref(&priv.buffered_pkt);
    vd_priv.state = (struct lavc_state){ .busy_policy = vd_priv.state.busy_policy };
    graph.unread = (struct mp_frame){0};
    graph.woken = true;

    int frames = codec.frames_out;
    run_until(now_us + HOLD_BOUND_US);
    checkf(codec.frames_out > frames, "the first frame after a paused seek comes within one poll");
    close_codec();
}

int main(void)
{
    test_paused_mid_file(0);
    test_paused_mid_file(1);
    test_paused_at_the_end(0);
    test_paused_at_the_end(1);
    test_codec_finishing_its_last_frames();
    test_codec_falling_behind_with_no_buffer_held();
    test_codec_taking_input_without_output();
    test_seek_while_paused();
    printf("PASS: paused with the codec's buffers held, mid-file and at the end, the decoder "
           "is not asked again %d s on until the player gives a buffer back, a run for any "
           "other reason asks once, no call holds the decoding thread more than %d ms, a "
           "codec still working or still taking input is asked until its frames come, and "
           "a paused seek decodes at once\n",
           IDLE_AFTER_US / 1000000, HOLD_BOUND_US / 1000);
    return 0;
}
