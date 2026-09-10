// Production MediaCodec cadence, VO admission and draw/flip regressions.
// The shell harness extracts the authoritative patch's implementations.
// Android/codec/OSD side effects use a deterministic host fixture.
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mediacodec_timing_core.inc"

#define MS INT64_C(1000000)
#define EPOCH INT64_C(604800000000000)

static void check_equal(int64_t actual, int64_t expected, const char *scenario,
                        int line)
{
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s (line %d): got %" PRId64 ", expected %" PRId64
                        "\n", scenario, line, actual, expected);
        exit(1);
    }
}

#define CHECK_EQ(actual, expected, scenario) \
    check_equal((actual), (expected), (scenario), __LINE__)

static void test_raw_deadline_jitter(void)
{
    struct mediacodec_timing timing = {0};
    const int64_t period = 41708333; // 23.976Hz, matching the reported output.
    for (int i = 0; i < 240; i++) {
        int64_t raw = EPOCH + (100 + i) * period + period / 2 +
                      (i % 2 ? MS : -MS);
        int64_t target = mediacodec_timing_predict(&timing, raw, 1000 + i, period);
        CHECK_EQ(mediacodec_timing_snap(&timing, target, EPOCH, period),
                 EPOCH + (100 + i) * period, "half-vsync raw jitter cadence");
    }
    // Stateless nearest-vsync instead chooses 100, 102, 102, 104, ... here.
}

static void test_sample_phase_jitter(void)
{
    struct mediacodec_timing timing = {0};
    const int64_t period = 16666668;
    for (int i = 0; i < 60; i++) {
        int64_t raw = EPOCH + (100 + i) * period + period / 2;
        int64_t sample = EPOCH + (i % 2 ? -MS : MS);
        int64_t target = mediacodec_timing_predict(&timing, raw, i + 1, period);
        CHECK_EQ(mediacodec_timing_snap(&timing, target, sample, period),
                 sample + (100 + i) * period, "independent sampled-phase jitter");
    }
}

static void test_mixed_refresh_cadence(void)
{
    struct mediacodec_timing timing = {0};
    const int64_t period = 11111112; // 90Hz display, 60fps video.
    for (int i = 0; i < 24; i++) {
        // Alternate ambiguous matches with exact matches. The ambiguous phase
        // crosses the midpoint; clear matches must not erase its preference.
        int64_t jitter = i % 2 ? 0 : (i % 4 ? MS / 2 : -MS / 2);
        int64_t target = EPOCH + 100 * period + period / 2 +
                         i * (period * 3 / 2) + jitter;
        CHECK_EQ(mediacodec_timing_snap(&timing, target, EPOCH, period),
                 EPOCH + (100 + (3 * i + 1) / 2) * period,
                 "60fps/90Hz preserves ambiguity across clear matches");
    }
}

static void test_hysteresis_boundaries(void)
{
    struct mediacodec_timing timing = {0};
    const int64_t period = 16 * MS;
    const int64_t base = EPOCH + 100 * period;
    CHECK_EQ(mediacodec_timing_snap(&timing, base + 8 * MS, EPOCH, period),
             base, "fresh midpoint tie chooses earlier");
    // Difference of neighbor distances is exactly T/4: clear the preference.
    CHECK_EQ(mediacodec_timing_snap(&timing, base + period + 10 * MS,
                                   EPOCH, period),
             base + 2 * period, "quarter-period boundary clears hysteresis");
    CHECK_EQ(mediacodec_timing_snap(&timing, base + 2 * period + 9 * MS,
                                   EPOCH, period),
             base + 3 * period, "new ambiguity can prefer later");
    // Difference is exactly T/2: this clear match must retain the preference.
    CHECK_EQ(mediacodec_timing_snap(&timing, base + 3 * period + 4 * MS,
                                   EPOCH, period),
             base + 3 * period, "half-period boundary keeps preference");
    CHECK_EQ(mediacodec_timing_snap(&timing, base + 4 * period + 7 * MS,
                                   EPOCH, period),
             base + 5 * period, "later preference survives a clear match");
    int64_t raw = base + 5 * period + 7 * MS;
    CHECK_EQ(mediacodec_timing_snap(&timing, raw, EPOCH, 0),
             raw, "unknown display period leaves target unsnapped");
    CHECK_EQ(mediacodec_timing_snap(&timing, raw, EPOCH, period),
             base + 5 * period, "unknown display period discards preference");
}

static void test_raw_clock_drift(void)
{
    struct mediacodec_timing timing = {0};
    const int64_t duration = 40 * MS;
    const int64_t anchor = EPOCH + 100 * duration + duration / 2 - MS;
    for (int i = 0; i < 64; i++) {
        int64_t raw = anchor + i * duration + i * MS;
        // Hold cadence through exactly 20ms, then follow the raw clock at
        // 21ms. Repeat long enough to require three genuine re-anchors.
        CHECK_EQ(mediacodec_timing_predict(&timing, raw, i + 1, duration),
                 raw - (i % 21) * MS, "raw drift never escapes 20ms bound");
    }

    mediacodec_timing_reset(&timing);
    int64_t target = mediacodec_timing_predict(&timing, anchor, 1, duration);
    CHECK_EQ(mediacodec_timing_snap(&timing, target, EPOCH, duration),
             EPOCH + 100 * duration, "prime earlier preference before drift");
    int64_t raw = anchor + 2 * duration + 2 * MS;
    CHECK_EQ(mediacodec_timing_predict(&timing, raw, 2, duration),
             raw, "large clock discontinuity reanchors immediately");
    CHECK_EQ(mediacodec_timing_snap(&timing, raw, EPOCH, duration),
             EPOCH + 103 * duration, "clock reanchor discards hysteresis");
}

