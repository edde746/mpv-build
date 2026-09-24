// Regression test for the Metal OSD rasterizer of vo_avfoundation
// (patches/mpv/pool/0031-avfoundation-metal-osd-rasterizer.patch).
//
// Compiled against the VERBATIM rasterizer core and the VERBATIM CPU
// composition the VO falls back to (see test_avf_metal_osd.sh). The CPU path is
// the reference: it is what shipped before, and it is what the VO still draws
// with when Metal is unavailable, so the two must agree.
//
// What is asserted:
//   - a render replaces the whole target: a recycled pool buffer's previous
//     contents never survive, for clear-only and part renders alike;
//   - libass coverage parts match the CPU composition within one step per
//     channel: color, the inverted-alpha convention, premultiplication,
//     overlap order and parts clipped by the target edges;
//   - unscaled premultiplied BGRA parts match the CPU composition;
//   - scaled BGRA parts (bitmap subtitles, dw/dh != w/h) cover exactly their
//     destination rectangle, with the source color, and nothing else;
//   - more parts than fit one atlas shelf, and growing the atlas and part buffer
//     between renders, keep all of the above.

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define MPMAX(a, b) ((a) > (b) ? (a) : (b))
#define MPMIN(a, b) ((a) > (b) ? (b) : (a))
#define MPCLAMP(a, min, max) (((a) > (max)) ? (max) : (((a) < (min)) ? (min) : (a)))

// Stand-ins for the mpv types the extracted code and the harness's mpv-like
// atlas use. Field names and meanings match sub/osd.h and video/mp_image.h.
enum { IMGFMT_Y8 = 1, IMGFMT_BGRA = 2 };
enum sub_bitmap_format { SUBBITMAP_EMPTY = 0, SUBBITMAP_LIBASS, SUBBITMAP_BGRA };

struct mp_image {
    int imgfmt;
    uint8_t *planes[4];
    int stride[4];
};

struct sub_bitmap {
    void *bitmap;
    int stride;
    int w, h;
    int x, y;
    int dw, dh;
    int src_x, src_y;
    struct {
        uint32_t color;
    } libass;
};

struct sub_bitmaps {
    enum sub_bitmap_format format;
    struct sub_bitmap *parts;
    int num_parts;
    struct mp_image *packed;
    int packed_w, packed_h;
};

#include "metal_osd_core.inc"
#include "cpu_osd_blend.inc"

static int failures;

static uint32_t rng_state = 0x2545f491;

static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int rnd_range(int lo, int hi) // [lo, hi)
{
    return lo + (int)(rnd() % (uint32_t)(hi - lo));
}

// ---- atlas: what mpv's packers hand the VO ---------------------------------

// Every part gets its own cell with one pixel of padding on each side, filled
// by edge replication, as sub/ass_mp.c (fill_padding_*) and sd_lavc do so that
// bilinear sampling never reads a neighbor.
struct atlas {
    struct mp_image img;
    int w, h, bpp;
    int pen_x, pen_y, row_h;
    int used_w, used_h;
};

static void atlas_init(struct atlas *a, int w, int h, int imgfmt)
{
    *a = (struct atlas){.w = w, .h = h, .bpp = imgfmt == IMGFMT_BGRA ? 4 : 1};
    a->img.imgfmt = imgfmt;
    a->img.stride[0] = w * a->bpp + 16; // not tightly packed, as mp_image isn't
    a->img.planes[0] = calloc((size_t)a->img.stride[0] * h, 1);
}

static void atlas_free(struct atlas *a)
{
    free(a->img.planes[0]);
}

static uint8_t *atlas_px(struct atlas *a, int x, int y)
{
    return a->img.planes[0] + (size_t)y * a->img.stride[0] + (size_t)x * a->bpp;
}

