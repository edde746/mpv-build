// Behavioral regression for vo_avfoundation's media-time stamp rule,
// compiled verbatim from the final Apple series by test_avf_media_stamp.sh:
// avf_media_stamp_offset must not let a frame the display layer already took
// before a reset (a new file, a seek back) be shown after a frame filed after
// it, and must leave every later frame's spacing as its timestamps have it.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "avf_media_stamp.inc"

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
    const int64_t frame = 41666667; // 24 fps

    // Ordinary playback from the start: no offset, every frame on its own
    // timestamp.
    int64_t offset = 0, last = INT64_MIN;
    bool kept = true;
    for (int i = 0; i < 48; i++) {
        int64_t pts = i * frame;
        offset = avf_media_stamp_offset(offset, last, pts);
        kept = kept && offset == 0;
        last = pts + offset;
    }
    check(kept, "steady playback keeps every frame on its own timestamp");

    // A file played 1.2 s, then another from 0 (loadfile replace): the old
    // frames up to the 128 ms lead keep their stamps. Every new stamp must
    // come after the last old one, in order, a frame apart as their
    // timestamps are.
    int64_t old_last = 1200 * MS + 128 * MS;
    last = old_last;
    offset = 0;
    int64_t prev = old_last;
    bool after_old = true, ordered = true, spaced = true;
    for (int i = 0; i < 48; i++) {
        int64_t pts = i * frame;
        offset = avf_media_stamp_offset(offset, last, pts);
        int64_t stamp = pts + offset;
        after_old = after_old && stamp > old_last;
        ordered = ordered && stamp > prev;
        if (i > 0)
            spaced = spaced && stamp - prev == frame;
        prev = stamp;
        last = stamp;
    }
    check(after_old, "no frame of the new file is stamped before one of the old");
    check(ordered, "the new file's stamps stay strictly increasing");
    check(spaced, "the new file's frames keep their own spacing");
    check(avf_media_stamp_offset(0, old_last, 0) == old_last + 1,
          "the new file's first frame comes just after the old file's last");

    // A next file starting at a resume point past the old stamps keeps its
    // timestamps: the rule delays nothing it does not have to.
    check(avf_media_stamp_offset(0, old_last, 70000 * MS) == 0,
          "a timestamp past the last stamp is untouched");

    // Once an offset is in place, a later file or seek that lands past the
    // last stamp keeps it; one that lands before it grows it.
    int64_t kept_offset = 5000 * MS;
    check(avf_media_stamp_offset(kept_offset, 9000 * MS, 6000 * MS) == kept_offset,
          "an offset already in place is kept while stamps go forward");
    int64_t grown = avf_media_stamp_offset(kept_offset, 9000 * MS, 1000 * MS);
    check(1000 * MS + grown == 9000 * MS + 1,
          "a seek back under an offset is stamped just after the last stamp");

    if (failures)
        return 1;
    printf("avf media stamp: all checks passed\n");
    return 0;
}
