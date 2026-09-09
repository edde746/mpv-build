// Behavioral regressions for the exact freestanding OSD scheduler core
// extracted from patch 0001 by test_mediacodec_osd.sh: the cadence estimator
// that predicts the next frame's pts, the single-use next-frame pre-render,
// the read horizon cap, the event pre-render planner and its serve window,
// and the swap lead. No libass, EGL or mpv thread is modelled here.
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// The macros the core takes from mpv's common/common.h.
#define MP_NOPTS_VALUE (-0x1p+63)
#define MPMAX(a, b) ((a) > (b) ? (a) : (b))
#define MPMIN(a, b) ((a) > (b) ? (b) : (a))
#define MPCLAMP(a, min, max) (((a) < (min)) ? (min) : (((a) > (max)) ? (max) : (a)))

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

static struct osd_prefetch static_prefetch(void)
{
    struct osd_prefetch p = {0};
    p.unchanged_streak = OSD_PREFETCH_STATIC_RENDERS;
    return p;
}

static void test_prefetch_plan_gates(void)
{
    struct osd_prefetch p = static_prefetch();
    CHECK(osd_prefetch_plan(&p, 10.0, 12.0, 0.05, MP_NOPTS_VALUE, INFINITY, true),
          "a static OSD plans the next event");
    CHECK(!osd_prefetch_plan(&p, 10.0, 12.0, 0.05, MP_NOPTS_VALUE, INFINITY, false),
          "ineligible");
    p.unchanged_streak = OSD_PREFETCH_STATIC_RENDERS - 1;
    CHECK(!osd_prefetch_plan(&p, 10.0, 12.0, 0.05, MP_NOPTS_VALUE, INFINITY, true),
          "an OSD still changing is not pre-rendered ahead");
    p = static_prefetch();
    p.valid = true;
    CHECK(!osd_prefetch_plan(&p, 10.0, 12.0, 0.05, MP_NOPTS_VALUE, INFINITY, true),
          "one kept pre-render at a time");
    p = static_prefetch();
    CHECK(!osd_prefetch_plan(&p, 10.0, 10.0 + OSD_PREFETCH_MIN_LEAD / 2, 0.05,
                             MP_NOPTS_VALUE, INFINITY, true),
          "too close to render ahead of");
    CHECK(!osd_prefetch_plan(&p, 10.0, 10.0 + OSD_PREFETCH_MAX_LEAD + 1, 0.05,
                             MP_NOPTS_VALUE, INFINITY, true),
          "too far to hold a slot for");
    CHECK(!osd_prefetch_plan(&p, 10.0, 12.0, 0.05, MP_NOPTS_VALUE, 11.0, true),
          "beyond the decoders' horizon");
    CHECK(!osd_prefetch_plan(&p, 10.0, 12.0, 0.05, MP_NOPTS_VALUE, NAN, true),
          "unknown horizon");
    CHECK(!osd_prefetch_plan(&p, 10.0, MP_NOPTS_VALUE, 0.05, MP_NOPTS_VALUE, INFINITY, true),
          "no next event");
    osd_prefetch_store(&p, 12.0, false, 3, 1);
    CHECK(p.valid && p.slot == 1 && p.count == 1, "stored");
    p.valid = false;
    CHECK(!osd_prefetch_plan(&p, 10.5, 12.0, 0.05, MP_NOPTS_VALUE, INFINITY, true),
          "the same event is not rendered twice");
    CHECK(osd_prefetch_plan(&p, 12.5, 14.0, 0.05, MP_NOPTS_VALUE, INFINITY, true),
          "the following event is");
}

static struct osd_frame_load load_of(int active, int cold)
{
    return (struct osd_frame_load){
        .active = active, .cold = cold,
        .boundary = MP_NOPTS_VALUE, .next_boundary = MP_NOPTS_VALUE,
        .visible_end = MP_NOPTS_VALUE,
    };
}

