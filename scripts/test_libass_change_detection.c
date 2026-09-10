// Behavioral regression for the libass series' static-event layout cache:
// rendering the same static frame twice must report the second render as
// unchanged (ass_render_frame's detect_change == 0). The cache captures
// owned-buffer images (opaque boxes, vector clips) on the miss frame; a copy
// there would hand the next frame a clone with a different bitmap pointer,
// which ass_detect_change reports as changed content, and every consumer
// keyed on that flag (mpv's sub bitmap packer) would redo the frame.
// Also exercise live renderer handoffs: releasing layout snapshots must leave
// retained output intact and let the next owner cache unchanged frames normally.
#include "libass/ass.h"
#include <stdio.h>
#include <stdint.h>

// libass's frame-retention API is declared in its private ass_render.h.
// This fixture links the static library without depending on private structs.
void ass_frame_ref(ASS_Image *img);
void ass_frame_unref(ASS_Image *img);

static const char script[] =
    "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
    "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
    "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,Sans,40,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,2,1,2,10,10,10,1\n"
    "Style: Box,Sans,40,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,3,2,1,2,10,10,10,1\n\n"
    "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
    "Dialogue: 0,0:00:00.00,0:00:10.00,Box,,0,0,0,,Opaque box background\n"
    "Dialogue: 1,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\\blur2\\pos(320,100)}Blurred over the box\n"
    "Dialogue: 2,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\\clip(m 100 100 l 500 100 500 300 100 300)\\pos(320,200)}Vector clipped\n"
    "Dialogue: 3,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\\an5\\pos(320,220)\\p1\\c&H0000FF&}m 0 0 l 200 0 200 60 0 60{\\p0}\n";

static void quiet(int level, const char *fmt, va_list args, void *data)
{
    (void)level; (void)fmt; (void)args; (void)data;
}

// Compare the observable image output, not allocation addresses or uninitialized
// stride padding. Retained frames must keep both their geometry and mask pixels.
static uint64_t frame_hash(const ASS_Image *img)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (; img; img = img->next) {
        uint32_t fields[] = {
            img->w, img->h, img->color, img->dst_x, img->dst_y,
        };
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
            for (int shift = 0; shift < 32; shift += 8) {
                hash ^= (fields[i] >> shift) & 255;
                hash *= UINT64_C(1099511628211);
            }
        }
        for (int y = 0; y < img->h; y++) {
            for (int x = 0; x < img->w; x++) {
                hash ^= img->bitmap[(size_t)y * img->stride + x];
                hash *= UINT64_C(1099511628211);
            }
        }
    }
    return hash;
}

static int check_frame(ASS_Renderer *renderer, ASS_Track *track, long long now,
                       uint64_t expected, int expected_change, const char *label)
{
    int changed = -1;
    ASS_Image *img = ass_render_frame(renderer, track, now, &changed);
    if (!img || frame_hash(img) != expected ||
        (expected_change >= 0 && changed != expected_change)) {
        fprintf(stderr, "FAIL: %s at %lld ms: pixels or change detection (%d)\n",
                label, now, changed);
        return 1;
    }
    return 0;
}

