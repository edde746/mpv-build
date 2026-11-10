// Behavioral regressions for vo_avfoundation's per-buffer OSD damage
// (avf_sig_add, avf_damage_apply), compiled verbatim from the final Apple
// series by test_avf_osd_damage.sh. An overlay buffer comes back from the
// pool with what was last drawn into it, and a frame drawn into it touches
// only the tiles whose signature changed: whatever it leaves alone, copies
// or redraws, the buffer must end up exactly as a full clear and a full
// blend of the frame leave it. A pixel that differs is a stale or a missing
// piece of subtitle on screen.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MPMIN(a, b) ((a) > (b) ? (b) : (a))
#define MPMAX(a, b) ((a) > (b) ? (a) : (b))
#define MPCLAMP(a, min, max) (((a) < (min)) ? (min) : (((a) > (max)) ? (max) : (a)))
struct mp_rect { int x0, y0, x1, y1; };

#include "avf_osd_damage.inc"

static int failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

// A model of the VO: parts with a place, an identity and a sparse,
// order-sensitive "blend"; buffers of a pool with their signatures.
struct part {
    int x, y, w, h;
    uint64_t id;        // what its pixels are
    uint32_t value;
};

#define MAXW 16400
#define MAXPX (640 * 400)
#define BUFS 6

struct model {
    int w, h, tiles;
    struct part parts[64];
    int num_parts;
    uint32_t *target;               // the buffer being drawn
    const uint32_t *prev;           // the previous canvas
    int64_t cleared, copied, blended;
    int clears;                     // clear operations
};

static uint32_t blend_px(uint32_t dst, const struct part *p, int x, int y)
{
    // Sparse like a glyph mask, and dependent on what is below.
    if (((x - p->x) * 7 + (y - p->y) * 13 + (int)(p->id % 5)) % 3 == 0)
        return dst;
    return dst * 1000003u + p->value + (uint32_t)p->id;
}

static void blend_parts(struct model *m, uint32_t *canvas, struct mp_rect r)
{
    for (int n = 0; n < m->num_parts; n++) {
        const struct part *p = &m->parts[n];
        int x0 = MPMAX(p->x, r.x0), x1 = MPMIN(p->x + p->w, r.x1);
        int y0 = MPMAX(p->y, r.y0), y1 = MPMIN(p->y + p->h, r.y1);
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++)
                canvas[y * m->w + x] = blend_px(canvas[y * m->w + x], p, x, y);
        }
        if (x1 > x0 && y1 > y0)
            m->blended += (int64_t)(x1 - x0) * (y1 - y0);
    }
}

static void op_clear(void *ctx, struct mp_rect r)
{
    struct model *m = ctx;
    check(r.x0 >= 0 && r.y0 >= 0 && r.x1 <= m->w && r.y1 <= m->h && r.x1 > r.x0 && r.y1 > r.y0,
          "clear stays inside the canvas");
    for (int y = r.y0; y < r.y1; y++)
        memset(&m->target[y * m->w + r.x0], 0, (size_t)(r.x1 - r.x0) * 4);
    m->cleared += (int64_t)(r.x1 - r.x0) * (r.y1 - r.y0);
    m->clears++;
}

static void op_copy(void *ctx, struct mp_rect r)
{
    struct model *m = ctx;
    check(m->prev != NULL, "copy only with a previous canvas");
    check(r.x0 >= 0 && r.y0 >= 0 && r.x1 <= m->w && r.y1 <= m->h, "copy stays inside the canvas");
    for (int y = r.y0; y < r.y1 && m->prev; y++)
        memcpy(&m->target[y * m->w + r.x0], &m->prev[y * m->w + r.x0], (size_t)(r.x1 - r.x0) * 4);
    m->copied += (int64_t)(r.x1 - r.x0) * (r.y1 - r.y0);
}

static void op_draw(void *ctx, struct mp_rect r)
{
    struct model *m = ctx;
    check(r.x0 >= 0 && r.y0 >= 0 && r.x1 <= m->w && r.y1 <= m->h, "draw stays inside the canvas");
    blend_parts(m, m->target, r);
}