static void test_skipped_frame_ids(void)
{
    struct mediacodec_timing timing = {0};
    const int64_t duration = 41708333;
    const int64_t anchor = EPOCH + 100 * duration;
    const uint64_t ids[] = {7, 8, 12, 13, 18};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        int64_t ideal = anchor + (int64_t)(ids[i] - ids[0]) * duration;
        int64_t raw = ideal + (i == 0 ? 0 : (i % 2 ? MS : -MS));
        CHECK_EQ(mediacodec_timing_predict(&timing, raw, ids[i], duration),
                 ideal, "dropped frame IDs advance by the full interval");
    }
}

static void test_explicit_reset(void)
{
    struct mediacodec_timing timing = {0};
    const int64_t period = 40 * MS;
    const int64_t base = EPOCH + 100 * period;
    int64_t target = mediacodec_timing_predict(&timing, base + period / 2 - MS,
                                              10, period);
    CHECK_EQ(mediacodec_timing_snap(&timing, target, EPOCH, period),
             base, "prime cadence before seek or untimed reset");
    mediacodec_timing_reset(&timing);
    // A nearby post-reset deadline must be raw even though continuing the old
    // predictor would be within its 20ms allowance. Its snap preference flips.
    int64_t raw = base + period + period / 2 + MS;
    CHECK_EQ(mediacodec_timing_predict(&timing, raw, 11, period),
             raw, "seek or untimed reset discards prediction");
    CHECK_EQ(mediacodec_timing_snap(&timing, raw, EPOCH, period),
             base + 2 * period, "seek or untimed reset discards snap preference");
}

