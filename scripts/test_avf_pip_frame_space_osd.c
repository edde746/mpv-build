// Regression test for the frame-space OSD geometry used by the PiP compositing
// path (patch/libmpv/0024-avfoundation-frame-space-osd-in-pip.patch).
//
// The geometry and the subtitle bounding-box mapping are compiled VERBATIM from
// the patched mpv source (the marked "avf frame-space osd core" region, which is
// deliberately freestanding C); see test_avf_pip_frame_space_osd.sh.
//
// What the harness models around them:
//   - vo_get_src_dst_rects()'s letterboxed window-space geometry, which is what
//     the composite path used before this patch;
//   - libass line placement for a converted (SRT/VTT) subtitle, which with
//     --sub-use-margins=yes (mpv's default) is laid out on the whole canvas and
//     therefore lands in the letterbox margin at high --sub-pos values;
//   - which composite path vo_avfoundation then takes, and hence the dimensions
//     of the sample it enqueues: the dimension-preserving NV12/P010 blend when
//     the box maps onto the frame, else the Core Image fallback, which is sized
//     from the geometry.
//
// What is asserted: the enqueued sample dimensions never depend on whether a
// subtitle is visible, at any --sub-pos, because the system sizes the PiP window
// from them (plezy#2078). The window-space behavior is run on the same inputs to
// document the bug being fixed.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define MPMAX(a, b) ((a) > (b) ? (a) : (b))
#define MPMIN(a, b) ((a) > (b) ? (b) : (a))
#define MPCLAMP(a, min, max) (((a) > (max)) ? (max) : (((a) < (min)) ? (min) : (a)))

// Stand-ins for the mpv types the extracted region uses. Field names and
// meanings match sub/osd.h and video/mp_image.h; the extracted code only ever
// touches these members.
struct mp_rect {
    int x0, y0, x1, y1;
};

struct mp_osd_res {
    int w, h;
    int mt, mb, ml, mr;
    double display_par;
};

struct avf_render_geometry {
    int w, h;
    struct mp_rect src, dst;
    struct mp_osd_res osd_res;
};

#include "frame_space_core.inc"

// ---- models ---------------------------------------------------------------

// vo_get_src_dst_rects() for the unzoomed, unpanscanned case: the video is
// scaled to fit the window and centered, and the OSD covers the whole window
// with the letterbox bars recorded as margins.
static struct avf_render_geometry window_space_geometry(int win_w, int win_h,
                                                        int frame_w, int frame_h)
{
    double scale_w = win_w / (double)frame_w;
    double scale_h = win_h / (double)frame_h;
    double scale = scale_w < scale_h ? scale_w : scale_h;
    int dst_w = (int)(frame_w * scale + 0.5);
    int dst_h = (int)(frame_h * scale + 0.5);
    int x0 = (win_w - dst_w) / 2;
    int y0 = (win_h - dst_h) / 2;

    return (struct avf_render_geometry){
        .w = win_w,
        .h = win_h,
        .src = {0, 0, frame_w, frame_h},
        .dst = {x0, y0, x0 + dst_w, y0 + dst_h},
        .osd_res = {
            .w = win_w,
            .h = win_h,
            .ml = x0,
            .mr = win_w - (x0 + dst_w),
            .mt = y0,
            .mb = win_h - (y0 + dst_h),
            .display_par = 1.0,
        },
    };
}

// Bounding box libass produces for one centered line of converted-subtitle
// text: ass_set_line_position(100 - sub_pos) places the line's bottom edge
// (100 - sub_pos) percent of the canvas height above the canvas bottom, clamped
// to stay on the canvas, and with use_margins the canvas is the whole OSD,
// letterbox margins included.
static struct mp_rect subtitle_box(struct mp_osd_res res, int sub_pos,
                                   double text_h_frac, double text_w_frac)
{
    int text_h = (int)(res.h * text_h_frac + 0.5);
    int text_w = (int)(res.w * text_w_frac + 0.5);
    int y1 = MPCLAMP((int)(res.h * (sub_pos / 100.0) + 0.5), text_h, res.h);
    int x0 = (res.w - text_w) / 2;

    return (struct mp_rect){x0, y1 - text_h, x0 + text_w, y1};
}