static void signatures(const struct model *m, uint64_t *sig)
{
    memset(sig, 0, sizeof(sig[0]) * m->tiles);
    for (int n = 0; n < m->num_parts; n++) {
        const struct part *p = &m->parts[n];
        struct mp_rect r = {
            MPCLAMP(p->x, 0, m->w), MPCLAMP(p->y, 0, m->h),
            MPCLAMP(p->x + p->w, 0, m->w), MPCLAMP(p->y + p->h, 0, m->h),
        };
        if (r.x1 <= r.x0 || r.y1 <= r.y0)
            continue;
        uint64_t key = avf_sig_mix(1, p->id);
        key = avf_sig_mix(key, ((uint64_t)(uint32_t)p->x << 32) | (uint32_t)p->y);
        key = avf_sig_mix(key, ((uint64_t)(uint32_t)p->w << 32) | (uint32_t)p->h);
        key = avf_sig_mix(key, p->value);
        avf_sig_add(sig, avf_tiles(m->w), r, key);
    }
}

static uint32_t seed = 99;
#define RND() (seed = seed * 1664525u + 1013904223u, seed >> 8)

// Frames of parts that stay, move, change and come and go, drawn into the
// buffers of a pool in rotation; each buffer is compared with a full redraw.
static void run_pool(int w, int h, int frames, const char *what)
{
    struct model m = {.w = w, .h = h, .tiles = avf_tiles(w) * avf_tiles(h)};
    size_t px = (size_t)w * h;
    uint32_t *buf[BUFS], *ref = malloc(px * 4);
    uint64_t *sig[BUFS], *want = malloc(sizeof(uint64_t) * m.tiles),
             *prev_sig = malloc(sizeof(uint64_t) * m.tiles);
    bool known[BUFS] = {0};
    for (int i = 0; i < BUFS; i++) {
        buf[i] = malloc(px * 4);
        sig[i] = malloc(sizeof(uint64_t) * m.tiles);
        for (size_t k = 0; k < px; k++)
            buf[i][k] = RND();      // a new buffer holds anything
    }
    int prev = -1, next_buf = 0, depth = 1 + RND() % BUFS;
    uint64_t next_id = 10;
    bool same = true;
    int64_t touched = 0, total = 0;

    for (int f = 0; f < frames && same; f++) {
        // Evolve the frame.
        if (f % 97 == 96) {
            m.num_parts = 0;                    // a cut: everything goes
        }
        for (int n = 0; n < m.num_parts; n++) {
            struct part *p = &m.parts[n];
            int kind = (int)(p->value % 4);     // 0, 1: static sign
            if (kind == 2) {                    // moves
                p->x += (int)(RND() % 7) - 3;
                p->y += (int)(RND() % 5) - 2;
            } else if (kind == 3) {             // new pixels every frame
                p->id = next_id++;
            }
            if (RND() % 60 == 0)
                m.parts[n--] = m.parts[--m.num_parts];
        }
        while (m.num_parts < 64 && (m.num_parts < 3 || RND() % 5 == 0)) {
            struct part *p = &m.parts[m.num_parts++];
            p->x = (int)(RND() % (w + 60)) - 30;
            p->y = (int)(RND() % (h + 60)) - 30;
            p->w = 1 + RND() % (w / 2 + 1);
            p->h = 1 + RND() % (h / 3 + 1);
            p->id = next_id++;
            p->value = RND();
        }
        if (f % 31 == 30)
            m.num_parts = 0;                    // an empty canvas in between

        // The pool hands out the next buffer of the rotation; now and then
        // its entry was evicted (contents unknown), or the rotation changes.
        if (RND() % 50 == 0)
            depth = 1 + RND() % BUFS;
        next_buf = (next_buf + 1) % depth;
        if (next_buf == prev)
            next_buf = (next_buf + 1) % BUFS;
        int b = next_buf;
        if (RND() % 40 == 0)
            known[b] = false;

        signatures(&m, want);
        m.target = buf[b];
        m.prev = prev >= 0 && RND() % 8 ? buf[prev] : NULL;
        m.cleared = m.copied = m.blended = 0;
        struct avf_damage_ops ops = {&m, op_clear, op_copy, op_draw};
        struct avf_damage_px dpx = {0};
        avf_damage_apply(known[b] ? sig[b] : NULL, m.prev ? prev_sig : NULL,
                         m.num_parts ? want : NULL, w, h, &ops, &dpx);
        memcpy(sig[b], want, sizeof(want[0]) * m.tiles);
        known[b] = true;

        memset(ref, 0, px * 4);
        blend_parts(&m, ref, (struct mp_rect){0, 0, w, h});
        same = !memcmp(ref, buf[b], px * 4);
        if (!same)
            fprintf(stderr, "frame %d differs (%d parts, buffer %d)\n", f, m.num_parts, b);
        touched += dpx.cleared + dpx.copied + dpx.drawn;
        total += (int64_t)px;

        // The canvas the next frame may copy from: only one with parts.
        if (m.num_parts) {
            prev = b;
            memcpy(prev_sig, want, sizeof(want[0]) * m.tiles);
        } else {
            prev = -1;
        }
    }
    check(same, what);
    check(touched < total, "damage touches less than a full clear and blend of every frame");
    for (int i = 0; i < BUFS; i++) {
        free(buf[i]);
        free(sig[i]);
    }
    free(ref);
    free(want);
    free(prev_sig);
}

