// Simulation test for the vo_avfoundation timebase servo
// (patch/libmpv/0016-avfoundation-timebase-rate-servo.patch).
//
// The servo and playback-rate decision code is compiled VERBATIM from the
// patched mpv source. The harness mirrors the surrounding bookkeeping and
// models the CMTimebase plus mpv's audio-slaved frame schedule under
// disturbance shapes seen on real hardware: constant clock skew, wandering
// skew (HDMI audio PLLs), step discontinuities (route changes), playback
// speed changes, timestamp quantization, and bursty scheduling deadlines.
//
// What is asserted, per scenario:
//   - pacing: the timebase's projected media time stays within a few ms of
//     the schedule at every frame's presentation instant (sub-vsync, so
//     cadence stays stable);
//   - no spurious hard re-anchors (each one is a visible snap);
//   - genuine discontinuities still snap, exactly and promptly.
// The legacy behavior (20ms dead zone, snap-only correction) is run on the
// same inputs to document the failure being fixed: error wanders many ms
// deep into cadence-visible territory (#1776).

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define MP_TIME_US_TO_NS(us) ((us) * INT64_C(1000))
#define MP_TIME_MS_TO_NS(ms) ((ms) * INT64_C(1000000))
#define MP_TIME_S_TO_NS(s) ((s) * INT64_C(1000000000))
#define MPCLAMP(a, min, max) (((a) > (max)) ? (max) : (((a) < (min)) ? (min) : (a)))
#define MPMAX(a, b) ((a) > (b) ? (a) : (b))

#include "servo_core.inc"

// ---- CMTimebase model -----------------------------------------------------

struct sim_timebase {
    double time_ns;      // media time
    double rate;
    int64_t last_host_ns;
};

static void tb_advance(struct sim_timebase *tb, int64_t host_ns)
{
    tb->time_ns += (double)(host_ns - tb->last_host_ns) * tb->rate;
    tb->last_host_ns = host_ns;
}

// CMTimebaseSetRate: rate changes, current time continues.
static void tb_set_rate(struct sim_timebase *tb, int64_t host_ns, double rate)
{
    tb_advance(tb, host_ns);
    tb->rate = rate;
}

// CMTimebaseSetRateAndAnchorTime(rate, media_pts @ present_host).
static void tb_anchor(struct sim_timebase *tb, int64_t now_ns,
                      int64_t present_host_ns, int64_t media_pts_ns,
                      double rate)
{
    tb->rate = rate;
    tb->last_host_ns = now_ns;
    tb->time_ns = (double)media_pts_ns -
        (double)(present_host_ns - now_ns) * rate;
}

// ---- VO state mirroring update_media_timebase_for_frame() ------------------

struct sim_vo {
    struct sim_timebase tb;
    bool anchor_valid;
    double nominal_rate;   // p->timebase_rate
    double applied_rate;   // p->timebase_applied_rate
    int64_t anchor_media_pts_ns;     // legacy model only
    int64_t anchor_present_host_ns;  // legacy model only
    int64_t last_rate_update_ns;
    double servo_integral;
    int64_t last_servo_eval_ns;
    struct avf_rate_est rate_est;
    bool legacy;           // pre-servo behavior: 20ms dead zone + snap
    // counters
    int anchors;
    int rate_updates;
};

static void vo_init(struct sim_vo *vo, bool legacy)
{
    *vo = (struct sim_vo){
        .nominal_rate = 1.0,
        .applied_rate = 1.0,
        .legacy = legacy,
    };
}

