// Behavioral regressions for vo_avfoundation's host-mode clock rules,
// compiled verbatim from the final Apple series by test_avf_host_present.sh:
// - avf_host_present_ns: a seek must not let a frame queued before it be
//   shown after one decoded after it (plezy#2823), and the rule must never
//   hold the picture back past the queue lead.
// - avf_host_timebase_drifted: a reset re-seeds the timebase, a clock write
//   that retimes the display layers, only when it has really left
//   mp_time_ns(); a thread descheduled between the clock reads must not count.
// - avf_host_refresh_stamp_ns: a stamp sits 30% into a refresh interval, away
//   from where the video and OSD layers can settle a refresh apart after a
//   seek (plezy#3073), moves by at most half a refresh, keeps the frames'
//   order, and content at the display's rate gets a refresh of its own for
//   every frame.
// - avf_host_restart_stamp_ns: a frame after a seek or a resume reaches the
//   layers at least 3.5 refreshes before the refresh that shows it, moved on
//   by whole refreshes and no further than that takes (plezy#3073).
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "avf_host_present.inc"
#include "avf_host_timebase_drift.inc"
#include "avf_host_refresh_stamp.inc"

#define MS INT64_C(1000000)

static int failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

// The refresh that shows a stamp: the first vsync at or after it.
static int64_t refresh_of(int64_t t, int64_t vsync, int64_t period)
{
    int64_t rest = ((t - vsync) % period + period) % period;
    return rest ? t + period - rest : t;
}

