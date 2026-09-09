// Behavioral regression for the libass fork's static-event layout cache:
// rendering the same static frame twice must report the second render as
// unchanged (ass_render_frame's detect_change == 0). The cache captures
// owned-buffer images (opaque boxes, vector clips) on the miss frame; a copy
// there would hand the next frame a clone with a different bitmap pointer,
// which ass_detect_change reports as changed content, and every consumer
// keyed on that flag (mpv's sub bitmap packer) would redo the frame.
#include "libass/ass.h"
#include <stdio.h>
#include <string.h>

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

int main(void)
{
    ASS_Library *lib = ass_library_init();
    ass_set_message_cb(lib, quiet, NULL);
    ASS_Renderer *r = ass_renderer_init(lib);
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

    ass_free_track(t);
    ass_renderer_done(r);
    ass_library_done(lib);
    if (failed)
        return 1;
    printf("PASS: libass reports a repeated static frame as unchanged\n");
    return 0;
}