static void vo_flip(struct sim_vo *vo, int64_t now, int64_t present_host_ns,
                    int64_t media_pts_ns, int64_t frame_duration_ns)
{
    tb_advance(&vo->tb, now);

    double rate = vo->nominal_rate > 0.0 ? vo->nominal_rate : 1.0;
    double sampled_rate = rate;
    bool sampled_valid = avf_rate_sample(
        &vo->rate_est, media_pts_ns, frame_duration_ns, rate,
        &sampled_rate);

    // Retain the old host-deadline inference only in the legacy comparison.
    bool inferred_valid = false;
    double inferred = rate;
    int64_t span_host_ns = 0;
    if (vo->legacy && vo->anchor_valid && vo->anchor_present_host_ns) {
        int64_t media_delta = media_pts_ns - vo->anchor_media_pts_ns;
        span_host_ns = present_host_ns - vo->anchor_present_host_ns;
        if (media_delta > 0 && span_host_ns >= MP_TIME_MS_TO_NS(5)) {
            double candidate = (double)media_delta / (double)span_host_ns;
            if (candidate >= AVF_RATE_MIN && candidate <= AVF_RATE_MAX) {
                inferred = candidate;
                inferred_valid = true;
            }
        }
    }

    bool rate_changed = false;
    if (!vo->legacy && sampled_valid &&
        fabs(sampled_rate - rate) > AVF_RATE_CHANGE_FRACTION * rate) {
        rate = sampled_rate;
        rate_changed = true;
    }
    if (vo->legacy && inferred_valid) {
        double noise_gate = (double)MP_TIME_MS_TO_NS(3) / (double)span_host_ns;
        double gate = MPMAX(0.01, noise_gate);
        if (fabs(inferred - rate) > gate * rate) {
            rate = inferred;
            rate_changed = true;
        }
    }

    bool should_anchor = !vo->anchor_valid || rate_changed;

    if (!should_anchor) {
        double applied = vo->applied_rate > 0.0 ? vo->applied_rate : rate;
        int64_t timebase_ns = (int64_t)vo->tb.time_ns;
        int64_t projected_ns = timebase_ns +
            (int64_t)((double)(present_host_ns - now) * applied);
        int64_t error_ns = media_pts_ns - projected_ns;

        if (vo->legacy) {
            if (llabs(error_ns) > MP_TIME_MS_TO_NS(20)) {
                should_anchor = true;
                if (inferred_valid && span_host_ns >= MP_TIME_MS_TO_NS(100))
                    rate = inferred;
            }
        } else {
            int64_t dt_ns = vo->last_servo_eval_ns
                ? now - vo->last_servo_eval_ns
                : 0;
            vo->last_servo_eval_ns = now;

            bool snap = false, update_rate = false;
            double servo = avf_servo_rate(error_ns, rate, applied, now,
                                          vo->last_rate_update_ns, dt_ns,
                                          &vo->servo_integral,
                                          &snap, &update_rate);
            if (snap) {
                should_anchor = true;
            } else if (update_rate) {
                tb_set_rate(&vo->tb, now, servo);
                vo->applied_rate = servo;
                vo->last_rate_update_ns = now;
                vo->rate_updates++;
            }
        }
    }

    if (should_anchor) {
        tb_anchor(&vo->tb, now, present_host_ns, media_pts_ns, rate);
        vo->nominal_rate = rate;
        vo->servo_integral = 0.0;
        vo->last_servo_eval_ns = 0;
        vo->applied_rate = rate;
        vo->last_rate_update_ns = now;
        vo->anchor_valid = true;
        vo->anchor_media_pts_ns = media_pts_ns;
        vo->anchor_present_host_ns = present_host_ns;
        vo->anchors++;
    }
}

// ---- schedule generator -----------------------------------------------------

// Deterministic PRNG (xorshift) for reproducible jitter.
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static double rng_uniform(void) // [-1, 1)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (double)(int64_t)rng_state / 9.223372036854776e18;
}

// Clock skew d(t): mpv's playback clock advances at (1 + d) vs the host.
struct disturbance {
    double skew;          // constant component
    double wander_amp;    // sinusoidal amplitude
    double wander_period; // seconds
    int64_t step_at_ns;   // host time of a one-shot schedule step (0 = none)
    int64_t step_ns;      // step size (audio clock jump)
    int64_t speed_at_ns;  // host time to switch playback speed (0 = none)
    double speed;         // new speed
    double jitter_ms;     // uniform schedule jitter amplitude
    int deadline_burst_every; // add a one-frame deadline delay every N frames
    double deadline_burst_ms;
};

