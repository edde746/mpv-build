// Behavioral regressions for vo_avfoundation's subtitle wait policy
// (avf_osd_wait_until), compiled verbatim from the final Apple series by
// test_avf_osd_ahead.sh: how long flip_page may hold a frame's video for
// subtitles the shared render-ahead pipeline has not delivered yet.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "avf_osd_ahead_policy.inc"

#define MS INT64_C(1000000)

static int failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

int main(void)
{
    const int64_t now = 1000 * MS, lead = 128 * MS;

    // A frame drawn a full queue lead early may wait until half of it is
    // left, and no longer: the video keeps 64 ms to be enqueued in.
    int64_t until = avf_osd_wait_until(now, now + lead, lead, true, false, false);
    check(until == now + 64 * MS, "a timed frame waits until half its lead is left");

    // Once half the lead is gone the video is not held at all.
    check(avf_osd_wait_until(now, now + 60 * MS, lead, true, false, false) == 0,
          "a frame already inside half its lead does not wait");
    check(avf_osd_wait_until(now, now - 5 * MS, lead, true, false, false) == 0,
          "a late frame does not wait");

    // With the render thread still on an older frame, waiting buys only part
    // of that render: the frame shows what is ready instead.
    check(avf_osd_wait_until(now, now + lead, lead, true, false, true) == 0,
          "a frame does not wait while the render thread is behind");

    // A shorter queue lead shrinks the wait with it.
    check(avf_osd_wait_until(now, now + 50 * MS, 50 * MS, true, false, false) ==
          now + 25 * MS, "the wait scales with the configured queue lead");

    // Untimed frames (redraws) have no deadline, only a bound.
    int64_t redraw = avf_osd_wait_until(now, now, lead, false, false, false);
    check(redraw > now && redraw <= now + 1000 * MS,
          "an untimed frame waits a bounded time");

    // The composite modes draw the subtitle into the video sample and keep
    // the synchronous path's ordering, behind or not.
    int64_t composite = avf_osd_wait_until(now, now + lead, lead, true, true, true);
    check(composite > now + lead, "a composite frame waits for its subtitles");

    if (failures)
        return 1;
    printf("avfoundation subtitle wait policy: all checks passed\n");
    return 0;
}