static bool atlas_place(struct atlas *a, int w, int h, int *x, int *y)
{
    if (a->pen_x + w + 2 > a->w) {
        a->pen_x = 0;
        a->pen_y += a->row_h;
        a->row_h = 0;
    }
    if (w + 2 > a->w || a->pen_y + h + 2 > a->h)
        return false;
    *x = a->pen_x + 1;
    *y = a->pen_y + 1;
    a->pen_x += w + 2;
    a->row_h = MPMAX(a->row_h, h + 2);
    a->used_w = MPMAX(a->used_w, a->pen_x);
    a->used_h = MPMAX(a->used_h, a->pen_y + a->row_h);
    return true;
}

static void atlas_pad(struct atlas *a, int x, int y, int w, int h)
{
    int bpp = a->bpp;
    for (int r = y; r < y + h; r++) {
        memcpy(atlas_px(a, x - 1, r), atlas_px(a, x, r), bpp);
        memcpy(atlas_px(a, x + w, r), atlas_px(a, x + w - 1, r), bpp);
    }
    memcpy(atlas_px(a, x - 1, y - 1), atlas_px(a, x - 1, y), (size_t)(w + 2) * bpp);
    memcpy(atlas_px(a, x - 1, y + h), atlas_px(a, x - 1, y + h - 1), (size_t)(w + 2) * bpp);
}

// A glyph-like coverage mask: mostly empty or solid, with partial edges.
static void fill_mask(struct atlas *a, int x, int y, int w, int h)
{
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            int k = rnd_range(0, 8);
            *atlas_px(a, x + c, y + r) = k < 3 ? 0 : k < 5 ? 255 : (uint8_t)rnd_range(0, 256);
        }
    }
}

// Valid premultiplied BGRA: no channel exceeds alpha.
static void fill_bgra(struct atlas *a, int x, int y, int w, int h)
{
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            uint8_t *px = atlas_px(a, x + c, y + r);
            int k = rnd_range(0, 6);
            int alpha = k == 0 ? 0 : k == 1 ? 255 : rnd_range(0, 256);
            for (int ch = 0; ch < 3; ch++)
                px[ch] = (uint8_t)(alpha ? rnd_range(0, alpha + 1) : 0);
            px[3] = (uint8_t)alpha;
        }
    }
}

static struct sub_bitmap add_part(struct atlas *a, int w, int h, int x, int y)
{
    int ax, ay;
    if (!atlas_place(a, w, h, &ax, &ay)) {
        fprintf(stderr, "harness: atlas %dx%d too small\n", a->w, a->h);
        exit(2);
    }
    if (a->bpp == 4)
        fill_bgra(a, ax, ay, w, h);
    else
        fill_mask(a, ax, ay, w, h);
    atlas_pad(a, ax, ay, w, h);
    return (struct sub_bitmap){
        .bitmap = atlas_px(a, ax, ay),
        .stride = a->img.stride[0],
        .w = w, .h = h, .x = x, .y = y, .dw = w, .dh = h,
        .src_x = ax, .src_y = ay,
    };
}

// libass colors are RGBA with the last byte a transparency, not an opacity.
static uint32_t random_libass_color(void)
{
    int k = rnd_range(0, 10);
    uint32_t transparency = k < 2 ? 0 : k == 2 ? 255 : (uint32_t)rnd_range(0, 256);
    return (rnd() & 0xffffff00u) | transparency;
}

static struct sub_bitmaps make_imgs(struct atlas *a, enum sub_bitmap_format format,
                                    struct sub_bitmap *parts, int num_parts)
{
    return (struct sub_bitmaps){
        .format = format,
        .parts = parts,
        .num_parts = num_parts,
        .packed = &a->img,
        .packed_w = a->used_w,
        .packed_h = a->used_h,
    };
}

// ---- targets ------------------------------------------------------------------

static CVPixelBufferRef make_target(int w, int h)
{
    NSDictionary *attrs = @{
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (id)kCVPixelBufferMetalCompatibilityKey: @YES,
    };
    CVPixelBufferRef buf = NULL;
    if (CVPixelBufferCreate(kCFAllocatorDefault, w, h, kCVPixelFormatType_32BGRA,
                            (CFDictionaryRef)attrs, &buf) != kCVReturnSuccess) {
        fprintf(stderr, "harness: CVPixelBufferCreate failed\n");
        exit(2);
    }
    return buf;
}