struct result {
    int anchors_after_first;
    int rate_updates;
    double err_peak_ms;        // peak |error| after settle window
    double err_peak_early_ms;  // peak |error| inside settle window
    int64_t worst_at_ns;
    int64_t last_anchor_host_ns; // host time of the last non-initial anchor
    double final_nominal_rate;
};

static struct result simulate(bool legacy, double fps, double duration_s,
                              double settle_s, const struct disturbance *d)
{
    struct sim_vo vo;
    vo_init(&vo, legacy);
    rng_state = 0x9E3779B97F4A7C15ull;

    struct result res = {0};
    const int64_t lead_ns = MP_TIME_MS_TO_NS(100); // flip runs ahead of pts
    double speed = 1.0;
    double media_ns = 0;
    double host_ns = 1e9; // arbitrary epoch
    int64_t frames = (int64_t)(duration_s * fps);
    bool stepped = false;

    for (int64_t n = 0; n < frames; n++) {
        double frame_media_ns = 1e9 / fps;
        double t_s = host_ns / 1e9;
        double skew = d->skew;
        if (d->wander_amp > 0)
            skew += d->wander_amp *
                sin(2 * M_PI * t_s / d->wander_period);

        if (d->speed_at_ns && host_ns >= (double)d->speed_at_ns)
            speed = d->speed;

        // Advance the schedule: the playback clock reaches the next frame's
        // media pts after frame_media_ns/speed of clock time, which takes
        // (1 + skew) times less host time.
        media_ns += frame_media_ns;
        host_ns += frame_media_ns / speed / (1 + skew);

        if (d->step_at_ns && !stepped && host_ns >= (double)d->step_at_ns) {
            // Audio clock jumped forward: all subsequent frames are
            // scheduled earlier by the step.
            host_ns -= (double)d->step_ns;
            stepped = true;
        }

        int64_t present = (int64_t)(host_ns +
            d->jitter_ms * 1e6 * rng_uniform());
        if (d->deadline_burst_every > 0 &&
            n > 0 && n % d->deadline_burst_every == 0)
            present += (int64_t)(d->deadline_burst_ms * 1e6);
        int64_t now = present - lead_ns;
        int64_t media_pts = (int64_t)media_ns;

        int anchors_before = vo.anchors;
        vo_flip(&vo, now, present, media_pts,
                (int64_t)(frame_media_ns / speed));
        if (vo.anchors > anchors_before && vo.anchors > 1) {
            res.anchors_after_first++;
            res.last_anchor_host_ns = (int64_t)host_ns;
        }

        // Pacing metric: projected media time at the frame's true (jitter-
        // free) presentation instant vs its media pts. This is what decides
        // which vsync the layer picks.
        struct sim_timebase tb = vo.tb;
        tb_advance(&tb, (int64_t)host_ns);
        double err_ms = (media_pts - tb.time_ns) / 1e6;
        bool settled = host_ns / 1e9 > settle_s;
        // Exclude the immediate aftermath of injected discontinuities: the
        // snap itself is the correct, expected response there.
        if (d->step_at_ns && llabs((int64_t)host_ns - d->step_at_ns) <
                MP_TIME_S_TO_NS(3))
            settled = false;
        if (d->speed_at_ns && (int64_t)host_ns - d->speed_at_ns <
                MP_TIME_S_TO_NS(15) && (int64_t)host_ns >= d->speed_at_ns -
                MP_TIME_S_TO_NS(1))
            settled = false;
        if (settled && fabs(err_ms) > res.err_peak_ms) {
            res.err_peak_ms = fabs(err_ms);
            res.worst_at_ns = (int64_t)host_ns;
        }
        if (!settled && fabs(err_ms) > res.err_peak_early_ms)
            res.err_peak_early_ms = fabs(err_ms);
    }

    res.rate_updates = vo.rate_updates;
    res.final_nominal_rate = vo.nominal_rate;
    return res;
}

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        failures++; \
        printf("FAIL: " __VA_ARGS__); \
        printf("  [%s]\n", #cond); \
    } \
} while (0)