static void test_cadence_boundaries(void)
{
    const int64_t period = 40 * MS;
    const int64_t base = EPOCH + 100 * period;
    const struct {
        double duration;
        uint64_t id;
        bool invalid_duration;
        const char *name;
    } cases[] = {
        {38 * MS, 11, false, "duration change over 1ms reanchors"},
        {-1, 11, true, "unknown duration cannot predict"},
        {0, 11, true, "zero duration cannot predict"},
        {NAN, 11, true, "NaN duration cannot predict"},
        {INFINITY, 11, true, "infinite duration cannot predict"},
        {40 * MS, 10, false, "repeated frame ID cannot advance cadence"},
        {40 * MS, 9, false, "backwards frame ID cannot advance cadence"},
        {40 * MS, 0, false, "unknown frame ID cannot predict"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct mediacodec_timing timing = {0};
        int64_t target = mediacodec_timing_predict(
            &timing, base + period / 2 - MS, 10, period);
        CHECK_EQ(mediacodec_timing_snap(&timing, target, EPOCH, period),
                 base, "prime preference before invalid cadence");
        int64_t raw = base + period + period / 2 + MS;
        CHECK_EQ(mediacodec_timing_predict(&timing, raw, cases[i].id,
                                          cases[i].duration),
                 raw, cases[i].name);
        CHECK_EQ(mediacodec_timing_snap(&timing, raw, EPOCH, period),
                 base + 2 * period, cases[i].name);
        if (cases[i].invalid_duration || cases[i].id == 0) {
            raw += period + 3 * MS;
            CHECK_EQ(mediacodec_timing_predict(&timing, raw, cases[i].id + 1,
                                              period),
                     raw, "valid cadence cannot inherit an invalid anchor");
        }
    }

    struct mediacodec_timing timing = {0};
    CHECK_EQ(mediacodec_timing_predict(&timing, base, 1, period), base,
             "first valid frame uses raw deadline");
    CHECK_EQ(mediacodec_timing_predict(&timing, base + period + 3 * MS, 2,
                                      period + MS),
             base + period + MS, "duration change at 1ms retains cadence");
}

#define MAX_AHEAD (500 * MS)

// The vsync a SurfaceView shows a buffer on: the first grid line at or after
// its timestamp (an untimed release has none) that is still after now.
static int64_t presented_vsync(int64_t release, int64_t now, int64_t sample,
                               int64_t period)
{
    int64_t earliest = release > now ? release : now + 1;
    int64_t count = (earliest - sample + period - 1) / period;
    return sample + count * period;
}

// 23.976 fps content on a fixed 2-period queue: a frame is presented on the
// vsync cadence chose no matter how late the VO thread runs, as long as that
// vsync is still ahead, and on the next vsync once it is not. A release that
// has already passed is still that vsync minus 80% of a period, never the
// unsnapped target, which would round a still-reachable vsync up to the next.
static void test_release_survives_late_submission(void)
{
    const int64_t duration = 41708333;
    const struct { int64_t period; int gap_min, gap_max; const char *name; }
    displays[] = {
        {16666667, 2, 3, "23.976 on 60 Hz"},
        {20833333, 2, 2, "23.976 on 48 Hz"},
    };
    enum { frames = 48 };
    for (size_t d = 0; d < sizeof(displays) / sizeof(displays[0]); d++) {
        const int64_t period = displays[d].period;
        int64_t reference[frames];
        for (int64_t late = 0; late < 2 * period; late += MS) {
            struct mediacodec_timing timing = {0};
            for (int i = 0; i < frames; i++) {
                int64_t raw = EPOCH + 100 * period + i * duration + 5 * MS +
                              (i % 2 ? MS : -MS);
                int64_t now = raw - 2 * period + late;
                int64_t release = mediacodec_timing_release(
                    &timing, raw, now, 1000 + i, duration, EPOCH, period,
                    MAX_AHEAD);
                int64_t shown = presented_vsync(release, now, EPOCH, period);
                if (late == 0) {
                    reference[i] = shown;
                    if (i) {
                        int64_t gap = (shown - reference[i - 1]) / period;
                        if (gap < displays[d].gap_min || gap > displays[d].gap_max) {
                            fprintf(stderr, "FAIL: %s cadence gap %" PRId64
                                    " at frame %d\n", displays[d].name, gap, i);
                            exit(1);
                        }
                    }
                } else {
                    int64_t expected = reference[i] > now
                        ? reference[i] : presented_vsync(0, now, EPOCH, period);
                    if (shown != expected) {
                        fprintf(stderr, "FAIL: %s: frame %d submitted %" PRId64
                                "ms late presents %" PRId64 " periods after the "
                                "reachable vsync\n", displays[d].name, i,
                                late / MS, (shown - expected) / period);
                        exit(1);
                    }
                }
            }
        }
    }
}

// A frame whose raw deadline already passed releases untimed, but the next
// frame still predicts from cadence and keeps its snap preference; the same
// holds when only the cadence-corrected target is behind the clock.
static void test_late_frames_keep_cadence(void)
{
    const int64_t period = 40 * MS;
    const int64_t base = EPOCH + 100 * period;
    const int64_t on_time = 2 * period;
    const struct { int64_t now_offset; const char *name; } cases[] = {
        {MS, "raw deadline behind the clock"},
        {0, "raw deadline on the clock"},
        {-MS, "predicted target behind the clock, raw ahead"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct mediacodec_timing timing = {0};
        int64_t raw = base + period / 2 - MS;
        CHECK_EQ(mediacodec_timing_release(&timing, raw, raw - on_time, 10,
                                           period, EPOCH, period, MAX_AHEAD),
                 base - period * 8 / 10, "prime cadence and snap preference");

        raw = base + period + period / 2 + MS;
        int64_t now = raw + cases[i].now_offset;
        int64_t release = mediacodec_timing_release(
            &timing, raw, now, 11, period, EPOCH, period, MAX_AHEAD);
        CHECK_EQ(release, cases[i].now_offset >= 0 ? 0 : base + period / 5,
                 cases[i].name);
        CHECK_EQ(presented_vsync(release, now, EPOCH, period), base + 2 * period,
                 cases[i].name);

        // Raw is 1ms past the midpoint: a reset would snap it to base + 3p.
        raw = base + 2 * period + period / 2 + MS;
        CHECK_EQ(mediacodec_timing_release(&timing, raw, raw - on_time, 12,
                                           period, EPOCH, period, MAX_AHEAD),
                 base + 2 * period - period * 8 / 10, cases[i].name);
    }
}

static void test_release_discontinuities(void)
{
    const int64_t period = 40 * MS;
    const int64_t base = EPOCH + 100 * period;
    struct mediacodec_timing timing = {0};
    int64_t raw = base + period / 2 - MS;
    CHECK_EQ(mediacodec_timing_release(&timing, raw, raw - 2 * period, 10,
                                       period, EPOCH, period, MAX_AHEAD),
             base - period * 8 / 10, "prime before discontinuities");

    raw = base + period + period / 2 + MS;
    CHECK_EQ(mediacodec_timing_release(&timing, raw, raw - MAX_AHEAD - MS, 11,
                                       period, EPOCH, period, MAX_AHEAD),
             0, "deadline beyond the surface window releases untimed");
    CHECK_EQ(mediacodec_timing_snap(&timing, raw, EPOCH, period),
             base + 2 * period, "deadline beyond the surface window resets");

    timing = (struct mediacodec_timing){0};
    raw = base + period / 2 - MS;
    mediacodec_timing_release(&timing, raw, raw - 2 * period, 10, period,
                              EPOCH, period, MAX_AHEAD);
    raw = base + period + period / 2 + MS;
    CHECK_EQ(mediacodec_timing_release(&timing, raw, 0, 11, period, EPOCH,
                                       period, MAX_AHEAD),
             0, "unknown clock releases untimed");
    CHECK_EQ(mediacodec_timing_snap(&timing, raw, EPOCH, period),
             base + 2 * period, "unknown clock resets");

    timing = (struct mediacodec_timing){0};
    raw = base + period / 2 - MS;
    CHECK_EQ(mediacodec_timing_release(&timing, raw, raw - 2 * period, 10,
                                       period, EPOCH, 0, MAX_AHEAD),
             raw, "unknown display period releases at the target");
    raw = base + period + period / 2 + MS;
    CHECK_EQ(mediacodec_timing_release(&timing, raw, raw - 2 * period, 11,
                                       period, 0, period, MAX_AHEAD),
             raw - 2 * MS, "stale vsync sample releases at the cadence target");
}

// Platform fixture for the extracted production admission/draw/flip path.
#define MP_TIME_MS_TO_NS(v) ((v) * MS)
#define MP_TIME_S_TO_NS(v) ((v) * INT64_C(1000000000))
#define MP_NOPTS_VALUE (-1e20)
#define VO_TRUE true
#define OSD_RELEASE_HISTORY 8
#define OSD_STATS_INTERVAL_FRAMES 120
#define MP_VERBOSE(...) ((void)0)
#define MP_WARN(...) ((void)0)

#include "mediacodec_timing_cadence.inc"

typedef struct {
    int releases;
    int64_t timestamp;
} AVMediaCodecBuffer;

struct mp_image {
    double pts;
    void *planes[4];
    int refs;
};

struct vo_frame {
    bool redraw, repeat, still, display_synced;
    int64_t pts;
    double duration;
    uint64_t frame_id;
    struct mp_image *current;
};

struct osd_request {
    double pts, delta;
    uint64_t seq, epoch;
};

struct osd_release {
    uint64_t seq;
    int64_t timestamp, vsync, period;
};

struct osd_shared {
    uint64_t epoch, release_seq;
    unsigned frames;
    struct osd_release releases[OSD_RELEASE_HISTORY];
    int release_next, results;
};

struct priv {
    struct mp_image *cur_image;
    int64_t cur_pts;
    uint64_t cur_frame_id, cur_seq, submitted_frame_id;
    uint64_t prepared_frame_id, prepared_seq, prepared_epoch;
    double cur_duration, osd_pts;
    bool cur_synthetic, osd_threads_created, osd_missing_logged;
    bool vsync_thread_created;
    int vsync_lock, osd_lock, osd_wakeup;
    int64_t vsync_sample, queue_period, queue_period_changed, queue_offset;
    struct mediacodec_timing timing;
    struct osd_cadence cadence;
    struct osd_shared osd;
    uint64_t frame_seq;
    unsigned stats_frames;
};

struct vo;
struct vo_driver {
    int64_t (*prepare_frame)(struct vo *, struct vo_frame *);
};

struct vo_internal {
    struct vo_frame *frame_queued;
    bool paused, send_reset;
    int lock;
    int64_t wakeup_pts;
};

struct vo {
    struct priv *priv;
    struct vo_internal *in;
    const struct vo_driver *driver;
    double period;
    int64_t admission_offset;
};

static int64_t clock_ns;
// CLOCK_MONOTONIC minus mp_time. Zero for every test that does not care;
// a non-zero, moving value is the only way to see that admission and the
// presentation timestamp are computed in different domains.
static int64_t clock_offset_ns;
static struct osd_request requested;
static struct osd_release published;
static unsigned requests;

static int64_t monotonic_now_ns(void) { return clock_ns + clock_offset_ns; }
static int64_t mp_time_ns(void) { return clock_ns; }
static int64_t mp_time_to_monotonic_ns(int64_t pts) { return pts + clock_offset_ns; }
static int64_t monotonic_to_mp_time_ns(int64_t mono) { return mono - clock_offset_ns; }
static void mp_mutex_lock(int *lock) { (*lock)++; }
static void mp_mutex_unlock(int *lock) { (*lock)--; }
static void mp_cond_broadcast(int *cond) { (void)cond; }
static double vo_get_vsync_interval(struct vo *vo) { return vo->period; }
static void vo_set_queue_params(struct vo *vo, int64_t offset, int count)
{
    (void)count;
    vo->admission_offset = offset;
}

static struct mp_image *mp_image_new_ref(struct mp_image *image)
{
    image->refs++;
    return image;
}

static void mp_image_unrefp(struct mp_image **image)
{
    if (*image)
        (*image)->refs--;
    *image = NULL;
}

static struct vo_frame *vo_frame_ref(struct vo_frame *frame)
{
    struct vo_frame *copy = malloc(sizeof(*copy));
    if (!copy)
        abort();
    *copy = *frame;
    if (copy->current)
        mp_image_new_ref(copy->current);
    return copy;
}

static void talloc_free(struct vo_frame *frame)
{
    mp_image_unrefp(&frame->current);
    free(frame);
}

static int av_mediacodec_render_buffer_at_time(AVMediaCodecBuffer *buffer,
                                               int64_t timestamp)
{
    buffer->releases++;
    buffer->timestamp = timestamp;
    return 0;
}

static int av_mediacodec_release_buffer(AVMediaCodecBuffer *buffer, int render)
{
    CHECK_EQ(render, 1, "buffer is rendered, not discarded");
    return av_mediacodec_render_buffer_at_time(buffer, 0);
}

// Armed by the seek regression: the core discards the queued frame while
// preparation runs with the VO lock released.
static struct vo_internal *seek_victim;

static void osd_file_request(struct priv *p, struct osd_request request)
{
    (void)p;
    requested = request;
    requests++;
    if (seek_victim) {
        seek_victim->frame_queued = NULL;
        seek_victim = NULL;
    }
}

static void osd_result_publish_release(int *results, struct osd_release release)
{
    (void)results;
    published = release;
}

static void osd_invalidate_locked(struct priv *p) { p->osd.epoch++; }
static void osd_log_stats(struct vo *vo, const char *why)
{
    (void)vo;
    (void)why;
}

#include "mediacodec_timing_driver.inc"
#include "mediacodec_timing_admission.inc"

static const struct vo_driver driver = {.prepare_frame = prepare_frame};

// Run the actual core admission decision and production draw/flip. A false
// result returns to the VO event loop without releasing a codec buffer.
static bool present_queued(struct vo *vo)
{
    mp_mutex_lock(&vo->in->lock);
    bool ready = prepare_queued_frame(vo);
    mp_mutex_unlock(&vo->in->lock);
    CHECK_EQ(vo->in->lock, 0, "admission returns with balanced locking");
    if (!ready)
        return false;
    struct vo_frame *frame = vo->in->frame_queued;
    vo->in->frame_queued = NULL;
    draw_frame(vo, frame);
    flip_page(vo);
    return true;
}

static void test_preparation_does_not_submit_early(void)
{
    struct priv p = {.osd_threads_created = true, .vsync_thread_created = true};
    struct vo_internal in = {0};
    struct vo vo = {&p, &in, &driver, 40 * MS, 0};
    AVMediaCodecBuffer buffer = {0};
    struct mp_image image = {.pts = 12.5, .planes[3] = &buffer, .refs = 1};
    struct vo_frame frame = {.current = &image, .frame_id = 1,
                              .pts = EPOCH + 1000 * MS, .duration = 40 * MS};
    in.frame_queued = &frame;
    clock_ns = frame.pts - 120 * MS;
    // A Choreographer grid whose lines fall on the frame's own deadline.
    p.vsync_sample = frame.pts - 2 * 40 * MS;
    CHECK_EQ(present_queued(&vo), false, "120ms preparation cannot submit");
    CHECK_EQ(buffer.releases, 0, "no immediate release at the old budget");
    CHECK_EQ(vo.admission_offset, 140 * MS,
             "OSD horizon still reaches past the codec window");
    CHECK_EQ(requested.pts * 1000, 12500, "OSD is prepared before admission");
    // Two display periods before the timestamp the codec is actually given
    // (the chosen vsync minus 80% of a period), not before the raw deadline.
    CHECK_EQ(in.wakeup_pts, frame.pts - 32 * MS - 80 * MS,
             "admission counts back from the presentation timestamp");
    unsigned prepared = requests;
    in.paused = true;
    CHECK_EQ(present_queued(&vo), false, "pause wakeup cannot release too early");
    CHECK_EQ(buffer.releases, 0, "paused queued video still respects codec lead");
    in.paused = false;
    clock_ns = in.wakeup_pts - 1;
    CHECK_EQ(present_queued(&vo), false, "one ns before admission stays queued");
    CHECK_EQ(requests, prepared, "unrelated wakeups do not restart OSD work");
    clock_ns++;
    CHECK_EQ(present_queued(&vo), true, "admit at the codec boundary");
    CHECK_EQ(buffer.releases, 1, "submit once");
    CHECK_EQ(buffer.timestamp, frame.pts - 32 * MS, "preserve minus 0.8P snap");
    CHECK_EQ(monotonic_to_mp_time_ns(buffer.timestamp) - clock_ns, 80 * MS,
             "the codec gets two display periods of notice");
    CHECK_EQ(published.seq, requested.seq, "OSD gets its matching video release");
    CHECK_EQ(image.refs, 1, "balanced image ownership");
}

static void test_driver_refresh_and_cadence(void)
{
    const struct { double fps, hz; } rates[] = {
        {23.976, 23.976}, {24, 24}, {25, 25}, {50, 50}, {60, 60},
        {90, 90}, {120, 120}, {24, 60}, {60, 90}, {25, 120}, {48, 60},
    };
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        int64_t period = llround(1e9 / rates[r].hz);
        int64_t duration = llround(1e9 / rates[r].fps);
        struct priv p = {.vsync_thread_created = true};
        struct vo_internal in = {0};
        struct vo vo = {&p, &in, &driver, period, 0};
        struct mediacodec_timing reference = {0};
        int64_t previous_vsync = 0;
        for (int i = 0; i < 30; i++) {
            AVMediaCodecBuffer buffer = {0};
            struct mp_image image = {.pts = i / rates[r].fps,
                                     .planes[3] = &buffer, .refs = 1};
            struct vo_frame frame = {.current = &image, .frame_id = 100 + i,
                .pts = EPOCH + 1000 * period + i * duration, .duration = duration};
            in.frame_queued = &frame;
            if (!i)
                clock_ns = frame.pts - 500 * MS;
            CHECK_EQ(present_queued(&vo), false, "early matched/mixed frame waits");
            clock_ns = in.wakeup_pts;
            p.vsync_sample = EPOCH + ((clock_ns - EPOCH) / period) * period;
            CHECK_EQ(present_queued(&vo), true, "matched/mixed frame admitted");
            CHECK_EQ(buffer.releases, 1, "matched/mixed submits once");
            if (!buffer.timestamp) {
                fprintf(stderr, "FAIL: %.3f fps / %.3f Hz rendered untimed\n",
                        rates[r].fps, rates[r].hz);
                exit(1);
            }
            int64_t expected = mediacodec_timing_release(&reference, frame.pts,
                clock_ns, frame.frame_id, duration, p.vsync_sample, period,
                mediacodec_release_horizon(period));
            CHECK_EQ(buffer.timestamp, expected, "admission preserves cadence/snap");
            // The whole point of admitting against the corrected timestamp:
            // MediaCodec must still get its two display periods of notice.
            // The first frame after a period change has no grid yet, so its
            // deadline is set from the raw pts and the 80% pull-back eats
            // into the window until a Choreographer sample is trusted.
            int64_t window = monotonic_to_mp_time_ns(buffer.timestamp) - clock_ns;
            if (i && window < 2 * period) {
                fprintf(stderr, "FAIL: %.3f fps / %.3f Hz submitted %" PRId64
                        "us before its timestamp, needs %" PRId64 "us\n",
                        rates[r].fps, rates[r].hz, window / 1000,
                        2 * period / 1000);
                exit(1);
            }
            int64_t shown = presented_vsync(buffer.timestamp, clock_ns, EPOCH, period);
            if (i && rates[r].fps <= rates[r].hz) {
                int64_t gap = (shown - previous_vsync) / period;
                if (gap < (int64_t)floor(rates[r].hz / rates[r].fps) ||
                    gap > (int64_t)ceil(rates[r].hz / rates[r].fps))
                    abort();
            }
            previous_vsync = shown;
        }
    }
}

static void test_refresh_transition_and_late_frame(void)
{
    struct priv p = {.vsync_thread_created = true, .osd_threads_created = true};
    struct vo_internal in = {0};
    struct vo vo = {&p, &in, &driver, 40 * MS, 0};
    AVMediaCodecBuffer buffer = {0};
    struct mp_image image = {.pts = 1, .planes[3] = &buffer, .refs = 1};
    struct vo_frame frame = {.current = &image, .frame_id = 1,
        .pts = EPOCH + 1000 * MS, .duration = 40 * MS};
    in.frame_queued = &frame;
    clock_ns = frame.pts - 120 * MS;
    CHECK_EQ(present_queued(&vo), false, "prepare before refresh transition");
    int64_t old_flip = in.wakeup_pts;
    unsigned old_requests = requests;
    vo.period = 1e9 / 120;
    clock_ns = old_flip;
    CHECK_EQ(present_queued(&vo), false, "old 25Hz deadline cannot submit at 120Hz");
    CHECK_EQ(buffer.releases, 0, "refresh tightening never releases untimed");
    CHECK_EQ(requests, old_requests, "refresh keeps prepared OSD");
    CHECK_EQ(in.wakeup_pts, frame.pts - 2 * llround(vo.period),
             "refresh transition rearms the interruptible timer");
    // An OSD invalidation while the video waits must reprepare its pose.
    p.osd.epoch++;
    CHECK_EQ(present_queued(&vo), false, "OSD invalidation does not submit video");
    CHECK_EQ(requests, old_requests + 1, "invalidated preparation is replaced");
    clock_ns = in.wakeup_pts;
    p.vsync_sample = clock_ns;
    CHECK_EQ(present_queued(&vo), true, "transition submits at new lead");
    CHECK_EQ(buffer.timestamp != 0, true, "transition remains timed");

    buffer = (AVMediaCodecBuffer){0};
    frame.frame_id++;
    frame.pts += 40 * MS;
    image.pts += 0.04;
    in.frame_queued = &frame;
    clock_ns = frame.pts + MS;
    CHECK_EQ(present_queued(&vo), true, "late execution submits without waiting");
    CHECK_EQ(buffer.timestamp, 0, "late deadline renders untimed");
    CHECK_EQ(p.timing.frame_id, frame.frame_id, "late frame advances cadence");

    buffer = (AVMediaCodecBuffer){0};
    frame.frame_id++;
    frame.pts += 40 * MS;
    in.frame_queued = &frame;
    vo.period = NAN;
    clock_ns = frame.pts - 100 * MS;
    CHECK_EQ(present_queued(&vo), false, "unknown period still waits");
    CHECK_EQ(in.wakeup_pts, frame.pts - 50 * MS, "unknown period 50ms fallback");
    clock_ns = in.wakeup_pts;
    CHECK_EQ(present_queued(&vo), true, "unknown period admits at fallback");
    CHECK_EQ(buffer.timestamp, frame.pts, "unknown period keeps raw timestamp");
}

static void test_drop_pause_redraw_resume(void)
{
    const int64_t period = 40 * MS, base = EPOCH + 1000 * MS;
    struct priv p = {.vsync_thread_created = true, .osd_threads_created = true};
    struct vo_internal in = {0};
    struct vo vo = {&p, &in, &driver, period, 0};
    AVMediaCodecBuffer a = {0}, b = {0}, c = {0};
    struct mp_image images[] = {
        {.pts = 20, .planes[3] = &a, .refs = 1},
        {.pts = 20.04, .planes[3] = &b, .refs = 1},
        {.pts = 20.08, .planes[3] = &c, .refs = 1},
    };
    struct vo_frame frame = {.current = &images[0], .frame_id = 10,
        .pts = base + period / 2 - MS, .duration = period};
    for (uint64_t i = 1; i < 10; i++)
        osd_cadence_observe(&p.cadence, i, 20 - (10 - i) * 0.04);
    in.frame_queued = &frame;
    clock_ns = frame.pts - 120 * MS;
    CHECK_EQ(present_queued(&vo), false, "prepare A");
    clock_ns = in.wakeup_pts;
    p.vsync_sample = base - 2 * period;
    CHECK_EQ(present_queued(&vo), true, "present A");

    frame = (struct vo_frame){.current = &images[1], .frame_id = 11,
        .pts = base + period + period / 2 + MS, .duration = period};
    in.frame_queued = &frame;
    // At 25 Hz the codec window is wider than one frame, so B's window is
    // already open when A is submitted. Preparation on its own must still
    // never touch the codec: the core can drop this frame before draw/flip.
    CHECK_EQ(prepare_frame(&vo, &frame) > 0, 1, "prepare B without submitting it");
    // Exercise the production cancellation notification used by the core drop
    // branch. There must be no codec release until the later full redraw.
    in.frame_queued = NULL;
    prepare_frame(&vo, NULL);
    CHECK_EQ(b.releases, 0, "dropping preparation does not render the buffer");
    double media_delta = osd_cadence_delta(&p.cadence);
    int64_t preparation_lead = p.queue_offset;

    // Core dropped B without draw/flip, then do_redraw gives its exact sentinel
    // shape: full redraw has redraw=false, repeat=false and still=true.
    frame = (struct vo_frame){.current = &images[1], .frame_id = 11,
                              .still = true, .pts = 0, .duration = -1};
    draw_frame(&vo, &frame);
    flip_page(&vo);
    CHECK_EQ(b.releases, 1, "previously dropped still buffer becomes visible");
    CHECK_EQ(b.timestamp, 0, "synthetic still presents immediately");
    CHECK_EQ(requested.pts * 1000, 20040, "still subtitle pose uses image pts");
    CHECK_EQ(published.seq, requested.seq, "still pose matches submitted buffer");
    CHECK_EQ(llround(osd_cadence_delta(&p.cadence) * 1e9),
             llround(media_delta * 1e9),
             "synthetic still preserves OSD pre-render cadence");
    CHECK_EQ(p.queue_offset, preparation_lead, "synthetic duration cannot shrink queue");
    draw_frame(&vo, &frame);
    flip_page(&vo);
    CHECK_EQ(b.releases, 1, "repeated full redraw does not resubmit still buffer");
    frame.redraw = true;
    draw_frame(&vo, &frame);
    flip_page(&vo);
    CHECK_EQ(b.releases, 1, "ordinary OSD redraw does not resubmit codec buffer");

    frame = (struct vo_frame){.current = &images[2], .frame_id = 12,
        .pts = base + 2 * period + period / 2 + MS, .duration = period};
    in.frame_queued = &frame;
    clock_ns = frame.pts - 160 * MS;
    CHECK_EQ(present_queued(&vo), false, "resume prepares C");
    clock_ns = in.wakeup_pts;
    p.vsync_sample = base;
    CHECK_EQ(present_queued(&vo), true, "resume presents C");
    CHECK_EQ(c.timestamp, base + 2 * period - period * 8 / 10,
             "resume keeps pre-drop cadence and midpoint preference");
    for (size_t i = 0; i < 3; i++)
        CHECK_EQ(images[i].refs, 1, "redraw/resume balances image ownership");
    reset_video(&vo);
    CHECK_EQ(p.timing.frame_id, 0, "real seek/reconfig still resets timing");
    CHECK_EQ(p.cadence.have_last, false, "real seek/reconfig resets OSD cadence");
}

// Admission is decided in mp_time, the presentation timestamp in
// CLOCK_MONOTONIC. mpv reads CLOCK_MONOTONIC_RAW where it can, so the two
// are neither equal nor rate-locked; the codec window has to survive the
// conversion with the offset moving in either direction.
static void test_clock_domain_drift(void)
{
    const int64_t period = 16666667, duration = 41708333;
    const int64_t drifts[] = {37 * MS, -37 * MS};
    for (size_t d = 0; d < sizeof(drifts) / sizeof(drifts[0]); d++) {
        struct priv p = {.vsync_thread_created = true};
        struct vo_internal in = {0};
        struct vo vo = {&p, &in, &driver, period, 0};
        for (int i = 0; i < 24; i++) {
            AVMediaCodecBuffer buffer = {0};
            struct mp_image image = {.pts = i * 0.0417,
                                     .planes[3] = &buffer, .refs = 1};
            struct vo_frame frame = {.current = &image, .frame_id = 500 + i,
                .pts = EPOCH + 1000 * period + i * duration, .duration = duration};
            in.frame_queued = &frame;
            clock_offset_ns = drifts[d] + (d ? -i : i) * INT64_C(1000);
            if (!i)
                clock_ns = frame.pts - 500 * MS;
            CHECK_EQ(present_queued(&vo), false, "drifting clocks still wait");
            clock_ns = in.wakeup_pts;
            int64_t mono = monotonic_now_ns();
            p.vsync_sample = EPOCH + ((mono - EPOCH) / period) * period;
            CHECK_EQ(present_queued(&vo), true, "drifting clocks admit");
            CHECK_EQ(buffer.releases, 1, "one submission per frame");
            int64_t window = monotonic_to_mp_time_ns(buffer.timestamp) - clock_ns;
            if (!buffer.timestamp || (i && window < 2 * period)) {
                fprintf(stderr, "FAIL: drift %+" PRId64 "ms frame %d submitted %"
                        PRId64 "us before its timestamp\n",
                        drifts[d] / MS, i, window / 1000);
                exit(1);
            }
        }
    }
    clock_offset_ns = 0;
}

// Playback speed reaches the VO only as vo_frame.duration. A change past the
// cadence tolerance re-anchors the prediction mid-stream; admission must
// re-evaluate with it and keep the codec window on both sides.
static void test_speed_change_admission(void)
{
    const int64_t period = 16666667;
    const int64_t durations[] = {41708333, 41708333, 41708333, 41708333,
                                 20854166, 20854166, 20854166, 20854166};
    struct priv p = {.vsync_thread_created = true};
    struct vo_internal in = {0};
    struct vo vo = {&p, &in, &driver, period, 0};
    int64_t pts = EPOCH + 1000 * period;
    clock_ns = pts - 500 * MS;
    for (size_t i = 0; i < sizeof(durations) / sizeof(durations[0]); i++) {
        AVMediaCodecBuffer buffer = {0};
        struct mp_image image = {.pts = 30 + 0.04 * (double)i,
                                 .planes[3] = &buffer, .refs = 1};
        struct vo_frame frame = {.current = &image, .frame_id = 700 + i,
            .pts = pts, .duration = (double)durations[i]};
        in.frame_queued = &frame;
        CHECK_EQ(present_queued(&vo), false, "speed change still waits");
        clock_ns = in.wakeup_pts;
        p.vsync_sample = EPOCH + ((clock_ns - EPOCH) / period) * period;
        CHECK_EQ(present_queued(&vo), true, "speed change admits");
        int64_t window = monotonic_to_mp_time_ns(buffer.timestamp) - clock_ns;
        if (!buffer.timestamp || (i && window < 2 * period)) {
            fprintf(stderr, "FAIL: %" PRId64 "us frame submitted %" PRId64
                    "us before its timestamp\n", durations[i] / 1000,
                    window / 1000);
            exit(1);
        }
        pts += durations[i];
    }
}

// Preparation runs with the VO lock released, so a seek can discard the
// queued frame inside that window. The core must not arm a wakeup for a
// frame that no longer exists, and nothing may reach the codec.
static void test_seek_during_preparation(void)
{
    struct priv p = {.osd_threads_created = true, .vsync_thread_created = true};
    struct vo_internal in = {0};
    struct vo vo = {&p, &in, &driver, 40 * MS, 0};
    AVMediaCodecBuffer buffer = {0};
    struct mp_image image = {.pts = 5, .planes[3] = &buffer, .refs = 1};
    struct vo_frame frame = {.current = &image, .frame_id = 1,
                             .pts = EPOCH + 1000 * MS, .duration = 40 * MS};
    in.frame_queued = &frame;
    clock_ns = frame.pts - 300 * MS;
    seek_victim = &in;
    mp_mutex_lock(&in.lock);
    CHECK_EQ(prepare_queued_frame(&vo), 1, "a discarded frame cannot hold the core");
    mp_mutex_unlock(&in.lock);
    CHECK_EQ(in.wakeup_pts, 0, "no wakeup is armed for a frame that was dropped");
    CHECK_EQ(buffer.releases, 0, "nothing is submitted for a discarded frame");
    CHECK_EQ(in.lock, 0, "balanced locking across the discard");
    CHECK_EQ(image.refs, 1, "balanced image ownership across the discard");
}

int main(void)
{
    test_raw_deadline_jitter();
    test_sample_phase_jitter();
    test_mixed_refresh_cadence();
    test_hysteresis_boundaries();
    test_raw_clock_drift();
    test_skipped_frame_ids();
    test_explicit_reset();
    test_cadence_boundaries();
    test_release_survives_late_submission();
    test_late_frames_keep_cadence();
    test_release_discontinuities();
    test_preparation_does_not_submit_early();
    test_driver_refresh_and_cadence();
    test_refresh_transition_and_late_frame();
    test_drop_pause_redraw_resume();
    test_clock_domain_drift();
    test_speed_change_admission();
    test_seek_during_preparation();
    puts("PASS: MediaCodec cadence, production admission/draw/flip, the codec "
         "window across clock drift and speed changes, refresh transitions, "
         "and dropped still redraw/resume");
    return 0;
}