// Dimensions of the sample create_video_sample_buffer() ends up enqueueing.
// Without a subtitle it is the decoded frame; with one it is the frame again if
// the box maps onto the frame's pixel grid (the in-place NV12/P010 blend), else
// the Core Image fallback's geometry-sized output.
struct dims {
    int w, h;
};

static struct dims enqueued_dims(struct avf_render_geometry geometry,
                                 int frame_w, int frame_h, bool has_subtitle,
                                 struct mp_rect sub, bool *mapped)
{
    struct mp_rect mapped_rect = {0};
    bool ok = has_subtitle &&
              avf_map_subtitle_rect(geometry, sub, geometry.w, geometry.h,
                                    frame_w, frame_h, &mapped_rect);
    if (mapped)
        *mapped = ok;

    if (!has_subtitle || ok)
        return (struct dims){frame_w, frame_h};

    return (struct dims){geometry.w, geometry.h};
}

// How much of the cue's height the composite actually blends, as a fraction of
// the box mapped into the frame's grid. Below 1 the cue is visibly cut off at
// the video edge; 0 means the composite refused the box entirely.
static double blended_fraction(struct avf_render_geometry geometry,
                               struct mp_rect sub, int frame_w, int frame_h)
{
    struct mp_rect out = {0};
    if (!avf_map_subtitle_rect(geometry, sub, geometry.w, geometry.h,
                               frame_w, frame_h, &out))
        return 0.0;

    double scale = (geometry.src.y1 - geometry.src.y0) /
                   (double)(geometry.dst.y1 - geometry.dst.y0);
    double full = (sub.y1 - sub.y0) * scale;
    return full > 0 ? (out.y1 - out.y0) / full : 0.0;
}

static bool rect_inside(struct mp_rect r, int w, int h)
{
    return r.x0 >= 0 && r.y0 >= 0 && r.x1 <= w && r.y1 <= h &&
           r.x1 > r.x0 && r.y1 > r.y0;
}

static uint32_t rng_state = 0x9e3779b9u;

static uint32_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int rng_range(int lo, int hi)
{
    return lo + (int)(rng_next() % (uint32_t)(hi - lo + 1));
}

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        failures++; \
        printf("FAIL: " __VA_ARGS__); \
        printf("  [%s]\n", #cond); \
    } \
} while (0)

