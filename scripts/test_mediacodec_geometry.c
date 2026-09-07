// Compiles the production geometry region extracted from patch 0001, not a
// mirrored implementation. Optional libass coverage renders a positioned vector
// sign so neither fonts nor Android headers/devices are needed.
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mp_rect { int x0, y0, x1, y1; };
struct mp_osd_res {
    int w, h;
    int mt, mb, ml, mr;
    double display_par;
};

#include "mediacodec_geometry_core.inc"

static struct mediacodec_geometry parse(const char *text)
{
    struct mediacodec_geometry geometry;
    assert(mediacodec_geometry_parse(text, &geometry));
    return geometry;
}

static void margins(struct mediacodec_geometry geometry, int w, int h,
                    int ml, int mr, int mt, int mb)
{
    struct mp_osd_res res = mediacodec_osd_geometry(geometry, w, h);
    assert(res.w == w && res.h == h);
    assert(res.ml == ml && res.mr == mr && res.mt == mt && res.mb == mb);
}

static void geometry_cases(void)
{
    struct mediacodec_geometry contain = parse("2340,1080,210,0,2130,1080");
    margins(contain, 2340, 1080, 210, 210, 0, 0);
    margins(contain, 780, 360, 70, 70, 0, 0);
    margins(parse("1920,1080,0,135,1920,945"), 1920, 1080, 0, 0, 135, 135);
    margins(parse("1920,1080,0,0,1920,1080"), 640, 360, 0, 0, 0, 0);
    // Independent axis scaling after fractional resolution rounds to even sizes.
    margins(parse("1001,601,101,71,901,531"), 334, 202, 34, 33, 24, 24);
    margins(parse("1001,601,-101,-71,1102,672"), 334, 202, -34, -34, -24, -24);
    // Half-pixel signed edges must round symmetrically rather than toward zero.
    margins(parse("100,100,-1,-1,101,101"), 50, 50, -1, -1, -1, -1);

    struct mediacodec_geometry active = contain;
    const char *invalid[] = {
        "2340,1080,210,0,2130", "2340,1080,210,0,2130,1080,0",
        "2340,1080,210,0,2130,1080x", "2340,1080,210,0,2130,",
        "2340,1080,210, 0,2130,1080", "2340,1080,210,0,2130,1.5",
        "0,1080,0,0,1920,1080", "1920,-1,0,0,1920,1080",
        "1920,1080,1,0,1,1080", "1920,1080,0,1,1920,0",
        "2147483648,1080,0,0,1920,1080", "1920,1080,-2147483649,0,1,1",
        "1920,1080,0,0,99999999999999999999999,1080",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(!mediacodec_geometry_parse(invalid[i], &active));
        assert(mediacodec_geometry_equal(active, contain));
        margins(active, 2340, 1080, 210, 210, 0, 0);
    }
    assert(mediacodec_geometry_equal(parse("+2340,+1080,+210,+0,+2130,+1080"), contain));
    // Contain -> zoom -> unavailable -> contain must not retain old margins.
    assert(mediacodec_geometry_parse("2340,1080,-30,-135,2370,1215", &active));
    assert(!mediacodec_geometry_equal(active, contain));
    margins(active, 2340, 1080, -30, -30, -135, -135);
    assert(mediacodec_geometry_parse("", &active));
    margins(active, 2340, 1080, 0, 0, 0, 0);
    assert(mediacodec_geometry_parse("2340,1080,210,0,2130,1080", &active));
    assert(mediacodec_geometry_equal(active, contain));
    // Signed integer endpoints are valid input, but cannot overflow the renderer.
    margins(parse("1,1,-2147483648,-2147483648,2147483647,2147483647"),
            INT_MAX, INT_MAX, 0, 0, 0, 0);
    margins(parse("1000,1000,1,1,2,2"), 2, 2, 0, 0, 0, 0);
    puts("PASS: MediaCodec geometry, signed scaling, atomic updates and transitions");
}

#ifdef HAVE_LIBASS
#include <ass/ass.h>

static struct mp_rect render_sign(ASS_Renderer *renderer, ASS_Track *track,
                                  struct mp_osd_res res, bool use_margins)
{
    ass_set_frame_size(renderer, res.w, res.h);
    ass_set_storage_size(renderer, 1920, 1080);
    ass_set_margins(renderer, res.mt, res.mb, res.ml, res.mr);
    ass_set_use_margins(renderer, use_margins);
    ass_set_pixel_aspect(renderer, 1);
    int changed;
    ASS_Image *image = ass_render_frame(renderer, track, 1000, &changed);
    struct mp_rect box = {INT_MAX, INT_MAX, INT_MIN, INT_MIN};
    for (; image; image = image->next) {
        for (int y = 0; y < image->h; y++) {
            for (int x = 0; x < image->w; x++) {
                if (!image->bitmap[y * image->stride + x])
                    continue;
                int px = image->dst_x + x, py = image->dst_y + y;
                if (px < box.x0) box.x0 = px;
                if (py < box.y0) box.y0 = py;
                if (px + 1 > box.x1) box.x1 = px + 1;
                if (py + 1 > box.y1) box.y1 = py + 1;
            }
        }
    }
    assert(box.x1 > box.x0 && box.y1 > box.y0);
    return box;
}

static void rendered_cases(void)
{
    char script[] =
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,48,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1\n"
        "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\\an7\\pos(100,100)\\p1}m 0 0 l 400 0 400 100 0 100\n";
    ASS_Library *library = ass_library_init();
    assert(library);
    ASS_Renderer *renderer = ass_renderer_init(library);
    assert(renderer);
    ass_set_fonts(renderer, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    ASS_Track *track = ass_read_memory(library, script, strlen(script), NULL);
    assert(track);
    struct mp_osd_res full = mediacodec_osd_geometry(parse(""), 1920, 1080);
    struct mp_rect reference = render_sign(renderer, track, full, false);
    struct mp_osd_res correct = mediacodec_osd_geometry(
        parse("2340,1080,210,0,2130,1080"), 2340, 1080);
    for (int use = 0; use < 2; use++) {
        struct mp_rect actual = render_sign(renderer, track, correct, use);
        assert(actual.x0 == reference.x0 + 210 && actual.x1 == reference.x1 + 210);
        assert(actual.y0 == reference.y0 && actual.y1 == reference.y1);
        struct mp_rect broken = render_sign(renderer, track,
            mediacodec_osd_geometry(parse(""), 2340, 1080), use);
        assert(broken.x1 - broken.x0 > actual.x1 - actual.x0);
        // Returning from zero-margin/GPU-like canvas restores the exact sign.
        struct mp_rect restored = render_sign(renderer, track, correct, use);
        assert(restored.x0 == actual.x0 && restored.x1 == actual.x1);
    }
    ass_free_track(track);
    ass_renderer_done(renderer);
    ass_library_done(library);
    puts("PASS: rendered libass sign preserves picture-relative placement and scale");
}
#endif

int main(void)
{
    geometry_cases();
#ifdef HAVE_LIBASS
    rendered_cases();
#endif
    return 0;
}
