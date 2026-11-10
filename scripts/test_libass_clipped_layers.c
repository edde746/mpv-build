// Behavioral regression for libass 0015, which spares a run the layers its
// clip hides whole: no composite for a run that lies outside the clip
// rectangle, no shadow for one whose shadow does. It has to know how far a
// blur carries a bitmap, and that a fill and a border are the same bitmaps
// with a shadow and without.
//
// - A blurred box next to a clip: what the blur brings inside the clip is the
//   same pixels as with the clip wide open, for every blur and every distance,
//   with fast blur and without. A reach taken too short drops them.
// - A bordered line whose shadow is thrown off the screen is the same border
//   and fill as the line without a shadow, with an opaque fill and with a
//   translucent one.
// - An opaque box (BorderStyle 3) is drawn as its own shadow: thrown off the
//   screen it must not come back as a border.
// - A line off the screen whose shadow falls onto it still casts it.
#include "libass/ass.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 1280
#define H 720

#define HEAD \
    "[Script Info]\nScriptType: v4.00+\nPlayResX: 1280\nPlayResY: 720\n\n" \
    "[V4+ Styles]\n" \
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, " \
    "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, " \
    "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n" \
    "Style: Default,Sans,48,&H00FFFFFF,&H000000FF,&H00000000,&H00404040,0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1\n" \
    "Style: Box,Sans,48,&H00FFFFFF,&H000000FF,&H00000000,&H00404040,0,0,0,0,100,100,0,0,3,0,4,7,0,0,0,1\n\n" \
    "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"

static void quiet(int level, const char *fmt, va_list args, void *data)
{
    (void)level; (void)fmt; (void)args; (void)data;
}

static ASS_Library *lib;
static ASS_Renderer *renderer;

// The images of one event, as a track of its own; `fast` turns fast blur on.
static ASS_Image *render(ASS_Track **track, const char *style, const char *text, int fast)
{
    static char script[4096];
    snprintf(script, sizeof(script), HEAD "Dialogue: 0,0:00:00.00,0:00:10.00,%s,,0,0,0,,%s\n", style, text);
    if (*track)
        ass_free_track(*track);
    *track = ass_read_memory(lib, script, strlen(script), NULL);
    if (!*track || ass_track_set_feature(*track, ASS_FEATURE_FAST_BLUR, fast) < 0) {
        fprintf(stderr, "FAIL: no track, or no fast blur in this libass\n");
        exit(1);
    }
    return ass_render_frame(renderer, *track, 1000, NULL);
}

// What the images of one color cover, inside the columns [0, x1) and rows [0, y1).
static void cover(const ASS_Image *img, unsigned char *canvas, int x1, int y1)
{
    memset(canvas, 0, (size_t)W * H);
    for (; img; img = img->next)
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++) {
                int cx = img->dst_x + x, cy = img->dst_y + y;
                if (cx >= 0 && cx < x1 && cy >= 0 && cy < y1)
                    canvas[(size_t)cy * W + cx] = img->bitmap[(size_t)y * img->stride + x];
            }
}

static const ASS_Image *of_type(const ASS_Image *img, unsigned type)
{
    for (; img; img = img->next)
        if ((unsigned)img->type == type)
            return img;
    return NULL;
}

// A copy of the image of that type, or an empty one: the next render reuses the renderer's.
static ASS_Image keep(const ASS_Image *img, unsigned type)
{
    ASS_Image copy = {0};
    const ASS_Image *src = of_type(img, type);
    if (!src)
        return copy;
    copy = *src;
    copy.bitmap = malloc((size_t)src->w * src->h + 1);
    for (int y = 0; y < src->h; y++)
        memcpy(copy.bitmap + (size_t)y * src->w, src->bitmap + (size_t)y * src->stride, src->w);
    copy.stride = src->w;
    return copy;
}

static int same(const ASS_Image *a, const ASS_Image *b)
{
    if (!a->bitmap || !b->bitmap)
        return !a->bitmap && !b->bitmap;
    if (a->w != b->w || a->h != b->h || a->dst_x != b->dst_x || a->dst_y != b->dst_y)
        return 0;
    for (int y = 0; y < a->h; y++)
        if (memcmp(a->bitmap + (size_t)y * a->stride, b->bitmap + (size_t)y * b->stride, a->w))
            return 0;
    return 1;
}