static int check_retained(const ASS_Image *img, uint64_t expected,
                          const char *label)
{
    if (!img || frame_hash(img) != expected) {
        fprintf(stderr, "FAIL: retained %s image geometry or pixels changed\n", label);
        return 1;
    }
    return 0;
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    ass_set_message_cb(lib, quiet, NULL);
    ASS_Renderer *r = ass_renderer_init(lib);
    ass_set_threads(r, 2);
    ass_set_frame_size(r, 1280, 720);
    ass_set_fonts(r, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    ASS_Track *t = ass_read_memory(lib, (char *)script, sizeof(script) - 1, NULL);
    if (!t) {
        fprintf(stderr, "FAIL: the script did not parse\n");
        return 1;
    }

    int failed = 0;
    for (int i = 0; i < 4; i++) {
        int changed = -1;
        ASS_Image *img = ass_render_frame(r, t, 1000 + i * 40, &changed);
        int images = 0;
        for (ASS_Image *c = img; c; c = c->next)
            images++;
        printf("render %d: changed=%d images=%d\n", i, changed, images);
        if (i == 0 && images < 4) {
            fprintf(stderr, "FAIL: %d images; are fonts available?\n", images);
            failed = 1;
        }
        if (i == 0 && changed != 2) {
            fprintf(stderr, "FAIL: the first render must report changed content\n");
            failed = 1;
        }
        if (i > 0 && changed != 0) {
            fprintf(stderr, "FAIL: render %d of an unchanged frame reported %d\n", i, changed);
            failed = 1;
        }
    }

    uint64_t overlap = frame_hash(ass_render_frame(r, t, 1160, NULL));

    // The old renderer owns the long-lived box/blur events; the warm renderer
    // first sees the clip/drawing events in their overlapping final second.
    // Unlike a blank old frame, this forces release of live owned-buffer
    // snapshots while preserving the warm renderer's independently owned ones.
    for (int i = 0; i < t->n_events; i++) {
        t->events[i].Start = i < 2 ? 0 : 2000;
        t->events[i].Duration = 3000 - t->events[i].Start;
    }
    ass_render_frame(r, t, 0, NULL);
    ASS_Image *old_frame = ass_render_frame(r, t, 40, NULL);
    ass_frame_ref(old_frame);
    uint64_t early = frame_hash(old_frame);
    if (!old_frame || early == overlap) {
        fprintf(stderr, "FAIL: the early and overlapping cues must have distinct pixels\n");
        failed = 1;
    }
    ASS_Renderer *warm = ass_renderer_init(lib);
    ass_set_threads(warm, 2);
    ass_set_frame_size(warm, 1280, 720);
    ass_set_fonts(warm, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    ASS_Image *warm_frame = ass_render_frame(warm, t, 2001, NULL);
    ass_frame_ref(warm_frame);
    failed |= check_retained(warm_frame, overlap, "prefetched overlap");
    failed |= check_frame(r, t, 80, early, 0, "old owner before release");
    failed |= check_frame(r, t, 120, early, 0, "old owner before release");

    ass_renderer_clear_layout_cache(r);
    failed |= check_retained(old_frame, early, "old owner after release");
    failed |= check_retained(warm_frame, overlap, "warm owner after old release");
    // Reacquiring formerly foreign snapshots can change bitmap identities on
    // this first render; the next static frame must be unchanged.
    failed |= check_frame(warm, t, 2042, overlap, -1, "forward handoff");
    failed |= check_frame(warm, t, 2082, overlap, 0, "forward handoff repeat");
    ass_renderer_clear_layout_cache(r);
    failed |= check_frame(warm, t, 2122, overlap, 0, "released owner stays detached");

    // Hand the same live track back without destroying either renderer.
    ass_renderer_clear_layout_cache(warm);
    failed |= check_retained(warm_frame, overlap, "warm owner after release");
    failed |= check_frame(r, t, 2162, overlap, 2, "reverse handoff");
    failed |= check_frame(r, t, 2202, overlap, 0, "reverse handoff repeat");
    failed |= check_retained(old_frame, early, "old owner after reuse");

    ASS_Renderer *renderers[] = {r, warm};
    for (size_t i = 0; i < sizeof(renderers) / sizeof(renderers[0]); i++) {
        int changed = -1;
        if (ass_render_frame(renderers[i], t, 3001, &changed) || changed == 0) {
            fprintf(stderr, "FAIL: renderer %zu did not signal cue clearing\n", i);
            failed = 1;
        }
        changed = -1;
        if (ass_render_frame(renderers[i], t, 3041, &changed) || changed != 0) {
            fprintf(stderr, "FAIL: renderer %zu did not keep the cleared frame unchanged\n", i);
            failed = 1;
        }
    }
    failed |= check_frame(r, t, 0, early, 2, "old renderer replay after clearing");
    failed |= check_frame(r, t, 40, early, 0, "old renderer replay repeat");
    failed |= check_retained(old_frame, early, "old owner after cue clearing and replay");
    failed |= check_retained(warm_frame, overlap, "warm owner after cue clearing and replay");

    ass_frame_unref(old_frame);
    ass_frame_unref(warm_frame);

    ass_free_track(t);
    ass_renderer_done(r);
    ass_renderer_done(warm);
    ass_library_done(lib);
    if (failed)
        return 1;
    printf("PASS: libass preserves pixels, retained images and change detection across reusable renderer handoffs\n");
    return 0;
}