// What a recycled pool buffer may still hold.
static void fill_garbage(CVPixelBufferRef buf)
{
    CVPixelBufferLockBaseAddress(buf, 0);
    uint8_t *base = CVPixelBufferGetBaseAddress(buf);
    size_t stride = CVPixelBufferGetBytesPerRow(buf);
    size_t h = CVPixelBufferGetHeight(buf);
    for (size_t i = 0; i < stride * h; i++)
        base[i] = (uint8_t)rnd();
    CVPixelBufferUnlockBaseAddress(buf, 0);
}

static void render_gpu(struct avf_metal_osd *m, CVPixelBufferRef target,
                       struct sub_bitmaps *imgs, const char *name)
{
    char err[256] = "";
    struct avf_metal_osd_timing timing = {0};
    if (!avf_metal_osd_draw(m, target, imgs, &timing, err, sizeof(err))) {
        fprintf(stderr, "FAIL %s: avf_metal_osd_draw: %s\n", name, err);
        exit(1);
    }
}

// The VO's CPU composition of the same part list into a cleared buffer.
static uint8_t *render_cpu(struct sub_bitmaps *imgs, int w, int h)
{
    size_t stride = (size_t)w * 4;
    uint8_t *base = calloc(stride * h, 1);
    for (int i = 0; i < imgs->num_parts; i++) {
        struct sub_bitmap *sb = &imgs->parts[i];
        if (imgs->format == SUBBITMAP_LIBASS)
            draw_direct_libass_bitmap(base, stride, w, h, sb);
        else
            draw_direct_bgra_bitmap(base, stride, w, h, sb);
    }
    return base;
}

// Compare the GPU target with the expected image, channel by channel. A
// scenario that expects coverage fails if the expected image is empty: it
// would check nothing.
static void compare(const char *name, CVPixelBufferRef target, const uint8_t *cpu,
                    int tolerance, bool expect_coverage)
{
    int w = (int)CVPixelBufferGetWidth(target);
    int h = (int)CVPixelBufferGetHeight(target);
    CVPixelBufferLockBaseAddress(target, kCVPixelBufferLock_ReadOnly);
    const uint8_t *base = CVPixelBufferGetBaseAddress(target);
    size_t stride = CVPixelBufferGetBytesPerRow(target);
    int max_diff = 0, over = 0, first_x = -1, first_y = -1;
    long covered = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *g = base + (size_t)y * stride;
        const uint8_t *c = cpu + (size_t)y * w * 4;
        for (int x = 0; x < w * 4; x++) {
            int d = abs((int)g[x] - (int)c[x]);
            if (d > tolerance && over++ == 0) {
                first_x = x / 4;
                first_y = y;
            }
            max_diff = MPMAX(max_diff, d);
            if ((x & 3) == 3 && c[x])
                covered++;
        }
    }
    CVPixelBufferUnlockBaseAddress(target, kCVPixelBufferLock_ReadOnly);
    if (over) {
        failures++;
        printf("FAIL %s: %d channels off by more than %d (max %d), first at %d,%d\n",
               name, over, tolerance, max_diff, first_x, first_y);
    } else {
        printf("ok   %s: max channel difference %d, %ld covered pixels\n",
               name, max_diff, covered);
    }
    if (expect_coverage && !covered) {
        failures++;
        printf("FAIL %s: the scenario drew nothing, so it checks nothing\n", name);
    }
}

// ---- scenarios ----------------------------------------------------------------

