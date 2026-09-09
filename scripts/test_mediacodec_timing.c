// Behavioral regressions for the exact freestanding MediaCodec timing core
// extracted from patch 0001 by test_mediacodec_timing.sh. The timestamps model
// several days of monotonic uptime; no Android or mpv scheduler is mocked here.
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
    puts("PASS: MediaCodec timing cadence, hysteresis, drift, reset boundaries "
         "and late release");
    return 0;
}
