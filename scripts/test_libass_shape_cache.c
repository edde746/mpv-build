// Behavioral regression for the libass series' shape cache: a run shaped once
// is reused on later frames, so every input HarfBuzz sees must be part of the
// key. Each case renders a baseline cue and then a cue whose shaped run differs
// from it in one input only (font size, font, kerning),
// through one warm renderer. Every frame must match what a fresh renderer, whose
// cache has never seen the baseline, draws for it. A key that dropped the
// differing input would hand the second cue the baseline's glyphs.
#include "libass/ass.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define HEADER \
    "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n%s\n" \
    "[V4+ Styles]\n" \
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, " \
    "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, " \
    "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n" \
    "Style: Default,Sans,40,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,0,0,5,10,10,10,1\n\n" \
    "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"

// One cue per second; case N's baseline is at 2N s and its variant at 2N+1 s.
// The variants keep the baseline's text for the run under test.
static const char *const cues[][2] = {
    { "size",    "{\\pos(320,180)}AVAWAY To Wa" },
    { "size",    "{\\pos(320,180)\\fs41}AVAWAY To Wa" },
    { "font",    "{\\pos(320,180)}AVAWAY To Wa" },
    { "font",    "{\\pos(320,180)\\fnTimes New Roman}AVAWAY To Wa" },
};

static void quiet(int level, const char *fmt, va_list args, void *data)
{
    (void)level; (void)fmt; (void)args; (void)data;
}

static uint64_t frame_hash(const ASS_Image *img)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (; img; img = img->next) {
        uint32_t fields[] = { img->w, img->h, img->color, img->dst_x, img->dst_y };
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

// The last image's size and pixels, without its position: the glyph a run
// was shaped to, wherever it lands.
static uint64_t last_glyph_hash(const ASS_Image *img)
{
    const ASS_Image *last = img;
    for (; img; img = img->next)
        last = img;
    uint64_t hash = UINT64_C(14695981039346656037);
    if (!last)
        return hash;
    uint32_t fields[] = { last->w, last->h };
    for (size_t i = 0; i < 2; i++) {
        hash ^= fields[i];
        hash *= UINT64_C(1099511628211);
    }
    for (int y = 0; y < last->h; y++) {
        for (int x = 0; x < last->w; x++) {
            hash ^= last->bitmap[(size_t)y * last->stride + x];
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

static ASS_Renderer *new_renderer(ASS_Library *lib)
{
    ASS_Renderer *r = ass_renderer_init(lib);
    // Unhinted, libass shapes every size at one nominal size and scales the
    // result, so the size case would reach the shaper with an identical run.
    // Hinting shapes at the real size.
    ass_set_hinting(r, ASS_HINTING_LIGHT);
    ass_set_threads(r, 2);
    ass_set_frame_size(r, 1280, 720);
    ass_set_fonts(r, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    return r;
}

static ASS_Track *new_track(ASS_Library *lib, const char *info, int first, int count)
{
    char script[4096];
    int len = snprintf(script, sizeof(script), HEADER, info);
    for (int i = 0; i < count; i++)
        len += snprintf(script + len, sizeof(script) - len,
                        "Dialogue: 0,0:00:%02d.00,0:00:%02d.00,Default,,0,0,0,,%s\n",
                        first + i, first + i + 1, cues[first + i][1]);
    ASS_Track *t = ass_read_memory(lib, script, len, NULL);
    // mpv shapes whole events, so a run's context is the rest of its cue.
    if (t)
        ass_track_set_feature(t, ASS_FEATURE_WHOLE_TEXT_LAYOUT, 1);
    return t;
}

// What a renderer that has shaped nothing else draws at time ms.
static uint64_t fresh_hash(ASS_Library *lib, ASS_Track *t, long long ms)
{
    ASS_Renderer *r = new_renderer(lib);
    uint64_t hash = frame_hash(ass_render_frame(r, t, ms, NULL));
    ass_renderer_done(r);
    return hash;
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    ass_set_message_cb(lib, quiet, NULL);
    int n = sizeof(cues) / sizeof(cues[0]);
    ASS_Track *t = new_track(lib, "", 0, n);
    if (!t) {
        fprintf(stderr, "FAIL: the script did not parse\n");
        return 1;
    }

    int failed = 0;
    ASS_Renderer *warm = new_renderer(lib);
    for (int i = 0; i < n; i += 2) {
        long long base_ms = i * 1000 + 500, variant_ms = base_ms + 1000;
        ASS_Image *img = ass_render_frame(warm, t, base_ms, NULL);
        uint64_t base_glyph = last_glyph_hash(img);
        img = ass_render_frame(warm, t, variant_ms, NULL);
        uint64_t variant = frame_hash(img);
        // The differing input must change the shaped run itself, or the
        // case proves nothing (a missing font falls back to the baseline's).
        if (base_glyph == last_glyph_hash(img)) {
            fprintf(stderr, "FAIL: %s: both cues end in the same glyph; are the fonts available?\n",
                    cues[i][0]);
            failed = 1;
            continue;
        }
        if (variant != fresh_hash(lib, t, variant_ms)) {
            fprintf(stderr, "FAIL: %s: the second cue reused the first cue's shaping\n",
                    cues[i][0]);
            failed = 1;
        }
    }

    // Kerning is a script property, so its case needs two tracks: the same
    // cue shaped with kerning on, then off, through one renderer that has
    // already shaped it without the header (libass's default is off).
    ASS_Track *kern_on = new_track(lib, "Kerning: yes\n", 0, 1);
    ASS_Track *kern_off = new_track(lib, "Kerning: no\n", 0, 1);
    if (!kern_on || !kern_off) {
        fprintf(stderr, "FAIL: the kerning scripts did not parse\n");
        failed = 1;
    } else {
        uint64_t on = frame_hash(ass_render_frame(warm, kern_on, 500, NULL));
        uint64_t off = frame_hash(ass_render_frame(warm, kern_off, 500, NULL));
        uint64_t fresh_on = fresh_hash(lib, kern_on, 500);
        uint64_t fresh_off = fresh_hash(lib, kern_off, 500);
        if (fresh_on == fresh_off) {
            fprintf(stderr, "FAIL: kerning: the cues draw identically; does the font kern?\n");
            failed = 1;
        } else if (on != fresh_on || off != fresh_off) {
            fprintf(stderr, "FAIL: kerning: a cue reused shaping from the other kerning setting\n");
            failed = 1;
        }
        ass_free_track(kern_on);
        ass_free_track(kern_off);
    }

    ass_renderer_done(warm);
    ass_free_track(t);
    ass_library_done(lib);
    if (failed)
        return 1;
    printf("PASS: libass shapes cached runs exactly as a cold renderer does\n");
    return 0;
}
