// Behavioral regression for pgssubdec's palette cache across a PGS epoch
// boundary (patch 0022), extracted by test_pgs_palette.sh.
//
// A palette segment is a delta only while its display set is Normal state; an
// acquisition point or an epoch start replaces the palette, so every entry the
// segment omits is transparent (libbluray's _decode_pds, clause 8.8.3.1.1).
// pgssubdec releases the epoch through flush_cache(), which drops
// palettes.count without touching PGSSubPalette.clut -- so before the patch a
// re-issued slot still held the previous epoch's colours, and a caption whose
// bitmap background index was not carried by the new palette rendered in
// whatever an earlier caption left there: an opaque box behind the subtitles,
// in a shade that changed from caption to caption (edde746/plezy#2391).
//
// What must hold: a palette slot handed out after a flush starts fully
// transparent, while a delta update inside one epoch still inherits the
// entries it omits.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- environment the two extracted functions compile against.

#define MAX_NEG_CROP 1024
static uint8_t crop_table[256 + 2 * MAX_NEG_CROP];
#define ff_crop_tab crop_table

#define AVERROR_INVALIDDATA (-0x3ebbb1b7)
#define AV_LOG_ERROR 16
typedef struct AVClass AVClass;

typedef struct AVCodecContext {
    void *priv_data;
    int height;
} AVCodecContext;

static int logged_errors;

static void av_log(void *avcl, int level, const char *fmt, ...)
{
    (void)avcl; (void)fmt;
    if (level <= AV_LOG_ERROR)
        logged_errors++;
}

#define ff_dlog(ctx, ...) do { (void)(ctx); } while (0)

static void av_freep(void *arg)
{
    void **ptr = arg;
    free(*ptr);
    *ptr = NULL;
}

static unsigned bytestream_get_byte(const uint8_t **b)
{
    return *(*b)++;
}

#include "pgs_palette.inc"

// --- test driver

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void check_entry(uint32_t got, uint32_t want, const char *what)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s: clut entry %08x, want %08x\n", what, got, want);
        failures++;
    }
}

// One palette segment: id, version, then (index, Y, Cr, Cb, alpha) entries.
static int feed_palette(AVCodecContext *avctx, int id, int version,
                        const uint8_t *entries, int entry_count)
{
    uint8_t buf[2 + 5 * 8];
    if (entry_count > 8)
        abort();
    buf[0] = id;
    buf[1] = version;
    memcpy(buf + 2, entries, (size_t)entry_count * 5);
    return parse_palette_segment(avctx, buf, 2 + entry_count * 5);
}

static uint32_t entry_of(PGSSubContext *ctx, int palette_id, int index)
{
    PGSSubPalette *palette = find_palette(palette_id, &ctx->palettes);
    if (!palette) {
        fprintf(stderr, "FAIL: palette %d is gone\n", palette_id);
        failures++;
        return 0;
    }
    return palette->clut[index];
}

int main(void)
{
    for (int i = 0; i < 256 + 2 * MAX_NEG_CROP; i++) {
        int value = i - MAX_NEG_CROP;
        crop_table[i] = value < 0 ? 0 : value > 255 ? 255 : value;
    }

    PGSSubContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    AVCodecContext avctx = { .priv_data = &ctx, .height = 1080 };

    // Epoch 1: index 0 is an opaque mid-grey this caption paints with, index 1
    // is the opaque white of its text.
    const uint8_t epoch1[] = {
        0, 128, 128, 128, 255,
        1, 235, 128, 128, 255,
    };
    check(feed_palette(&avctx, 0, 0, epoch1, 2) == 0, "epoch 1 palette parses");
    uint32_t grey = entry_of(&ctx, 0, 0);
    check((grey >> 24) == 255, "epoch 1 index 0 is opaque");
    check((entry_of(&ctx, 0, 1) >> 24) == 255, "epoch 1 index 1 is opaque");

    // Same epoch, a delta that carries only index 2: the entries it omits must
    // survive, which is what makes a palette update an update.
    const uint8_t delta[] = { 2, 16, 128, 128, 255 };
    check(feed_palette(&avctx, 0, 1, delta, 1) == 0, "in-epoch update parses");
    check_entry(entry_of(&ctx, 0, 0), grey, "in-epoch update keeps an omitted entry");
    check((entry_of(&ctx, 0, 2) >> 24) == 255, "in-epoch update applies its own entry");
    check(ctx.palettes.count == 1, "an update does not allocate a second palette");

    // Epoch boundary: the decoder releases the cache, and the next epoch's
    // palette carries index 1 and 2 only. Index 0 is undefined there, so it is
    // transparent -- not the grey epoch 1 left in the slot.
    flush_cache(&avctx);
    check(ctx.palettes.count == 0, "flush releases the palette cache");
    const uint8_t epoch2[] = {
        1, 235, 128, 128, 255,
        2,  16, 128, 128, 255,
    };
    check(feed_palette(&avctx, 0, 0, epoch2, 2) == 0, "epoch 2 palette parses");
    check_entry(entry_of(&ctx, 0, 0), 0u, "an entry the new epoch omits is transparent");
    check((entry_of(&ctx, 0, 1) >> 24) == 255, "epoch 2 index 1 is opaque");

    // A second palette id in the same epoch starts clean too: slot reuse is
    // per slot, so the id that lands in slot 1 must not inherit either.
    flush_cache(&avctx);
    check(feed_palette(&avctx, 0, 0, epoch2, 2) == 0, "palette 0 parses");
    check(feed_palette(&avctx, 7, 0, delta, 1) == 0, "palette 7 parses");
    check_entry(entry_of(&ctx, 7, 0), 0u, "a fresh palette id starts transparent");
    check_entry(entry_of(&ctx, 7, 1), 0u, "a fresh palette id inherits no neighbour");

    check(logged_errors == 0, "no decoder errors were logged");

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: pgs palette cache\n");
    return 0;
}