static void test_clear(struct avf_metal_osd *m)
{
    int w = 97, h = 61;
    CVPixelBufferRef target = make_target(w, h);
    uint8_t *zero = calloc((size_t)w * h * 4, 1);

    fill_garbage(target);
    render_gpu(m, target, NULL, "clear");
    compare("clear-only render leaves the recycled buffer transparent", target, zero, 0, false);

    struct atlas a;
    atlas_init(&a, 64, 64, IMGFMT_Y8);
    struct sub_bitmaps none = make_imgs(&a, SUBBITMAP_LIBASS, NULL, 0);
    fill_garbage(target);
    render_gpu(m, target, &none, "clear");
    compare("empty part list clears the recycled buffer", target, zero, 0, false);

    atlas_free(&a);
    free(zero);
    CVPixelBufferRelease(target);
}

static void test_libass(struct avf_metal_osd *m, const char *name, int w, int h,
                        int atlas_w, int atlas_h, int num_parts, bool stacked)
{
    struct atlas a;
    atlas_init(&a, atlas_w, atlas_h, IMGFMT_Y8);
    struct sub_bitmap *parts = calloc(num_parts, sizeof(*parts));
    for (int i = 0; i < num_parts; i++) {
        int pw, ph, x, y;
        if (stacked) {
            // Signs: dozens of layers (shadow, border, fill, gradient
            // strips) over the same few pixels.
            pw = rnd_range(40, 72);
            ph = rnd_range(24, 52);
            x = 100 + rnd_range(-6, 7);
            y = 50 + rnd_range(-6, 7);
        } else {
            pw = rnd_range(1, 61);
            ph = rnd_range(1, 41);
            x = rnd_range(-30, w + 10);
            y = rnd_range(-20, h + 10);
        }
        parts[i] = add_part(&a, pw, ph, x, y);
        parts[i].libass.color = random_libass_color();
    }
    struct sub_bitmaps imgs = make_imgs(&a, SUBBITMAP_LIBASS, parts, num_parts);

    CVPixelBufferRef target = make_target(w, h);
    fill_garbage(target);
    render_gpu(m, target, &imgs, name);
    uint8_t *cpu = render_cpu(&imgs, w, h);
    compare(name, target, cpu, 1, true);

    free(cpu);
    free(parts);
    atlas_free(&a);
    CVPixelBufferRelease(target);
}

// Full-width signs at 4K: more coverage than one capped atlas holds, so the
// rasterizer draws in several passes. Parts overlap throughout, so any pass
// that clears, reorders or drops parts shows up against the CPU composition.
static void test_libass_multipass(struct avf_metal_osd *m)
{
    const char *name = "parts beyond one atlas draw in order across passes";
    int w = 3200, h = 900, num_parts = 120;
    struct atlas a;
    atlas_init(&a, 6200, 7500, IMGFMT_Y8);
    struct sub_bitmap *parts = calloc(num_parts, sizeof(*parts));
    for (int i = 0; i < num_parts; i++) {
        parts[i] = add_part(&a, rnd_range(2900, 3100), rnd_range(110, 125),
                            rnd_range(-100, 200), rnd_range(-60, h - 60));
        parts[i].libass.color = random_libass_color();
    }
    struct sub_bitmaps imgs = make_imgs(&a, SUBBITMAP_LIBASS, parts, num_parts);

    CVPixelBufferRef target = make_target(w, h);
    fill_garbage(target);
    render_gpu(m, target, &imgs, name);
    uint8_t *cpu = render_cpu(&imgs, w, h);
    compare(name, target, cpu, 1, true);

    free(cpu);
    free(parts);
    atlas_free(&a);
    CVPixelBufferRelease(target);
}

static void test_bgra_unscaled(struct avf_metal_osd *m)
{
    const char *name = "unscaled BGRA parts match the CPU composition";
    int w = 250, h = 170;
    struct atlas a;
    atlas_init(&a, 600, 400, IMGFMT_BGRA);
    struct sub_bitmap parts[50];
    for (int i = 0; i < 50; i++)
        parts[i] = add_part(&a, rnd_range(1, 50), rnd_range(1, 35),
                            rnd_range(-20, w), rnd_range(-15, h));
    struct sub_bitmaps imgs = make_imgs(&a, SUBBITMAP_BGRA, parts, 50);

    CVPixelBufferRef target = make_target(w, h);
    fill_garbage(target);
    render_gpu(m, target, &imgs, name);
    uint8_t *cpu = render_cpu(&imgs, w, h);
    compare(name, target, cpu, 1, true);

    free(cpu);
    atlas_free(&a);
    CVPixelBufferRelease(target);
}