int main(void)
{
    // Tiles and signatures: a part marks the tiles it reaches, and no others.
    enum { W = 301, H = 203 };  // 10 x 7 tiles, the last column and row cut
    int tx = avf_tiles(W), ty = avf_tiles(H);
    check(tx == 10 && ty == 7, "a cut tile counts");
    uint64_t sig[70] = {0};
    avf_sig_add(sig, tx, (struct mp_rect){40, 70, 100, 96}, 5);
    bool marks = true;
    for (int y = 0; y < ty; y++) {
        for (int x = 0; x < tx; x++)
            marks &= (sig[y * tx + x] != 0) == (x >= 1 && x <= 3 && y == 2);
    }
    check(marks, "a part marks exactly the tiles it reaches");
    uint64_t one = sig[2 * tx + 1];
    avf_sig_add(sig, tx, (struct mp_rect){40, 70, 100, 96}, 6);
    uint64_t ab = sig[2 * tx + 1];
    uint64_t rev[70] = {0};
    avf_sig_add(rev, tx, (struct mp_rect){40, 70, 100, 96}, 6);
    avf_sig_add(rev, tx, (struct mp_rect){40, 70, 100, 96}, 5);
    check(ab != one && ab != rev[2 * tx + 1], "the order parts are drawn in is part of a signature");

    // A buffer that holds the frame is left alone entirely.
    struct model m = {.w = W, .h = H, .tiles = tx * ty};
    m.parts[m.num_parts++] = (struct part){20, 30, 120, 40, 1, 7};
    m.parts[m.num_parts++] = (struct part){25, 150, 250, 40, 2, 8};
    static uint32_t a[W * H], b[W * H], ref[W * H];
    uint64_t want[70], have[70];
    signatures(&m, want);
    struct avf_damage_ops ops = {&m, op_clear, op_copy, op_draw};
    struct avf_damage_px px = {0};
    for (int i = 0; i < W * H; i++)
        a[i] = 0xdeadbeef;
    m.target = a;
    avf_damage_apply(NULL, NULL, want, W, H, &ops, &px);
    blend_parts(&m, ref, (struct mp_rect){0, 0, W, H});
    check(!memcmp(a, ref, sizeof(a)), "a buffer of unknown contents ends up as a full redraw");
    check(px.cleared == (int64_t)W * H, "a buffer of unknown contents is cleared whole");
    // In one piece: on the device one 13.8 MB clear runs three times as fast
    // per byte as the same canvas cleared row by row around the subtitles,
    // which is what every change of plain dialogue pays (its buffers are new).
    check(m.clears == 1 && m.cleared == (int64_t)W * H, "a buffer of unknown contents is cleared in one piece");
    memcpy(have, want, sizeof(want));
    px = (struct avf_damage_px){0};
    m.cleared = m.copied = m.blended = 0;
    avf_damage_apply(have, NULL, want, W, H, &ops, &px);
    check(!px.cleared && !px.copied && !px.drawn && !m.blended, "an unchanged frame touches nothing");

    // Only the line changes: the sign above it is neither cleared nor
    // blended, and the line's tiles are all that is.
    m.parts[1].id = 3;
    signatures(&m, want);
    px = (struct avf_damage_px){0};
    avf_damage_apply(have, NULL, want, W, H, &ops, &px);
    memset(ref, 0, sizeof(ref));
    blend_parts(&m, ref, (struct mp_rect){0, 0, W, H});
    check(!memcmp(a, ref, sizeof(a)), "a changed line is redrawn");
    // The line spans x 25-275, y 150-190: tiles 0-8 x 4-5.
    check(px.drawn == 9 * 32 * 2 * 32 && px.cleared == px.drawn && !px.copied,
          "only the tiles under the changed line are cleared and blended");
    memcpy(have, want, sizeof(want));

    // The same frame into another buffer that still holds the frame before
    // the change: the line comes from the previous canvas, nothing is blended.
    for (int i = 0; i < W * H; i++)
        b[i] = 0x12345678;
    m.parts[1].id = 2;
    uint64_t old[70];
    signatures(&m, old);
    m.target = b;
    px = (struct avf_damage_px){0};
    avf_damage_apply(NULL, NULL, old, W, H, &ops, &px);
    m.parts[1].id = 3;
    m.prev = a;
    m.blended = 0;
    px = (struct avf_damage_px){0};
    avf_damage_apply(old, have, want, W, H, &ops, &px);
    check(!memcmp(b, ref, sizeof(b)), "tiles copied from the previous canvas match a redraw");
    check(px.copied == 9 * 32 * 2 * 32 && !px.drawn && !px.cleared && !m.blended,
          "what the previous canvas holds is copied, not blended");

    // Nothing wanted: what the buffer holds is cleared, and only that.
    px = (struct avf_damage_px){0};
    m.target = a;
    m.prev = NULL;
    avf_damage_apply(have, NULL, NULL, W, H, &ops, &px);
    bool empty = true;
    for (int i = 0; i < W * H; i++)
        empty &= a[i] == 0;
    check(empty, "an empty frame clears the buffer");
    int covered = 0;
    for (int i = 0; i < tx * ty; i++)
        covered += have[i] != 0;
    check(px.cleared > 0 && px.cleared <= (int64_t)covered * 32 * 32 && !px.drawn,
          "an empty frame clears only the tiles that held something");

    // Pools of every depth over sizes with whole and cut tiles.
    run_pool(301, 203, 3000, "a rotating pool always matches a full redraw (301x203)");
    run_pool(640, 384, 1500, "a rotating pool always matches a full redraw (640x384)");
    run_pool(33, 31, 1500, "a rotating pool always matches a full redraw (33x31)");

    // A canvas too wide for the row buffers is redrawn whole.
    {
        struct model wm = {.w = 512 * 32 + 5, .h = 40};
        wm.tiles = avf_tiles(wm.w) * avf_tiles(wm.h);
        wm.parts[wm.num_parts++] = (struct part){100, 3, 16000, 30, 4, 9};
        size_t n = (size_t)wm.w * wm.h;
        uint32_t *wide = malloc(n * 4), *wref = calloc(n, 4);
        uint64_t *ws = malloc(sizeof(uint64_t) * wm.tiles);
        for (size_t i = 0; i < n; i++)
            wide[i] = 0xabcdef01;
        signatures(&wm, ws);
        wm.target = wide;
        struct avf_damage_ops wops = {&wm, op_clear, op_copy, op_draw};
        px = (struct avf_damage_px){0};
        avf_damage_apply(ws, NULL, ws, wm.w, wm.h, &wops, &px);
        blend_parts(&wm, wref, (struct mp_rect){0, 0, wm.w, wm.h});
        check(!memcmp(wide, wref, n * 4), "a canvas wider than the tile rows is redrawn whole");
        free(wide);
        free(wref);
        free(ws);
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("avf osd damage: ok\n");
    return 0;
}
