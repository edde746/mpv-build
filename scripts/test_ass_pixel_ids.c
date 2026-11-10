// Behavioral regressions for the ids mp_ass_packer gives libass bitmaps
// (assign_pixels_ids), compiled verbatim from the final Apple series by
// test_ass_pixel_ids.sh. A consumer keeps what it drew of a part while the
// part's id repeats, so two parts with one id must be the same bitmap of a
// list that was held all along: an id carried over anything else is a stale
// subtitle on screen.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sub_bitmap {
    void *bitmap;
    int stride;
    int w, h;
    struct {
        uint32_t color;
        uint64_t pixels_id;
    } libass;
};

#define MP_TARRAY_GROW(ctx, p, nextidx) \
    ((void)(ctx), (p) = realloc((p), sizeof((p)[0]) * ((size_t)(nextidx) + 1)))
#define MPSWAP(type, a, b) \
    do { type SWAP_tmp = b; b = a; a = SWAP_tmp; } while (0)

#include "ass_pixel_ids.inc"

static int failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

#define MAXP 4096
static unsigned char pool[MAXP * 16];   // "bitmaps": distinct addresses

static struct sub_bitmap part(int bitmap, int w, int h)
{
    return (struct sub_bitmap){.bitmap = &pool[bitmap * 16], .stride = w, .w = w, .h = h};
}

static bool seen_before(const uint64_t *old, int n, uint64_t id)
{
    for (int i = 0; i < n; i++) {
        if (old[i] == id)
            return true;
    }
    return false;
}