int main(void)
{
    // 1. Frame-space geometry is the frame itself: no margins for a subtitle to
    //    fall into, and the OSD canvas the composite writes is the coded grid,
    //    so a composite cannot change the sample size.
    {
        struct avf_render_geometry g = avf_frame_space_geometry(1920, 1080, 1, 1);
        printf("frame-space 1920x1080: geom=%dx%d dst=%d,%d-%d,%d par=%.4f\n",
               g.w, g.h, g.dst.x0, g.dst.y0, g.dst.x1, g.dst.y1,
               g.osd_res.display_par);
        CHECK(g.w == 1920 && g.h == 1080, "geometry must be the frame size\n");
        CHECK(g.src.x0 == 0 && g.src.y0 == 0 && g.src.x1 == 1920 && g.src.y1 == 1080,
              "src must be the whole frame\n");
        CHECK(g.dst.x0 == 0 && g.dst.y0 == 0 && g.dst.x1 == 1920 && g.dst.y1 == 1080,
              "dst must be the whole frame\n");
        CHECK(g.osd_res.w == 1920 && g.osd_res.h == 1080,
              "the OSD canvas must be the frame\n");
        CHECK(g.osd_res.ml == 0 && g.osd_res.mr == 0 &&
              g.osd_res.mt == 0 && g.osd_res.mb == 0,
              "the frame has no letterbox margins\n");
        CHECK(g.osd_res.display_par == 1.0, "square pixels\n");
    }

    // 2. Anamorphic source: the canvas stays the coded grid (so the composite
    //    output keeps the coded size that carries the sample-aspect attachment),
    //    and display_par squeezes the text the way the layer will stretch it
    //    back out. Same convention as osd_res_from_image_params().
    {
        struct avf_render_geometry g = avf_frame_space_geometry(720, 576, 64, 45);
        printf("frame-space 720x576 par 64:45: geom=%dx%d par=%.5f\n",
               g.w, g.h, g.osd_res.display_par);
        CHECK(g.w == 720 && g.h == 576, "coded size must be preserved\n");
        CHECK(g.osd_res.display_par == 45 / 64.0, "display_par must be p_h/p_w\n");
    }

    // 3. Degenerate frame size yields a zeroed geometry, which every caller
    //    treats as "cannot render".
    {
        struct avf_render_geometry g = avf_frame_space_geometry(0, 1080, 1, 1);
        CHECK(g.w == 0 && g.h == 0, "no frame, no geometry\n");
    }

    // 4. The bug, on the reporter's hardware: an iPad Pro 13" (2752x2064 layer
    //    pixels) playing a 16:9 frame. The letterbox bars are part of the OSD
    //    canvas, so a cue laid out far enough down misses the video rect
    //    entirely; window-space compositing then falls back to Core Image,
    //    whose output is canvas-sized, and the PiP window follows the sample.
    //    Portrait has the deep bars that make --sub-pos=90 miss (the reported
    //    setting); landscape's shallower bars take the shipped default of 100.
    {
        struct {
            const char *name;
            int win_w, win_h;
            int breaking_pos;
        } cases[] = {
            {"portrait ", 2064, 2752, 90},
            {"landscape", 2752, 2064, 100},
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            struct avf_render_geometry win =
                window_space_geometry(cases[i].win_w, cases[i].win_h, 1920, 1080);
            struct mp_rect broken = subtitle_box(win.osd_res, cases[i].breaking_pos,
                                                 0.06, 0.5);
            bool mapped = false;
            struct dims with = enqueued_dims(win, 1920, 1080, true, broken, &mapped);
            struct dims without = enqueued_dims(win, 1920, 1080, false, broken, NULL);

            printf("window-space %s: canvas %dx%d, video rect y %d..%d, "
                   "sub-pos %d -> sample %dx%d (no cue: %dx%d)\n",
                   cases[i].name, win.w, win.h, win.dst.y0, win.dst.y1,
                   cases[i].breaking_pos, with.w, with.h, without.w, without.h);

            CHECK(win.osd_res.mt > 0 && win.osd_res.mb > 0,
                  "%s: 16:9 in a non-16:9 window letterboxes\n", cases[i].name);
            CHECK(!mapped, "%s: sub-pos %d sits in the bar\n",
                  cases[i].name, cases[i].breaking_pos);
            CHECK(with.w != without.w || with.h != without.h,
                  "%s: sub-pos %d must reproduce the PiP resize\n",
                  cases[i].name, cases[i].breaking_pos);

            // Mid-frame cues always landed on the video rect, which is why the
            // reporter saw the resizing stop at --sub-pos=50.
            struct mp_rect mid = subtitle_box(win.osd_res, 50, 0.06, 0.5);
            struct dims mid_dims = enqueued_dims(win, 1920, 1080, true, mid, &mapped);
            CHECK(mapped, "%s: sub-pos 50 maps onto the video rect\n", cases[i].name);
            CHECK(mid_dims.w == 1920 && mid_dims.h == 1080,
                  "%s: sub-pos 50 never resized\n", cases[i].name);
            CHECK(blended_fraction(win, mid, 1920, 1080) >= 1.0,
                  "%s: a cue inside the video rect blends whole\n", cases[i].name);
        }

        // Between the two: the cue straddles the video edge, so the sample size
        // holds but the composite clips the overhanging half away: the same
        // margin layout, cut off instead of resizing.
        struct avf_render_geometry land = window_space_geometry(2752, 2064, 1920, 1080);
        struct mp_rect straddling = subtitle_box(land.osd_res, 90, 0.06, 0.5);
        double covered = blended_fraction(land, straddling, 1920, 1080);
        printf("window-space landscape: sub-pos 90 straddles the edge, "
               "%.0f%% of the cue blended\n", covered * 100.0);
        CHECK(covered > 0.0 && covered < 0.8,
              "a straddling cue is clipped at the video edge\n");
    }

    // 5. The fix, same content: every --sub-pos maps onto the frame, so the
    //    enqueued sample is the frame at all times and the PiP window holds
    //    still. The cue is also inside the frame, i.e. actually visible.
    {
        struct avf_render_geometry g = avf_frame_space_geometry(1920, 1080, 1, 1);
        int worst_top = 0;
        for (int pos = 0; pos <= 100; pos++) {
            struct mp_rect sub = subtitle_box(g.osd_res, pos, 0.06, 0.5);
            struct mp_rect mapped_rect = {0};
            bool mapped = avf_map_subtitle_rect(g, sub, g.w, g.h, 1920, 1080,
                                                &mapped_rect);
            struct dims with = enqueued_dims(g, 1920, 1080, true, sub, NULL);
            struct dims without = enqueued_dims(g, 1920, 1080, false, sub, NULL);
            CHECK(mapped, "sub-pos %d must map onto the frame\n", pos);
            CHECK(with.w == without.w && with.h == without.h,
                  "sub-pos %d must not change the sample size\n", pos);
            CHECK(rect_inside(mapped_rect, 1920, 1080),
                  "sub-pos %d must blend inside the frame\n", pos);
            // Frame space is an identity mapping, so the blended rect is the
            // box plus the one pixel of slack, never a rescaled guess.
            CHECK(mapped_rect.x0 >= sub.x0 - 1 && mapped_rect.x1 <= sub.x1 + 1 &&
                  mapped_rect.y0 >= sub.y0 - 1 && mapped_rect.y1 <= sub.y1 + 1,
                  "sub-pos %d mapping must be the identity\n", pos);
            if (mapped_rect.y0 > worst_top)
                worst_top = mapped_rect.y0;
        }
        printf("frame-space sweep: sub-pos 0..100 all in-frame, lowest cue top y=%d\n",
               worst_top);
    }

    // 6. Fuzz: arbitrary frame sizes and boxes. A box overlapping the frame is
    //    always blended in bounds; a box entirely outside it is refused rather
    //    than clamped onto some edge.
    {
        int mapped_count = 0, refused_count = 0;
        for (int i = 0; i < 20000; i++) {
            int fw = rng_range(16, 4096);
            int fh = rng_range(16, 2304);
            struct avf_render_geometry g =
                avf_frame_space_geometry(fw, fh, rng_range(1, 64), rng_range(1, 64));
            int x0 = rng_range(-fw, 2 * fw);
            int y0 = rng_range(-fh, 2 * fh);
            struct mp_rect sub = {x0, y0, x0 + rng_range(1, fw), y0 + rng_range(1, fh)};
            struct mp_rect out = {0};
            bool mapped = avf_map_subtitle_rect(g, sub, fw, fh, fw, fh, &out);

            bool overlaps = sub.x0 < fw && sub.y0 < fh && sub.x1 > 0 && sub.y1 > 0;
            if (mapped) {
                mapped_count++;
                CHECK(rect_inside(out, fw, fh), "blend rect must stay in the frame\n");
                CHECK(overlaps, "a box outside the frame must not map\n");
            } else {
                refused_count++;
                struct dims with = enqueued_dims(g, fw, fh, true, sub, NULL);
                CHECK(with.w == fw && with.h == fh,
                      "a refused overlay must not resize the sample\n");
            }
        }
        printf("fuzz: %d mapped, %d refused, sample size never moved\n",
               mapped_count, refused_count);
        CHECK(mapped_count > 0 && refused_count > 0, "fuzz must cover both outcomes\n");
    }

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }

    printf("\nall checks passed\n");
    return 0;
}