static void test_cost_separates_fixed_cold_and_warm(void)
{
    struct osd_cost c = {0};
    struct osd_frame_load line = load_of(1, 1);
    struct osd_frame_load sign_cold = load_of(1735, 1735);
    struct osd_frame_load sign_warm = load_of(1735, 0); // rendered a frame ago
    CHECK_NEAR(osd_cost_estimate(&c, &line, 1.0), OSD_COST_DEFAULT_FIXED + OSD_COST_DEFAULT_COLD,
               "defaults before any measurement");
    osd_cost_observe(&c, &line, 0.022); // a dialogue line took 22 ms
    CHECK_NEAR(osd_cost_estimate(&c, &line, 1.0), 0.022 + OSD_COST_DEFAULT_COLD,
               "a light render sets the fixed part");
    CHECK_NEAR(osd_cost_estimate(&c, &sign_cold, 1.0), 0.022 + 1735 * OSD_COST_DEFAULT_COLD,
               "and not the per-event rate: 22 ms is not 22 ms per event");
    osd_cost_observe(&c, &sign_cold, 0.022 + 1735 * 0.00013); // 0.13 ms per cold event
    CHECK_NEAR(osd_cost_estimate(&c, &sign_cold, 1.0), 0.022 + 1735 * 0.00013,
               "a cold render sets the cold rate");
    CHECK_NEAR(osd_cost_estimate(&c, &sign_cold, 1.25), 0.022 + 1.25 * 1735 * 0.00013,
               "the margin scales the per-event part only");
    CHECK_NEAR(osd_cost_estimate(&c, &sign_warm, 1.0), 0.022 + 1735 * OSD_COST_DEFAULT_WARM,
               "the same sign a frame later is charged the warm rate");
    struct osd_frame_load w131 = load_of(131, 0);
    osd_cost_observe(&c, &w131, 0.022 + 131 * 0.00001); // re-rendering a sign: 0.01 ms per event
    CHECK_NEAR(osd_cost_estimate(&c, &sign_cold, 1.0), 0.022 + 1735 * 0.00013,
               "a warm render leaves the cold rate alone");
    CHECK_NEAR(osd_cost_estimate(&c, &sign_warm, 1.0), 0.022 + 1735 * 0.00001,
               "and sets the warm one");
    struct osd_frame_load cached = load_of(100, 100);
    for (int n = 0; n < 3; n++)
        osd_cost_observe(&c, &cached, 0.022); // 100 "cold" events libass had cached: free
    CHECK(osd_cost_estimate(&c, &sign_cold, 1.0) > 0.022 + 1735 * 0.00013 * 0.75,
          "three free 100-event renders barely move a rate set by a 1735-event one");
    struct osd_cost fresh = {0};
    osd_cost_observe(&fresh, &line, 0.022);
    osd_cost_observe(&fresh, &sign_cold, 0.022 + 1735 * 0.00013);
    osd_cost_observe(&fresh, &sign_cold, 0.022 + 1735 * 0.00026); // twice the rate
    double rate = osd_cost_rate(&fresh, true);
    CHECK(rate > 0.00013 && rate < 0.00026, "recent cold renders are averaged, not maxed");
    struct osd_frame_load mixed = load_of(1178, 626); // 626 new over a 552-event sign
    CHECK_NEAR(osd_cost_estimate(&fresh, &mixed, 1.0),
               0.022 + 626 * rate + 552 * OSD_COST_DEFAULT_WARM,
               "only the events the last render lacked are charged cold");
    struct osd_frame_load few = load_of(8, 8);
    osd_cost_observe(&fresh, &few, 0.5); // neither light nor heavy: not attributable
    CHECK_NEAR(osd_cost_rate(&fresh, true), rate, "unchanged");
    struct osd_frame_load fast = load_of(100, 100);
    osd_cost_observe(&fresh, &fast, 0.001); // faster than the fixed part
    CHECK(osd_cost_rate(&fresh, true) >= 0, "the cold rate never goes negative");
    struct osd_frame_load huge = load_of(1000000, 1000000);
    CHECK_NEAR(osd_cost_estimate(&fresh, &huge, 1.0), OSD_COST_MAX, "capped");
}