int main(void)
{
    struct pixel_ids ids = {0};
    struct sub_bitmap p[MAXP];

    // Nothing held: nothing is known.
    p[0] = part(1, 10, 10);
    p[1] = part(2, 10, 10);
    p[0].libass.pixels_id = p[1].libass.pixels_id = 77;
    assign_pixels_ids(NULL, &ids, p, 2, false);
    check(!p[0].libass.pixels_id && !p[1].libass.pixels_id, "no held list: ids are 0");

    // A first held list: every bitmap a nonzero id of its own. (A bitmap
    // drawn twice may have two: equal ids mean equal pixels, not the reverse.)
    p[0] = part(1, 10, 10);
    p[1] = part(2, 10, 10);
    p[2] = part(1, 10, 10);
    assign_pixels_ids(NULL, &ids, p, 3, true);
    uint64_t a = p[0].libass.pixels_id, b = p[1].libass.pixels_id, a2 = p[2].libass.pixels_id;
    check(a && b && a != b, "distinct bitmaps get distinct nonzero ids");
    check(a2 && a2 != b, "a bitmap drawn twice never shares an id with another bitmap");

    // The same list again, and with a part inserted in front: ids follow the
    // bitmaps, the new one gets an id nobody had.
    assign_pixels_ids(NULL, &ids, p, 3, true);
    check(p[0].libass.pixels_id == a && p[1].libass.pixels_id == b &&
          p[2].libass.pixels_id == a2, "an unchanged list keeps its ids, part by part");
    p[0] = part(3, 10, 10);
    p[1] = part(1, 10, 10);
    p[2] = part(2, 10, 10);
    assign_pixels_ids(NULL, &ids, p, 3, true);
    uint64_t c = p[0].libass.pixels_id;
    check((p[1].libass.pixels_id == a || p[1].libass.pixels_id == a2) && p[2].libass.pixels_id == b,
          "ids survive a part inserted before them");
    check(c && c != a && c != a2 && c != b, "the inserted bitmap gets a new id");

    // Reordered and thinned out.
    p[0] = part(2, 10, 10);
    p[1] = part(3, 10, 10);
    assign_pixels_ids(NULL, &ids, p, 2, true);
    check(p[0].libass.pixels_id == b && p[1].libass.pixels_id == c, "ids survive reordering and removal");

    // A bitmap that left the list comes back with a new id: its memory may
    // have been freed and reused in between.
    p[0] = part(2, 10, 10);
    p[1] = part(1, 10, 10);
    assign_pixels_ids(NULL, &ids, p, 2, true);
    check(p[0].libass.pixels_id == b, "a bitmap that stayed keeps its id");
    check(p[1].libass.pixels_id != a && p[1].libass.pixels_id != a2 && p[1].libass.pixels_id != c &&
          p[1].libass.pixels_id, "a bitmap that was gone for a list gets a new id");

    // The same address with another size is another bitmap (a clipped view).
    uint64_t d = p[1].libass.pixels_id;
    p[0] = part(1, 10, 8);
    p[1] = part(1, 10, 10);
    assign_pixels_ids(NULL, &ids, p, 2, true);
    check(p[1].libass.pixels_id == d, "the same view keeps its id");
    check(p[0].libass.pixels_id != d && p[0].libass.pixels_id, "another view of the address gets its own id");

    // An unheld pack in between forgets everything.
    uint64_t e = p[0].libass.pixels_id;
    assign_pixels_ids(NULL, &ids, p, 2, false);
    check(!p[0].libass.pixels_id && !p[1].libass.pixels_id, "an unheld pack gives 0");
    assign_pixels_ids(NULL, &ids, p, 2, true);
    check(p[0].libass.pixels_id != e && p[1].libass.pixels_id != d &&
          p[0].libass.pixels_id && p[1].libass.pixels_id, "ids do not survive an unheld pack");

    // Random lists, each made from the one before by dropping, adding,
    // duplicating and shuffling bitmaps: against a map of the previous list.
    static int cur[MAXP], prev[MAXP];
    static uint64_t prev_id[MAXP], all_ids[MAXP * 64];
    int ncur = 0, nprev = 0, nall = 0;
    bool keep_ok = true, new_ok = true, zero_ok = true;
    uint32_t seed = 12345;
#define RND() (seed = seed * 1664525u + 1013904223u, seed >> 8)
    assign_pixels_ids(NULL, &ids, p, 0, false);
    for (int round = 0; round < 400; round++) {
        ncur = 0;
        for (int i = 0; i < nprev; i++) {
            if (RND() % 100 < 85)
                cur[ncur++] = prev[i];
        }
        int add = RND() % 40;
        for (int i = 0; i < add && ncur < 2000; i++) {
            int at = ncur ? RND() % (ncur + 1) : 0;
            memmove(&cur[at + 1], &cur[at], sizeof(cur[0]) * (ncur - at));
            cur[at] = RND() % MAXP;
            ncur++;
        }
        if (round % 7 == 3) {
            for (int i = ncur - 1; i > 0; i--) {
                int j = RND() % (i + 1), t = cur[i];
                cur[i] = cur[j];
                cur[j] = t;
            }
        }
        for (int i = 0; i < ncur; i++)
            p[i] = part(cur[i], 16, 16);
        assign_pixels_ids(NULL, &ids, p, ncur, true);
        for (int i = 0; i < ncur; i++) {
            uint64_t id = p[i].libass.pixels_id;
            zero_ok &= id != 0;
            // In the previous list: one of the ids it had there. New to
            // this list: an id no list ever had.
            bool was = false, same = false;
            for (int k = 0; k < nprev; k++) {
                if (prev[k] == cur[i]) {
                    was = true;
                    same |= id == prev_id[k];
                }
            }
            if (was) {
                keep_ok &= same;
            } else {
                new_ok &= !seen_before(all_ids, nall, id);
            }
            // Equal ids within the list: the same bitmap.
            for (int k = 0; k < i; k++)
                keep_ok &= p[k].libass.pixels_id != id || cur[k] == cur[i];
        }
        for (int i = 0; i < ncur; i++) {
            prev[i] = cur[i];
            prev_id[i] = p[i].libass.pixels_id;
            if (nall < MAXP * 64 && !seen_before(all_ids, nall, prev_id[i]))
                all_ids[nall++] = prev_id[i];
        }
        nprev = ncur;
    }
    check(zero_ok, "random lists: no id is 0 while held");
    check(keep_ok, "random lists: a bitmap of the previous list keeps its id");
    check(new_ok, "random lists: a bitmap new to the list gets an id never used");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_ass_pixel_ids: ok\n");
    return 0;
}
