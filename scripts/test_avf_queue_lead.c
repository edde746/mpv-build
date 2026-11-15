// Behavioral regressions for vo_avfoundation's queue lead, compiled verbatim
// from the final Apple series by test_avf_queue_lead.sh: on tvOS a frame must
// reach the video and OSD layers further ahead of its refresh than either
// layer's limit (2.9 to 3.3 refreshes; in between the subtitles showed a
// refresh before or after their picture, plezy#3073), also when the flip is
// late, and where queue-lead-ms already covers that it is kept, so a pause
// does not take longer to show.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "avf_queue_lead.inc"

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
    const int64_t option = 128 * MS; // queue-lead-ms's default

    // The refresh rates tvOS matches video to at or below 30 fps. The flip
    // that enqueues a frame comes some milliseconds after the lead asks for
    // it (118 ms of 128 measured); the frame must still be past the later of
    // the two layers' limits, 3.3 refreshes, by a margin.
    const int64_t low[] = {41708333, 41666667, 40000000, 33366667, 33333333};
    bool past_limits = true;
    for (size_t i = 0; i < sizeof(low) / sizeof(low[0]); i++) {
        int64_t lead = avf_queue_lead_ns(option, low[i]);
        past_limits = past_limits && (lead - 10 * MS) * 10 >= low[i] * 35;
    }
    check(past_limits, "at 24, 25 and 30 Hz a frame flipped 10 ms late is 3.5 refreshes ahead");

    // At 50 Hz and up queue-lead-ms is already past the limits: kept, so the
    // pictures queued when playback pauses do not grow.
    const int64_t high[] = {20000000, 16683333, 16666667, 8333333};
    bool kept = true;
    for (size_t i = 0; i < sizeof(high) / sizeof(high[0]); i++)
        kept = kept && avf_queue_lead_ns(option, high[i]) == option;
    check(kept, "at 50 Hz and up queue-lead-ms is kept");

    check(avf_queue_lead_ns(300 * MS, 41708333) == 300 * MS,
          "a queue-lead-ms longer than the refreshes ask for is kept");
    check(avf_queue_lead_ns(option, 0) == option,
          "with the refresh unknown queue-lead-ms is kept");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("avf queue lead: all checks passed\n");
    return EXIT_SUCCESS;
}