static void test_ahead_plan(void)
{
    double target = 0;
    CHECK(osd_ahead_plan(0.5 * FRAME, FRAME, 10.0, MP_NOPTS_VALUE, false, INFINITY, &target)
          == OSD_AHEAD_NONE, "a render that fits its frame is rendered as asked");
    CHECK(osd_ahead_plan(0.25, 0, 10.0, MP_NOPTS_VALUE, false, INFINITY, &target)
          == OSD_AHEAD_NONE, "no cadence, no prediction");
    CHECK(osd_ahead_plan(0.25, FRAME, 10.0, MP_NOPTS_VALUE, false, INFINITY, &target)
          == OSD_AHEAD_TARGET && target == 10.25,
          "a hopeless one renders a frame current when it finishes");
    CHECK(osd_ahead_plan(0.25, FRAME, 10.0, MP_NOPTS_VALUE, false, 10.2, &target)
          == OSD_AHEAD_NONE, "unless that frame is past the decoders' horizon");
    CHECK(osd_ahead_plan(0.25, FRAME, 10.0, MP_NOPTS_VALUE, false, NAN, &target)
          == OSD_AHEAD_NONE, "or the horizon is unknown");
    CHECK(osd_ahead_plan(0.25, FRAME, 10.0, 10.25, false, INFINITY, &target)
          == OSD_AHEAD_SKIP, "a held frame at the target is already the answer");
    CHECK(osd_ahead_plan(0.25, FRAME, 10.0, 10.4, false, INFINITY, &target)
          == OSD_AHEAD_SKIP, "or past it");
    CHECK(osd_ahead_plan(0.25, FRAME, 10.0, 10.1, false, INFINITY, &target)
          == OSD_AHEAD_TARGET, "a held frame inside the run is replaced");
    CHECK(osd_ahead_plan(0.25, FRAME, 10.0, 10.1, true, INFINITY, &target)
          == OSD_AHEAD_SKIP, "unless it persists: the sign the run leads to");
    CHECK(osd_ahead_plan(0.25, FRAME, 10.0, 9.9, true, INFINITY, &target)
          == OSD_AHEAD_TARGET, "a held frame already due is not waited for");
}

static void test_ahead_hold_wants_a_frame_ready_in_time_that_persists(void)
{
    // Ex1's zoom-out: playing at 16.267 with an 86 ms render ahead. The
    // window reaches a quarter second out, past the intro to the sign.
    const double pts = 16.267;
    const double window = osd_ahead_window(pts, 0.086);
    CHECK_NEAR(window, pts + OSD_AHEAD_MIN_WINDOW, "a short estimate still looks a quarter second ahead");
    CHECK_NEAR(osd_ahead_window(pts, 0.3), pts + 0.6, "a long one twice its length");
    struct osd_frame_load intro = load_of(552, 552);
    intro.visible_end = 16.37; intro.next_boundary = 16.35; intro.next_starting = 4;
    struct osd_frame_load sign = load_of(1178, 1178);
    sign.visible_end = 18.14; sign.next_boundary = 16.5; sign.next_starting = 1;
    CHECK(!osd_ahead_hold(pts, FRAME, window, 16.33, 0.089, INFINITY, &intro),
          "an intro frame that lasts 40 ms is not worth holding");
    CHECK(osd_ahead_hold(pts, FRAME, window, 16.43, 0.134, INFINITY, &sign),
          "the sign after it is: ready in time, stays, light boundary next");
    CHECK(!osd_ahead_hold(pts, FRAME, window, 16.43, 0.25, INFINITY, &sign),
          "not when it cannot be ready within a frame of its start");
    CHECK(osd_ahead_hold(pts, FRAME, window, 16.43, 0.19, INFINITY, &sign),
          "one frame late is accepted");
    CHECK(!osd_ahead_hold(pts, FRAME, window, 16.43, 0.134, 16.4, &sign),
          "not past the decoders' horizon");
    struct osd_frame_load half = sign;
    half.next_boundary = 16.43; half.next_starting = 626;
    CHECK(!osd_ahead_hold(pts, FRAME, window, 16.41, 0.089, INFINITY, &half),
          "not a frame the next boundary redoes with 626 new events");
    CHECK(!osd_ahead_hold(pts, FRAME, window, window + 0.01, 0.134, INFINITY, &sign),
          "not past the window");
    CHECK(!osd_ahead_hold(pts, FRAME, window, pts, 0.01, INFINITY, &sign),
          "not the request's own frame");
    struct osd_frame_load open = sign;
    open.next_boundary = MP_NOPTS_VALUE;
    CHECK(osd_ahead_hold(pts, FRAME, window, 16.43, 0.134, INFINITY, &open), "no boundary after is fine");
    struct osd_frame_load brief = sign;
    brief.visible_end = 16.43 + OSD_AHEAD_MIN_LIFE / 2;
    CHECK(!osd_ahead_hold(pts, FRAME, window, 16.43, 0.134, INFINITY, &brief),
          "a sign gone within the minimum life is not");
}