int main(void)
{
    // 1. Ideal clocks, scheduling jitter only.
    {
        struct disturbance d = { .jitter_ms = 1.5 };
        struct result r = simulate(false, 23.976, 1800, 10, &d);
        printf("jitter-only:      err_peak=%.2fms anchors=%d updates=%d\n",
               r.err_peak_ms, r.anchors_after_first, r.rate_updates);
        CHECK(r.anchors_after_first == 0, "jitter must not cause snaps\n");
        CHECK(r.err_peak_ms < 2.0, "err %.2fms\n", r.err_peak_ms);
    }

    // 2. Bursty host deadlines observed with compressed AVPlayer audio:
    //    one frame arrives 30ms late every five seconds. This must remain
    //    phase noise, not be misclassified as a playback-speed change.
    {
        struct disturbance d = { .jitter_ms = 1.5,
                                 .deadline_burst_every = 120,
                                 .deadline_burst_ms = 30.0 };
        struct result servo = simulate(false, 23.976, 600, 10, &d);
        struct result legacy = simulate(true, 23.976, 600, 10, &d);
        printf("bursty deadlines: servo anchors=%d | legacy anchors=%d\n",
               servo.anchors_after_first, legacy.anchors_after_first);
        CHECK(servo.anchors_after_first == 0,
              "deadline bursts must not change rate or snap\n");
        CHECK(legacy.anchors_after_first > 0,
              "legacy host-deadline inference should reproduce the bug\n");
    }

    // 3. Severe constant skew (500ppm; real HDMI clocks are usually <100).
    {
        struct disturbance d = { .skew = 500e-6, .jitter_ms = 1.0 };
        struct result r = simulate(false, 23.976, 3600, 10, &d);
        printf("skew 500ppm:      err_peak=%.2fms anchors=%d updates=%d\n",
               r.err_peak_ms, r.anchors_after_first, r.rate_updates);
        CHECK(r.anchors_after_first == 0, "constant skew must be slewed, not snapped\n");
        CHECK(r.err_peak_ms < 3.0, "err %.2fms\n", r.err_peak_ms);
    }

    // 4. Wandering skew (PLL temperature wander).
    {
        struct disturbance d = { .skew = 30e-6, .wander_amp = 300e-6,
                                 .wander_period = 120, .jitter_ms = 1.5 };
        struct result r = simulate(false, 23.976, 3600, 10, &d);
        printf("wander 300ppm:    err_peak=%.2fms anchors=%d updates=%d\n",
               r.err_peak_ms, r.anchors_after_first, r.rate_updates);
        CHECK(r.anchors_after_first == 0, "wander must not cause snaps\n");
        CHECK(r.err_peak_ms < 3.0, "err %.2fms\n", r.err_peak_ms);
    }

    // 5. Slow, large wander: the shape that pushed the legacy dead zone into
    //    cadence-visible error. Servo must hold it to sub-vsync error.
    {
        struct disturbance d = { .skew = 30e-6, .wander_amp = 150e-6,
                                 .wander_period = 900, .jitter_ms = 1.5 };
        struct result servo = simulate(false, 23.976, 3600, 10, &d);
        struct result legacy = simulate(true, 23.976, 3600, 10, &d);
        printf("slow wander:      servo err=%.2fms anchors=%d | "
               "legacy err=%.2fms anchors=%d\n",
               servo.err_peak_ms, servo.anchors_after_first,
               legacy.err_peak_ms, legacy.anchors_after_first);
        CHECK(servo.err_peak_ms < 3.0, "servo err %.2fms\n", servo.err_peak_ms);
        CHECK(servo.anchors_after_first == 0, "servo must not snap\n");
        // Documents the defect: legacy error wanders several ms deep into
        // the dead zone, flapping 24@60 cadence (#1776).
        CHECK(legacy.err_peak_ms > 8.0,
              "legacy expected to drift visibly, got %.2fms\n",
              legacy.err_peak_ms);
    }

    // 6. Audio-clock step (route renegotiation): one prompt snap, then clean.
    {
        struct disturbance d = { .skew = 30e-6, .jitter_ms = 1.0,
                                 .step_at_ns = MP_TIME_S_TO_NS(600),
                                 .step_ns = MP_TIME_MS_TO_NS(80) };
        struct result r = simulate(false, 23.976, 1200, 10, &d);
        printf("step 80ms:        err_peak=%.2fms anchors=%d updates=%d\n",
               r.err_peak_ms, r.anchors_after_first, r.rate_updates);
        CHECK(r.anchors_after_first == 1, "step must snap exactly once, got %d\n",
              r.anchors_after_first);
        CHECK(r.err_peak_ms < 3.0, "err %.2fms after recovery\n", r.err_peak_ms);
    }

    // 7. Playback speed change 1.0 -> 1.5: the frame-duration estimator
    //    adopts the new nominal rate within a few frames, then runs clean.
    {
        struct disturbance d = { .skew = 30e-6, .jitter_ms = 1.0,
                                 .speed_at_ns = MP_TIME_S_TO_NS(300),
                                 .speed = 1.5 };
        struct result r = simulate(false, 23.976, 900, 10, &d);
        printf("speed 1.5x:       err_peak=%.2fms anchors=%d updates=%d\n",
               r.err_peak_ms, r.anchors_after_first, r.rate_updates);
        CHECK(r.anchors_after_first >= 1 && r.anchors_after_first <= 2,
              "speed change should settle in <=2 anchors, got %d\n",
              r.anchors_after_first);
        CHECK(fabs(r.final_nominal_rate - d.speed) < 0.01,
              "expected nominal rate %.2f, got %.6f\n",
              d.speed, r.final_nominal_rate);
        CHECK(r.last_anchor_host_ns - d.speed_at_ns < MP_TIME_S_TO_NS(5),
              "snaps must cluster at the change, last at +%.1fs\n",
              (r.last_anchor_host_ns - d.speed_at_ns) / 1e9);
        CHECK(r.err_peak_ms < 3.0, "err %.2fms after adoption\n", r.err_peak_ms);
    }

    // 8. A small speed change stays below the fast-path threshold and must be
    //    adopted by the quantization-resistant 500ms estimator instead.
    {
        struct disturbance d = { .skew = 30e-6, .jitter_ms = 1.0,
                                 .speed_at_ns = MP_TIME_S_TO_NS(300),
                                 .speed = 1.02 };
        struct result r = simulate(false, 23.976, 900, 10, &d);
        printf("speed 1.02x:      err_peak=%.2fms anchors=%d updates=%d\n",
               r.err_peak_ms, r.anchors_after_first, r.rate_updates);
        CHECK(r.anchors_after_first >= 1 && r.anchors_after_first <= 2,
              "small speed change should settle in <=2 anchors, got %d\n",
              r.anchors_after_first);
        CHECK(fabs(r.final_nominal_rate - d.speed) < 0.005,
              "expected nominal rate %.2f, got %.6f\n",
              d.speed, r.final_nominal_rate);
        CHECK(r.err_peak_ms < 3.0, "err %.2fms after adoption\n", r.err_peak_ms);
    }

    // 9. 50fps content (PAL): same guarantees at a tighter frame budget.
    {
        struct disturbance d = { .skew = 100e-6, .wander_amp = 200e-6,
                                 .wander_period = 180, .jitter_ms = 1.0 };
        struct result r = simulate(false, 50, 3600, 10, &d);
        printf("50fps wander:     err_peak=%.2fms anchors=%d updates=%d\n",
               r.err_peak_ms, r.anchors_after_first, r.rate_updates);
        CHECK(r.anchors_after_first == 0, "wander must not cause snaps\n");
        CHECK(r.err_peak_ms < 3.0, "err %.2fms\n", r.err_peak_ms);
    }

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all timebase servo invariants hold\n");
    return 0;
}
