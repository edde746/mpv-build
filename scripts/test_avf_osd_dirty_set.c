// Behavioral regressions for vo_avfoundation's per-buffer OSD dirty set
// (avf_dirty_add, avf_dirty_pixels), compiled verbatim from the final Apple
// series by test_avf_osd_dirty_set.sh. An overlay buffer is cleared from the
// set recorded when it was last drawn into, so the set must cover every
// recorded rect: a pixel it misses is a stale subtitle fragment on screen.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MPMIN(a, b) ((a) > (b) ? (b) : (a))
#define MPMAX(a, b) ((a) > (b) ? (a) : (b))
#define MPCLAMP(a, min, max) (((a) < (min)) ? (min) : (((a) > (max)) ? (max) : (a)))
struct mp_rect { int x0, y0, x1, y1; };

#include "avf_osd_dirty_set.inc"

#define W 2736
#define H 1260

static int failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static uint8_t cover[H][W];

static void mark(struct mp_rect r, uint8_t bit)
{
    for (int y = MPMAX(r.y0, 0); y < MPMIN(r.y1, H); y++)
        for (int x = MPMAX(r.x0, 0); x < MPMIN(r.x1, W); x++)
            cover[y][x] |= bit;
}

int main(void)
{
    // A sign at the top and dialogue at the bottom stay two rects: their
    // bounding box would clear the whole picture between them.
    struct avf_dirty_set s = {0};
    avf_dirty_add(&s, (struct mp_rect){400, 40, 1800, 160});
    avf_dirty_add(&s, (struct mp_rect){300, 1100, 2400, 1220});
    check(s.num == 2, "far-apart rects stay separate");
    check(avf_dirty_pixels(&s, W, H) == 1400 * 120 + 2100 * 120,
          "separate rects clear only themselves");

    // The fill, border and shadow parts of one line overlap almost
    // completely: one box clears them once.
    s = (struct avf_dirty_set){0};
    avf_dirty_add(&s, (struct mp_rect){500, 1100, 1500, 1180});
    avf_dirty_add(&s, (struct mp_rect){497, 1097, 1503, 1183});
    avf_dirty_add(&s, (struct mp_rect){499, 1099, 1505, 1185});
    check(s.num == 1, "overlapping parts of one line merge");
    check(s.r[0].x0 == 497 && s.r[0].y0 == 1097 && s.r[0].x1 == 1505 &&
          s.r[0].y1 == 1185, "the merged box is their bounds");

    // Unknown contents stay unknown until the buffer is cleared.
    s = (struct avf_dirty_set){.num = -1};
    avf_dirty_add(&s, (struct mp_rect){0, 0, 10, 10});
    check(s.num == -1 && avf_dirty_pixels(&s, W, H) == (int64_t)W * H,
          "an unknown buffer is cleared whole");

    // Empty rects are not recorded.
    s = (struct avf_dirty_set){0};
    avf_dirty_add(&s, (struct mp_rect){10, 10, 10, 50});
    avf_dirty_add(&s, (struct mp_rect){10, 10, 50, 5});
    check(s.num == 0, "empty rects are dropped");

    // Rects past the canvas are clamped when counted.
    s = (struct avf_dirty_set){0};
    avf_dirty_add(&s, (struct mp_rect){W - 10, H - 10, W + 50, H + 50});
    check(avf_dirty_pixels(&s, W, H) == 100, "pixels are counted on the canvas");

    // Randomized: subtitle-like parts in three bands. Whatever the merging
    // and the capacity do, every recorded pixel stays covered.
    srand(1);
    for (int it = 0; it < 400 && !failures; it++) {
        s = (struct avf_dirty_set){0};
        memset(cover, 0, sizeof(cover));
        int n = 1 + rand() % 120;
        for (int i = 0; i < n; i++) {
            int x0 = rand() % (W - 20), y0 = (rand() % 3) * 420 + rand() % 300;
            struct mp_rect r = {x0, y0, MPMIN(W, x0 + 1 + rand() % 700),
                                MPMIN(H, y0 + 1 + rand() % 140)};
            avf_dirty_add(&s, r);
            mark(r, 1);
        }
        check(s.num >= 1 && s.num <= AVF_OSD_DIRTY_MAX, "the set stays within capacity");
        for (int i = 0; i < s.num; i++)
            mark(s.r[i], 2);
        bool covered = true;
        for (int y = 0; y < H && covered; y++)
            for (int x = 0; x < W; x++)
                if (cover[y][x] == 1) {
                    covered = false;
                    break;
                }
        check(covered, "every drawn pixel is in the set");
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("avf osd dirty set: ok\n");
    return 0;
}
