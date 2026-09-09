// Behavioral regressions for the exact freestanding OSD scheduler core
// extracted from patch 0001 by test_mediacodec_osd.sh: the cadence estimator
// that predicts the next frame's pts, the single-use pre-render, the read
// horizon cap, the latency shift, the swap lead, and dirty-region folding.
// No libass, EGL or mpv thread is modelled here.
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mediacodec_osd_core.inc"

#define FRAME 0.041708333 // 23.976 fps

static void fail(const char *scenario, int line)
{
    fprintf(stderr, "FAIL: %s (line %d)\n", scenario, line);
    exit(1);
}

#define CHECK(cond, scenario) do { if (!(cond)) fail((scenario), __LINE__); } while (0)
#define CHECK_NEAR(a, b, scenario) CHECK(fabs((a) - (b)) < 1e-9, scenario)

static void feed_cadence(struct osd_cadence *c, uint64_t first_id, int frames,
                         double start, double delta)
{
    for (int n = 0; n < frames; n++)
        osd_cadence_observe(c, first_id + n, start + n * delta);
}

static void test_cadence_needs_four_consecutive_deltas(void)
{
    struct osd_cadence c;
    osd_cadence_reset(&c);
    feed_cadence(&c, 1, 4, 10.0, FRAME); // 3 deltas
    CHECK(osd_cadence_delta(&c) == 0, "three deltas are not a cadence");
    osd_cadence_observe(&c, 5, 10.0 + 4 * FRAME);
    CHECK_NEAR(osd_cadence_delta(&c), FRAME, "fourth delta enables prediction");
}

static void test_cadence_median_ignores_matroska_rounding(void)
{
    // Matroska keeps ms timestamps: 42, 41, 42, 42, 41, ... at 23.976.
    struct osd_cadence c;
    osd_cadence_reset(&c);
    double pts = 10.0;
    static const double steps[] = {0.042, 0.041, 0.042, 0.042, 0.041, 0.042,
                                   0.042, 0.041, 0.042};
    osd_cadence_observe(&c, 1, pts);
    for (int n = 0; n < 9; n++) {
        pts += steps[n];
        osd_cadence_observe(&c, 2 + n, pts);
    }
    CHECK_NEAR(osd_cadence_delta(&c), 0.042, "median of the rounded deltas");
}

static void test_cadence_resets_on_gap_backwards_and_long_delta(void)
{
    struct osd_cadence c;
    osd_cadence_reset(&c);
    feed_cadence(&c, 1, 8, 10.0, FRAME);
    CHECK(osd_cadence_delta(&c) > 0, "cadence established");

    osd_cadence_observe(&c, 10, 10.0 + 9 * FRAME); // id 9 was dropped
    CHECK(osd_cadence_delta(&c) == 0, "a frame_id gap starts over");

    feed_cadence(&c, 11, 5, 20.0, FRAME);
    CHECK(osd_cadence_delta(&c) > 0, "cadence re-established");
    osd_cadence_observe(&c, 16, 5.0); // seek backwards
    CHECK(osd_cadence_delta(&c) == 0, "a backwards pts starts over");

    feed_cadence(&c, 17, 5, 30.0, FRAME);
    osd_cadence_observe(&c, 22, 30.0 + 4 * FRAME + 0.5); // half a second gap
    CHECK(osd_cadence_delta(&c) == 0, "a gap longer than a cadence starts over");
}

static void test_latency_shift_needs_cadence(void)
{
    struct osd_cadence c;
    osd_cadence_reset(&c);
    CHECK_NEAR(osd_cadence_shift(&c, 10.0, 1), 10.0, "no cadence, no shift");
    feed_cadence(&c, 1, 5, 10.0, FRAME);
    CHECK_NEAR(osd_cadence_shift(&c, 10.0, 1), 10.0 - FRAME, "one frame earlier");
    CHECK_NEAR(osd_cadence_shift(&c, 10.0, -2), 10.0 + 2 * FRAME, "two frames later");
    CHECK_NEAR(osd_cadence_shift(&c, 10.0, 0), 10.0, "zero leaves pts alone");
}