static void test_prefetch_plan_fits_before_visible_end_but_may_overrun_start(void)
{
    struct osd_prefetch p = static_prefetch();
    const double sign = 0.5, line = 0.05; // estimates
    CHECK(!osd_prefetch_plan(&p, 10.0, 12.0, sign, 10.3, INFINITY, true),
          "a line ending mid-render would wait for it");
    CHECK(osd_prefetch_plan(&p, 10.0, 12.0, sign, 10.9, INFINITY, true),
          "a line ending after the render, with margin, does not");
    CHECK(osd_prefetch_plan(&p, 10.0, 12.0, sign, 13.0, INFINITY, true),
          "a line outliving the event is no constraint");
    CHECK(osd_prefetch_plan(&p, 10.0, 10.3, sign, MP_NOPTS_VALUE, INFINITY, true),
          "a render that may overrun the start by less than its lead is attempted");
    CHECK(!osd_prefetch_plan(&p, 10.0, 10.24, sign, MP_NOPTS_VALUE, INFINITY, true),
          "a hopeless one is not");
    CHECK(osd_prefetch_plan(&p, 10.0, 10.24, line, MP_NOPTS_VALUE, INFINITY, true),
          "a dialogue line that close is");
}

static void test_prefetch_service_window(void)
{
    struct osd_prefetch p = static_prefetch();
    osd_prefetch_store(&p, 10.0, false, 3, 2);
    CHECK(osd_prefetch_service(&p, 10.0 - FRAME, 3, FRAME) == OSD_PREFETCH_NONE,
          "the frame before the event keeps the pre-render");
    CHECK(p.valid, "still kept");
    CHECK(!osd_prefetch_covers(&p, 10.0 - FRAME, FRAME), "the next-frame pre-render still covers that frame");
    CHECK(osd_prefetch_covers(&p, 10.0 + 0.001, FRAME), "the event's first frame is covered");
    CHECK(osd_prefetch_service(&p, 10.0 + 0.001, 3, FRAME) == OSD_PREFETCH_HIT,
          "the event's first frame is served");
    CHECK(!p.valid && p.hits == 1, "single use");
    CHECK(osd_prefetch_service(&p, 10.0 + FRAME, 3, FRAME) == OSD_PREFETCH_NONE,
          "nothing kept afterwards");

    osd_prefetch_store(&p, 20.0, false, 3, 2);
    CHECK(osd_prefetch_service(&p, 20.0 + (OSD_PREFETCH_SERVE_FRAMES - 1) * FRAME, 3, FRAME)
          == OSD_PREFETCH_HIT, "a request coalesced past the start is still served");
    osd_prefetch_store(&p, 30.0, false, 3, 2);
    CHECK(osd_prefetch_service(&p, 30.0 + OSD_PREFETCH_SERVE_FRAMES * FRAME, 3, FRAME)
          == OSD_PREFETCH_EXPIRED, "too late to show the first frame");
    CHECK(!p.valid, "dropped");
    osd_prefetch_store(&p, 40.0, false, 3, 2);
    CHECK(osd_prefetch_service(&p, 40.0, 4, FRAME) == OSD_PREFETCH_EXPIRED,
          "a seek invalidates it");
    CHECK(p.hits == 2, "hits counted");
}

static void test_prefetch_streak_counts_unchanged_renders(void)
{
    struct osd_prefetch p = {0};
    osd_prefetch_observe_render(&p, false);
    osd_prefetch_observe_render(&p, false);
    CHECK(p.unchanged_streak == 2, "two unchanged renders");
    osd_prefetch_observe_render(&p, true);
    CHECK(p.unchanged_streak == 0, "a change starts over");
}

int main(void)
{
    test_cadence_needs_four_consecutive_deltas();
    test_cadence_median_ignores_matroska_rounding();
    test_cadence_resets_on_gap_backwards_and_long_delta();
    test_spec_hit_is_single_use_and_epoch_bound();
    test_spec_plan_respects_horizon_and_eligibility();
    test_swap_lead_is_half_the_release_interval_clamped();
    test_prefetch_plan_gates();
    test_cost_separates_fixed_cold_and_warm();
    test_ahead_plan();
    test_ahead_hold_wants_a_frame_ready_in_time_that_persists();
    test_prefetch_plan_fits_before_visible_end_but_may_overrun_start();
    test_prefetch_service_window();
    test_prefetch_streak_counts_unchanged_renders();
    printf("mediacodec osd scheduler: all scenarios passed\n");
    return 0;
}