// Bitmap subtitles are scaled to the video rect (dw/dh != w/h). A uniform
// source makes the expected image exact regardless of filtering: the source
// color over exactly the destination rectangle, clipped to the target.
static void test_bgra_scaled(struct avf_metal_osd *m)
{
    const char *name = "scaled BGRA parts cover exactly their destination";
    int w = 120, h = 90;
    const uint8_t color[4] = {40, 80, 120, 200}; // premultiplied B, G, R, A
    struct atlas a;
    atlas_init(&a, 64, 64, IMGFMT_BGRA);
    struct { int sw, sh, x, y, dw, dh; } spec[] = {
        {16, 8, 7, 11, 50, 30},   // upscaled, inside
        {20, 10, -10, 70, 40, 25}, // upscaled, clipped left and bottom
        {12, 12, 90, 5, 6, 6},    // downscaled
    };
    int n = sizeof(spec) / sizeof(spec[0]);
    struct sub_bitmap parts[3];
    uint8_t *expected = calloc((size_t)w * h * 4, 1);
    for (int i = 0; i < n; i++) {
        int ax, ay;
        atlas_place(&a, spec[i].sw, spec[i].sh, &ax, &ay);
        for (int r = 0; r < spec[i].sh; r++) {
            for (int c = 0; c < spec[i].sw; c++)
                memcpy(atlas_px(&a, ax + c, ay + r), color, 4);
        }
        atlas_pad(&a, ax, ay, spec[i].sw, spec[i].sh);
        parts[i] = (struct sub_bitmap){
            .bitmap = atlas_px(&a, ax, ay), .stride = a.img.stride[0],
            .w = spec[i].sw, .h = spec[i].sh, .x = spec[i].x, .y = spec[i].y,
            .dw = spec[i].dw, .dh = spec[i].dh, .src_x = ax, .src_y = ay,
        };
        for (int y = MPMAX(spec[i].y, 0); y < MPMIN(spec[i].y + spec[i].dh, h); y++) {
            for (int x = MPMAX(spec[i].x, 0); x < MPMIN(spec[i].x + spec[i].dw, w); x++)
                memcpy(expected + ((size_t)y * w + x) * 4, color, 4);
        }
    }
    struct sub_bitmaps imgs = make_imgs(&a, SUBBITMAP_BGRA, parts, n);

    CVPixelBufferRef target = make_target(w, h);
    fill_garbage(target);
    render_gpu(m, target, &imgs, name);
    compare(name, target, expected, 0, true);

    free(expected);
    atlas_free(&a);
    CVPixelBufferRelease(target);
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            printf("SKIP: no Metal device\n");
            return 0;
        }
        [device release];

        struct avf_metal_osd m;
        char err[256] = "";
        if (!avf_metal_osd_init(&m, err, sizeof(err))) {
            printf("FAIL avf_metal_osd_init: %s\n", err);
            return 1;
        }

        test_clear(&m);
        test_libass(&m, "libass parts match the CPU composition, clipped at the edges",
                    333, 201, 512, 512, 80, false);
        // Larger atlas and part list than the previous render: both grow.
        test_libass(&m, "stacked libass layers match the CPU composition",
                    260, 160, 1400, 1100, 300, true);
        test_bgra_unscaled(&m);
        test_bgra_scaled(&m);
        test_libass_multipass(&m);
        // Back to a small render after growth.
        test_libass(&m, "small render after growth still matches", 64, 48, 128, 128, 12,
                    false);

        avf_metal_osd_uninit(&m);
    }
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all Metal OSD checks passed\n");
    return 0;
}