static void test_spec_hit_is_single_use_and_epoch_bound(void)
{
    struct osd_spec s = {0};
    double pts;
    CHECK(osd_spec_plan(&s, 10.0, FRAME, INFINITY, true, &pts), "plan next frame");
    CHECK_NEAR(pts, 10.0 + FRAME, "next frame pts");
    osd_spec_store(&s, pts, 7, 2);

    CHECK(osd_spec_service(&s, pts + 0.001, 7, false, FRAME) == OSD_SPEC_HIT,
          "1 ms of container rounding still hits");
    CHECK(s.hits == 1 && s.misses == 0, "hit counted");
    CHECK(osd_spec_service(&s, pts, 7, false, FRAME) == OSD_SPEC_MISS,
          "a pre-render serves one request only");
    CHECK(s.misses == 0, "an absent pre-render is not a miss");

    osd_spec_store(&s, pts, 7, 2);
    CHECK(osd_spec_service(&s, pts, 8, false, FRAME) == OSD_SPEC_MISS,
          "a seek (epoch) invalidates the pre-render");
    CHECK(s.misses == 1, "epoch miss counted");

    osd_spec_store(&s, pts, 8, 2);
    CHECK(osd_spec_service(&s, pts, 8, true, FRAME) == OSD_SPEC_MISS,
          "a full repaint cannot be served from a partial pre-render");

    osd_spec_store(&s, pts, 8, 2);
    CHECK(osd_spec_service(&s, pts + FRAME / 2, 8, false, FRAME) == OSD_SPEC_MISS,
          "half a frame off is the wrong frame");

    // At 240 fps the tolerance shrinks below the constant.
    osd_spec_store(&s, pts, 8, 2);
    CHECK(osd_spec_service(&s, pts + 0.003, 8, false, 1.0 / 240) == OSD_SPEC_MISS,
          "tolerance scales with the cadence");
}

static void test_spec_plan_respects_horizon_and_eligibility(void)
{
    struct osd_spec s = {0};
    double pts = 0;

    CHECK(!osd_spec_plan(&s, 10.0, 0, INFINITY, true, &pts), "no cadence, no plan");
    CHECK(s.skips == 1, "skip counted");
    CHECK(!osd_spec_plan(&s, 10.0, FRAME, INFINITY, false, &pts), "ineligible");
    CHECK(s.skips == 2, "skip counted");

    CHECK(!osd_spec_plan(&s, 10.0, FRAME, 10.0 + FRAME / 2, true, &pts),
          "next frame past the decoders' read horizon");
    CHECK(s.horizon_skips == 1, "horizon skip counted");
    CHECK(!osd_spec_plan(&s, 10.0, FRAME, NAN, true, &pts),
          "unknown horizon renders nothing ahead");
    CHECK(s.horizon_skips == 2, "unknown horizon counted as a horizon skip");
    CHECK(osd_spec_plan(&s, 10.0, FRAME, 10.0 + FRAME, true, &pts),
          "a horizon exactly at the next frame allows it");

    osd_spec_store(&s, pts, 1, 0);
    CHECK(!osd_spec_plan(&s, 10.0, FRAME, INFINITY, true, &pts),
          "one pre-render at a time, not counted as a skip");
    CHECK(s.skips == 2, "kept pre-render is no skip");
}

static void test_swap_lead_is_half_the_release_interval_clamped(void)
{
    struct osd_swap_lead l;
    osd_swap_lead_reset(&l);
    CHECK(l.lead == OSD_SWAP_LEAD_MAX_NS, "conservative lead until measured");

    const int64_t period = 41708333;
    int64_t t = INT64_C(604800000000000);
    for (int n = 0; n < 4; n++)
        osd_swap_lead_observe(&l, t + n * period);
    CHECK(l.lead == OSD_SWAP_LEAD_MAX_NS, "three intervals are not a cadence");
    osd_swap_lead_observe(&l, t + 4 * period);
    CHECK(l.lead == OSD_SWAP_LEAD_MAX_NS, "23.976: half a frame clamps to 18 ms");

    osd_swap_lead_reset(&l);
    const int64_t period60 = 16666667;
    for (int n = 0; n < 6; n++)
        osd_swap_lead_observe(&l, t + n * period60);
    CHECK(l.lead == period60 / 2, "60 Hz: half a frame");

    osd_swap_lead_reset(&l);
    const int64_t period240 = 4166667;
    for (int n = 0; n < 6; n++)
        osd_swap_lead_observe(&l, t + n * period240);
    CHECK(l.lead == OSD_SWAP_LEAD_MIN_NS, "240 Hz: never below 6 ms");

    osd_swap_lead_observe(&l, t + 6 * period240 + INT64_C(2000000000));
    CHECK(l.lead == OSD_SWAP_LEAD_MAX_NS, "a two-second gap starts over");
}

static void test_dirty_union(void)
{
    struct osd_dirty d = {0};
    osd_dirty_add(&d, 10, 10, 10, 20);
    CHECK(!d.any, "an empty rect adds nothing");
    osd_dirty_add(&d, 100, 200, 300, 400);
    osd_dirty_add(&d, 50, 250, 120, 500);
    CHECK(d.any && d.x0 == 50 && d.y0 == 200 && d.x1 == 300 && d.y1 == 500,
          "bounding union of the folded regions");
}

int main(void)
{
    test_cadence_needs_four_consecutive_deltas();
    test_cadence_median_ignores_matroska_rounding();
    test_cadence_resets_on_gap_backwards_and_long_delta();
    test_latency_shift_needs_cadence();
    test_spec_hit_is_single_use_and_epoch_bound();
    test_spec_plan_respects_horizon_and_eligibility();
    test_swap_lead_is_half_the_release_interval_clamped();
    test_dirty_union();
    printf("mediacodec osd scheduler: all scenarios passed\n");
    return 0;
}
