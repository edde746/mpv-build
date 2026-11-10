// Bit-exactness of vo_avfoundation's NEON libass blend against its scalar
// reference, both compiled verbatim from the final Apple series by
// test_avf_osd_blend.sh. Every subtitle pixel on the inline OSD layer goes
// through the NEON kernel; any disagreement is a visible color shift.
#include <arm_neon.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MPMIN(a, b) ((a) > (b) ? (b) : (a))

#include "avf_osd_blend.inc"

static int failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static unsigned reference(unsigned m, unsigned c, unsigned a, unsigned d)
{
    uint32_t px = d * 0x01010101u, color = c << 24 | c << 16 | c << 8 | (255 - a);
    uint8_t mask = m;
    blend_libass_bgra_scalar((uint8_t *)&px, 4, &mask, 1, 1, 1, color);
    return px & 0xff;
}

int main(void)
{
    // The division: floor(y / 65025) for every numerator the blend forms.
    bool div_ok = true;
    for (uint32_t y = 0; y < (1u << 24) && div_ok; y += 4) {
        uint32_t in[4] = {y, y + 1, y + 2, y + 3};
        uint16_t out[4];
        vst1_u16(out, div65025_u32_neon(vld1q_u32(in)));
        for (int i = 0; i < 4; i++)
            div_ok &= out[i] == in[i] / 65025;
    }
    check(div_ok, "div65025_u32_neon is exact below 2^24");

    // One channel, every mask, color and destination value, for a spread of
    // color alphas including opaque (its own path) and nearly transparent.
    static const unsigned alphas[] = {1, 2, 64, 127, 128, 155, 200, 254, 255};
    bool chan_ok = true, opaque_ok = true;
    for (size_t ai = 0; ai < sizeof(alphas) / sizeof(alphas[0]); ai++) {
        unsigned a = alphas[ai];
        for (unsigned c = 0; c < 256; c++) {
            for (unsigned d = 0; d < 256; d++) {
                for (unsigned m0 = 0; m0 < 256; m0 += 8) {
                    uint8_t mv[8], got[8], got_opaque[8];
                    for (int i = 0; i < 8; i++)
                        mv[i] = m0 + i;
                    uint8x8_t m = vld1_u8(mv), dv = vdup_n_u8(d);
                    uint16x8_t aa = vmull_u8(m, vdup_n_u8(a));
                    uint16x8_t inv = vsubq_u16(vdupq_n_u16(255 * 255), aa);
                    vst1_u8(got, blend_libass_channel_neon(aa, inv, c, dv));
                    if (a == 255)
                        vst1_u8(got_opaque, blend_opaque_channel_neon(m, vmvn_u8(m), vdup_n_u8(c), dv));
                    for (int i = 0; i < 8; i++) {
                        unsigned want = reference(m0 + i, c, a, d);
                        chan_ok &= got[i] == want;
                        if (a == 255)
                            opaque_ok &= got_opaque[i] == want;
                    }
                }
            }
        }
    }
    check(chan_ok, "the general channel blend matches the scalar one");
    check(opaque_ok, "the opaque channel blend matches the scalar one");

    // Whole kernel on libass-like masks (runs of 0, runs of 255, edges) over
    // a random canvas, opaque and translucent colors.
    enum { W = 1024, H = 256, STRIDE = W * 4 };
    uint8_t *ref = malloc(STRIDE * H), *neon = malloc(STRIDE * H);
    srand(1);
    bool kernel_ok = true;
    for (int it = 0; it < 2000 && kernel_ok; it++) {
        int w = 8 * (1 + rand() % 100), h = 1 + rand() % 100, ss = (w + 31) & ~31;
        uint8_t *mask = malloc((size_t)ss * h);
        for (int i = 0; i < ss * h; i++) {
            int k = rand() % 4;
            mask[i] = k == 0 ? 0 : k == 1 ? 255 : rand() & 255;
        }
        for (int y = 0; y < h; y++) {
            if (rand() % 3 == 0)
                memset(mask + y * ss + rand() % (w / 2), rand() % 2 ? 0 : 255,
                       rand() % (w / 2 + 1));
        }
        for (int i = 0; i < STRIDE * H; i++)
            ref[i] = neon[i] = rand();
        uint32_t color = (uint32_t)rand() << 8 | (rand() & 0xff);
        if (it % 2)
            color &= ~0xffu;
        int x0 = rand() % (W - w + 1), y0 = rand() % (H - h + 1);
        size_t off = (size_t)y0 * STRIDE + (size_t)x0 * 4;
        blend_libass_bgra_scalar(ref + off, STRIDE, mask, ss, w, h, color);
        blend_libass_bgra_neon(neon + off, STRIDE, mask, ss, w, h, color);
        kernel_ok = !memcmp(ref, neon, STRIDE * H);
        free(mask);
    }
    check(kernel_ok, "the NEON kernel matches the scalar one on libass-like masks");

    // The premultiplied BGRA blend: every source alpha, source channel and
    // destination value (the scalar one shortcuts an opaque source and an
    // empty destination; the kernel must give what those give).
    bool over_ok = true;
    for (unsigned sa = 0; sa < 256 && over_ok; sa++) {
        for (unsigned c = 0; c < 256; c++) {
            uint32_t src[256], want[256], got[256];
            for (unsigned d = 0; d < 256; d++) {
                src[d] = sa << 24 | c << 16 | c << 8 | c;
                // The empty destination in a lane of its own, and one whose
                // alpha alone is set.
                want[d] = got[d] = d * 0x01010101u ^ (d == 7 ? 0x07000000u : 0);
            }
            blend_bgra_over_bgra_scalar((uint8_t *)want, 256 * 4, (uint8_t *)src, 256 * 4, 256, 1);
            blend_bgra_over_bgra_neon((uint8_t *)got, 256 * 4, (uint8_t *)src, 256 * 4, 256, 1);
            over_ok &= !memcmp(want, got, sizeof(want));
        }
    }
    check(over_ok, "the BGRA-over-BGRA kernel matches the scalar one for every alpha, channel and destination");

    // And on random premultiplied bitmaps with transparent and opaque runs.
    bool over_kernel_ok = true;
    for (int it = 0; it < 2000 && over_kernel_ok; it++) {
        int w = 8 * (1 + rand() % 100), h = 1 + rand() % 60, ss = w * 4 + 16 * (rand() % 3);
        uint8_t *bmp = malloc((size_t)ss * h);
        for (int i = 0; i < ss * h; i += 4) {
            int k = rand() % 4;
            unsigned al = k == 0 ? 0 : k == 1 ? 255 : rand() & 255;
            bmp[i + 3] = al;
            for (int ch = 0; ch < 3; ch++)
                bmp[i + ch] = al ? rand() % (al + 1) : 0;
        }
        for (int i = 0; i < STRIDE * H; i++)
            ref[i] = neon[i] = rand() % 3 ? rand() : 0;
        int x0 = rand() % (W - w + 1), y0 = rand() % (H - h + 1);
        size_t off = (size_t)y0 * STRIDE + (size_t)x0 * 4;
        blend_bgra_over_bgra_scalar(ref + off, STRIDE, bmp, ss, w, h);
        blend_bgra_over_bgra_neon(neon + off, STRIDE, bmp, ss, w, h);
        over_kernel_ok = !memcmp(ref, neon, STRIDE * H);
        free(bmp);
    }
    check(over_kernel_ok, "the BGRA-over-BGRA kernel matches the scalar one on random bitmaps");
    free(ref);
    free(neon);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("avf osd blend: ok\n");
    return 0;
}
