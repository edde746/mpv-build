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

    // --avfoundation-osd-queue-lead-ms: a change waits as a bitmap list and
    // goes to the layer at the last flip that leaves the lead before its
    // frame. Flips come one frame spacing apart, a queue lead before their
    // own frame: walk them and see when a change for frame 0 is sent.
    static const int64_t spacings[] = {41708333, 40000000, 33366666, 16683333, 8341666};
    static const int64_t leads[] = {20 * MS, 40 * MS, 60 * MS, 100 * MS};
    bool margin_ok = true, last_ok = true, early_ok = true;
    for (size_t si = 0; si < sizeof(spacings) / sizeof(spacings[0]); si++) {
        for (size_t li = 0; li < sizeof(leads) / sizeof(leads[0]); li++) {
            int64_t spacing = spacings[si], want = leads[li], frame = 5000 * MS;
            int64_t sent = -1;
            for (int k = 0; sent < 0 && k < 100; k++) {
                int64_t flip = frame - lead + k * spacing;  // flip of frame k
                if (avf_osd_change_due(flip, frame, spacing, want))
                    sent = flip;
            }
            // Sent with at least the lead left (or at the frame's own flip,
            // when the lead asked for is longer than the queue lead) ...
            margin_ok &= sent >= 0 && (frame - sent >= want || sent == frame - lead);
            // ... and at the last such flip: the next would be too late.
            last_ok &= sent >= 0 && frame - (sent + spacing) < want;
            // Never before the frame's own flip.
            early_ok &= sent >= frame - lead;
        }
    }
    check(margin_ok, "a deferred change is sent with at least its lead left");
    check(last_ok, "a deferred change is sent at the last flip that leaves its lead");
    check(early_ok, "a deferred change is not sent before its frame is flipped");
    // A frame shown at once (a redraw, a seek while paused) is due at once.
    check(avf_osd_change_due(now, now, 41 * MS, 40 * MS), "an untimed frame's change is due at once");
    // The timer that looks again when no flip comes fires a frame spacing
    // and a slack later: late enough that a flip on time always comes first.
    check(AVF_OSD_PENDING_SLACK_NS > 0 && AVF_OSD_PENDING_SLACK_NS < 20 * MS,
          "the fallback timer's slack is a few milliseconds");

    if (failures)
        return 1;
    printf("avfoundation subtitle wait policy: all checks passed\n");
    return 0;
}