int main(void)
{
    lib = ass_library_init();
    ass_set_message_cb(lib, quiet, NULL);
    renderer = ass_renderer_init(lib);
    ass_set_frame_size(renderer, W, H);
    ass_set_fonts(renderer, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    ASS_Track *track = NULL;
    unsigned char *inside = malloc((size_t)W * H), *open = malloc((size_t)W * H);
    int failed = 0;
    char text[512];

    // A box from (400, 300) to (600, 400), and a clip that ends `gap` pixels
    // to its left, or above it.
    static const double blurs[] = { 0.5, 1, 2, 3, 4, 6, 10, 20, 40 };
    static const int gaps[] = { 0, 1, 2, 4, 8, 16, 32, 64, 120, 250 };
    long reached = 0;
    for (int fast = 0; fast < 2; fast++)
        for (size_t b = 0; b < sizeof(blurs) / sizeof(blurs[0]); b++)
            for (size_t g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++)
                for (int vertical = 0; vertical < 2; vertical++) {
                    int x1 = vertical ? W : 400 - gaps[g], y1 = vertical ? 300 - gaps[g] : H;
                    snprintf(text, sizeof(text), "{\\pos(400,300)\\blur%g\\clip(0,0,%d,%d)\\p1}m 0 0 l 200 0 200 100 0 100",
                             blurs[b], x1, y1);
                    ASS_Image *img = render(&track, "Default", text, fast);
                    cover(img, inside, x1, y1);
                    // the box of the one image, if there is one
                    int box[4] = { img ? img->dst_x : 0, img ? img->dst_y : 0, img ? img->w : 0, img ? img->h : 0 };
                    snprintf(text, sizeof(text), "{\\pos(400,300)\\blur%g\\clip(0,0,%d,%d)\\p1}m 0 0 l 200 0 200 100 0 100",
                             blurs[b], W, H);
                    img = render(&track, "Default", text, fast);
                    cover(img, open, x1, y1);
                    // The image itself, not only what shows of it: the blurred
                    // bitmap is wider than what is not zero in it, and a frame
                    // has to be the same list of images.
                    int w = !img ? 0 : (img->dst_x + img->w < x1 ? img->dst_x + img->w : x1) - img->dst_x;
                    int h = !img ? 0 : (img->dst_y + img->h < y1 ? img->dst_y + img->h : y1) - img->dst_y;
                    if (!img || (img->next) || (w > 0 && h > 0 ? box[0] != img->dst_x || box[1] != img->dst_y ||
                                                                 box[2] != w || box[3] != h : box[2] || box[3])) {
                        fprintf(stderr, "FAIL: \\blur%g, clip %d px %s the box%s: not the open clip's image, cut\n",
                                blurs[b], gaps[g], vertical ? "above" : "left of", fast ? ", fast blur" : "");
                        failed = 1;
                    }
                    if (memcmp(inside, open, (size_t)W * H)) {
                        fprintf(stderr, "FAIL: \\blur%g, clip %d px %s the box%s: not what the blur brings inside\n",
                                blurs[b], gaps[g], vertical ? "above" : "left of", fast ? ", fast blur" : "");
                        failed = 1;
                    }
                    for (size_t i = 0; i < (size_t)W * H; i++)
                        reached += open[i] != 0;
                }
    if (!reached) {
        fprintf(stderr, "FAIL: no blur reached into any clip: the fixture tests nothing\n");
        failed = 1;
    }

    // The shadow thrown off the screen: the line without a shadow.
    static const char *const fills[] = { "", "\\1a&H80&" };
    ASS_Image *img;
    for (int fast = 0; fast < 2; fast++)
        for (int f = 0; f < 2; f++) {
            snprintf(text, sizeof(text), "{\\pos(100,100)\\bord3\\blur1%s}Shadow", fills[f]);
            img = render(&track, "Default", text, fast);
            ASS_Image border = keep(img, IMAGE_TYPE_OUTLINE), fill = keep(img, IMAGE_TYPE_CHARACTER);
            snprintf(text, sizeof(text), "{\\pos(100,100)\\bord3\\blur1%s\\xshad3000\\yshad-2000}Shadow", fills[f]);
            img = render(&track, "Default", text, fast);
            ASS_Image border_s = keep(img, IMAGE_TYPE_OUTLINE), fill_s = keep(img, IMAGE_TYPE_CHARACTER);
            if (!border.bitmap || !fill.bitmap || !same(&border, &border_s) || !same(&fill, &fill_s) ||
                    of_type(img, IMAGE_TYPE_SHADOW)) {
                fprintf(stderr, "FAIL: a line whose shadow is off the screen is not the line without one%s%s\n",
                        f ? " (translucent fill)" : "", fast ? ", fast blur" : "");
                failed = 1;
            }
            free(border.bitmap); free(fill.bitmap); free(border_s.bitmap); free(fill_s.bitmap);
        }

    // The opaque box is its own shadow, and gone with it.
    img = render(&track, "Box", "{\\pos(100,100)\\bord0\\xshad3000\\yshad0}Box", 1);
    if (!of_type(img, IMAGE_TYPE_CHARACTER) || of_type(img, IMAGE_TYPE_OUTLINE) || of_type(img, IMAGE_TYPE_SHADOW)) {
        fprintf(stderr, "FAIL: an opaque box thrown off the screen as a shadow left something but its text\n");
        failed = 1;
    }

    // Off the screen, casting its shadow onto it.
    img = render(&track, "Default", "{\\pos(-600,100)\\bord2\\xshad700\\yshad0}Cast", 1);
    const ASS_Image *shadow = of_type(img, IMAGE_TYPE_SHADOW);
    if (!shadow || shadow->w <= 0 || of_type(img, IMAGE_TYPE_CHARACTER)) {
        fprintf(stderr, "FAIL: a line off the screen does not cast its shadow onto it\n");
        failed = 1;
    }
    // And with nothing of it on the screen there is nothing.
    if (render(&track, "Default", "{\\pos(-600,100)\\bord2\\xshad5\\blur3}Gone", 1)) {
        fprintf(stderr, "FAIL: a line off the screen left an image\n");
        failed = 1;
    }

    free(inside);
    free(open);
    ass_free_track(track);
    ass_renderer_done(renderer);
    ass_library_done(lib);
    if (!failed)
        puts("PASS: what a clip hides whole is not drawn, and nothing else is missing");
    return failed;
}
