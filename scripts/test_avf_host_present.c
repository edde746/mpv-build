// Behavioral regressions for vo_avfoundation's host-mode presentation stamp
// rule (avf_host_present_ns), compiled verbatim from the final Apple series by
// test_avf_host_present.sh: a seek must not let a frame queued before it be
// shown after one decoded after it (plezy#2823), and the rule must never hold
// the picture back past the queue lead.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "avf_host_present.inc"

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

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("avf host present stamp: all checks passed\n");
    return EXIT_SUCCESS;
}