int main(void)
{
    const int64_t now = 1000 * MS, window = (128 + 50) * MS;
    const int64_t frame = 41708333; // 23.976 fps

    // Ordinary playback: each target is after the last stamp and is kept.
    int64_t last = 0;
    bool kept = true;
    for (int i = 0; i < 48; i++) {
        int64_t target = now + i * frame;
        int64_t stamp = avf_host_present_ns(last, target, window);
        kept = kept && stamp == target;
        last = stamp;
    }
    check(kept, "steady playback keeps every frame on its own target");

    // A seek while frames are queued up to the 128 ms lead: the old frames
    // keep their stamps, and the first post-seek frames are due now. Every
    // post-seek stamp must come after the last pre-seek one, in order.
    int64_t old_last = now + 120 * MS;
    last = old_last;
    int64_t prev = old_last;
    bool after_old = true, ordered = true;
    for (int i = 0; i < 6; i++) {
        int64_t stamp = avf_host_present_ns(last, now + i * frame, window);
        after_old = after_old && stamp > old_last;
        ordered = ordered && stamp > prev;
        prev = stamp;
        last = stamp;
    }
    check(after_old, "no post-seek frame is stamped before a pre-seek one");
    check(ordered, "post-seek stamps stay strictly increasing");

    // Post-seek frames whose targets are already past the old frames keep
    // their own timing: the rule delays nothing it does not have to.
    check(avf_host_present_ns(old_last, now + 125 * MS, window) == now + 125 * MS,
          "a target past the last stamp is untouched");
    check(avf_host_present_ns(old_last, old_last, window) == old_last + 1,
          "a target equal to the last stamp moves just after it");

    // A last stamp further ahead than any queued frame can be is not a frame
    // the display holds: honoring it would hold the picture, so it is ignored.
    check(avf_host_present_ns(now + window + 1 * MS, now, window) == now,
          "a last stamp beyond the window never delays a frame");
    check(avf_host_present_ns(now + window, now, window) == now + window + 1,
          "a last stamp exactly at the window still orders the frame");

    // No delay is ever more than the window.
    bool bounded = true;
    for (int64_t ahead = 0; ahead <= 400 * MS; ahead += 1 * MS) {
        int64_t stamp = avf_host_present_ns(now + ahead, now, window);
        bounded = bounded && stamp - now <= window + 1;
    }
    check(bounded, "a frame is never stamped more than the window past its target");

    // The timebase drift check. Reads are mp_time_ns() before and after the
    // timebase read; tolerance as in vo_avfoundation.
    const int64_t tol = 5 * MS;
    check(!avf_host_timebase_drifted(now, now, now, 1.0, tol),
          "an untouched timebase is not drifted");
    check(!avf_host_timebase_drifted(now, now + 20 * MS, now + 20 * MS, 1.0, tol),
          "a thread descheduled before the timebase read is not drift");
    check(!avf_host_timebase_drifted(now, now, now + 20 * MS, 1.0, tol),
          "a thread descheduled after the timebase read is not drift");
    check(!avf_host_timebase_drifted(now, now - 4 * MS, now, 1.0, tol) &&
          !avf_host_timebase_drifted(now, now + 4 * MS, now, 1.0, tol),
          "an offset within the tolerance is not drift");
    check(avf_host_timebase_drifted(now, now - 20 * MS, now, 1.0, tol),
          "a timebase behind the clock is drifted");
    check(avf_host_timebase_drifted(now, now + 20 * MS, now + 1 * MS, 1.0, tol),
          "a timebase ahead of the clock is drifted");
    check(avf_host_timebase_drifted(now, now - 900 * MS, now + 20 * MS, 1.0, tol),
          "a timebase clamped far back is drifted even across a long read");
    check(avf_host_timebase_drifted(now, now, now, 0.0, tol),
          "a stopped timebase is drifted");

    // Refresh stamps. A 24 Hz display (the refresh matched to 23.976 fps)
    // with a vsync at an arbitrary time; targets every 0.1 ms over three
    // refreshes either side of it, so before it too.
    const int64_t period = 41708333, vsync = now + 7 * MS;
    const int64_t phase = period * 3 / 10;
    bool on_phase = true, near = true, in_order = true;
    int64_t prev_stamp = INT64_MIN;
    for (int64_t t = vsync - 3 * period; t <= vsync + 3 * period; t += MS / 10) {
        int64_t stamp = avf_host_refresh_stamp_ns(t, vsync, period);
        int64_t into = ((stamp - vsync) % period + period) % period;
        on_phase = on_phase && into == phase;
        near = near && (stamp > t ? stamp - t : t - stamp) * 2 <= period;
        in_order = in_order && stamp >= prev_stamp;
        prev_stamp = stamp;
    }
    check(on_phase, "every stamp sits 30% of the way through a refresh");
    check(near, "no stamp moves by more than half a refresh");
    check(in_order, "a later target never gets an earlier stamp");
    check(avf_host_refresh_stamp_ns(vsync + 2 * period + phase, vsync, period) ==
              vsync + 2 * period + phase,
          "a target already 30% into a refresh keeps its stamp");

    // Content at the display's rate, from every start within a refresh (late
    // in the interval, where the layers can settle a refresh apart, among
    // them): one refresh per frame, none shared and none skipped.
    bool one_each = true;
    for (int64_t start = 0; start < period; start += period / 50) {
        int64_t prev = avf_host_refresh_stamp_ns(vsync + start, vsync, period);
        for (int i = 1; i < 48; i++) {
            int64_t stamp = avf_host_refresh_stamp_ns(vsync + start + i * period,
                                                      vsync, period);
            one_each = one_each && stamp - prev == period;
            prev = stamp;
        }
    }
    check(one_each, "frames at the display's rate get one refresh each");

    check(avf_host_refresh_stamp_ns(now + 5 * MS, 0, 0) == now + 5 * MS,
          "without a refresh seen the target is kept");

    // Restart stamps: stamps 30% into a refresh, flipped from 8 refreshes
    // before them to 2 after (late), every 1 ms.
    bool later = true, whole = true, enough = true, no_further = true,
         far_kept = true;
    for (int64_t k = -2; k < 12; k++) {
        int64_t stamp = vsync + k * period + phase;
        for (int64_t flip = stamp - 8 * period; flip <= stamp + 2 * period; flip += MS) {
            int64_t moved = avf_host_restart_stamp_ns(stamp, flip, vsync, period);
            int64_t lead = refresh_of(moved, vsync, period) - flip;
            later = later && moved >= stamp;
            whole = whole && (moved - stamp) % period == 0;
            enough = enough && lead * 2 >= 7 * period;
            if (moved != stamp)
                no_further = no_further && (lead - period) * 2 < 7 * period;
            if ((refresh_of(stamp, vsync, period) - flip) * 2 >= 7 * period)
                far_kept = far_kept && moved == stamp;
        }
    }
    check(later && whole, "a restart stamp only moves later, by whole refreshes");
    check(enough, "a restart frame's refresh is at least 3.5 refreshes after its flip");
    check(no_further, "a restart stamp moves no further than that takes");
    check(far_kept, "a restart stamp already that far ahead is kept");
    check(avf_host_restart_stamp_ns(now + 5 * MS, now, 0, 0) == now + 5 * MS,
          "without a refresh seen a restart stamp is kept");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("avf host clock rules: all checks passed\n");
    return EXIT_SUCCESS;
}
