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
    puts("PASS: MediaCodec timing cadence, hysteresis, drift and reset boundaries");
    return 0;
}
