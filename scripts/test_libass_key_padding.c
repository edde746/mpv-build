// Behavioral regression for the libass series' cache key hashes: a key must
// hash the same whatever its padding holds. The series hashes a key's members
// as byte runs, and where the ABI pads between two members (ARM EABI aligns a
// double to 8 bytes, so every key that starts with a pointer and a size has
// four bytes of padding on ARMv7) the run used to include the padding, which
// is whatever the stack held before: equal keys hashed differently, the glyph
// was looked up in another bucket, and its outline, bitmap and blur were made
// again. The frame came out the same but was reported as changed, on other
// threads always.
//
// An event with a finished \t is rendered through the whole pipeline on every
// frame (no layout snapshot), so every frame looks its glyphs up again. Fill
// the stack with another byte before each render: the frames must be reported
// unchanged. Meaningful on ARMv7 (cross-compile with the Android ARMv7
// compiler against the patched libass and its dependencies, and run on a
// device with a fonts directory and a family in it as arguments, e.g.
// `/system/fonts Roboto`: that libass has no system font provider). On
// targets without such padding it passes by construction, and
// test_libass_series.sh runs it there with the system's fonts.
#include "libass/ass.h"
#include <stdio.h>
#include <stddef.h>

static const char script[] =
    "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
    "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
    "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,Sans,40,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,2,1,2,10,10,10,1\n\n"
    "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
    "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\\t(0,500,\\frz8)\\blur1}Cache key padding\n";

static void quiet(int level, const char *fmt, va_list args, void *data)
{
    (void)level; (void)fmt; (void)args; (void)data;
}

// Leave `value` in the stack memory the next call chain will run in.
__attribute__((noinline)) static void fill_stack(unsigned char value)
{
    volatile unsigned char stack[128 * 1024];
    for (size_t i = 0; i < sizeof(stack); i++)
        stack[i] = value;
}

int main(int argc, char **argv)
{
    ASS_Library *lib = ass_library_init();
    ass_set_message_cb(lib, quiet, NULL);
    if (argc > 1)
        ass_set_fonts_dir(lib, argv[1]);
    ASS_Renderer *r = ass_renderer_init(lib);
    ass_set_threads(r, 1);  // the lookups happen on this thread's stack
    ass_set_frame_size(r, 1280, 720);
    ass_set_fonts(r, NULL, argc > 2 ? argv[2] : "sans-serif",
                  argc > 1 ? ASS_FONTPROVIDER_NONE : ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    ASS_Track *t = ass_read_memory(lib, (char *)script, sizeof(script) - 1, NULL);
    if (!t) {
        fprintf(stderr, "FAIL: the script did not parse\n");
        return 1;
    }

    static const unsigned char fill[] = {0x00, 0xFF, 0x5A, 0xA5, 0x01, 0x80};
    int failed = 0;
    for (size_t i = 0; i < sizeof(fill); i++) {
        int changed = -1;
        fill_stack(fill[i]);
        ASS_Image *img = ass_render_frame(r, t, 1000 + 40 * (long long)i, &changed);
        if (!img) {
            fprintf(stderr, "FAIL: nothing is rendered; are fonts available?\n");
            return 1;
        }
        if (i > 0 && changed != 0) {
            fprintf(stderr, "FAIL: frame %zu, the same as the one before, was reported as changed (%d) "
                    "with 0x%02X on the stack\n", i, changed, fill[i]);
            failed = 1;
        }
    }

    ass_free_track(t);
    ass_renderer_done(r);
    ass_library_done(lib);
    if (failed)
        return 1;
    printf("PASS: cached glyphs are found again whatever the stack held\n");
    return 0;
}
