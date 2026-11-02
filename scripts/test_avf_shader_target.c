// Behavioral regressions for avf_shader_target, compiled verbatim from patch
// 0004 by test_avf_shader_target.sh: the size vo_avfoundation renders user
// shaders at and the part of the frame that fills it, for each display-layer
// gravity. The display layer shows the result under the gravity it applies to
// every frame, so a wrong size or crop shows as a stretched, cropped or
// rescaled picture, or as an upscaler working for pixels the layer throws away.
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "shader_target.inc"

static int failures;

static void check(const char *name, int frame_w, int frame_h, int p_w, int p_h,
                  int layer_w, int layer_h, enum avf_gravity gravity, int max_dim,
                  int w, int h, double x0, double y0, double x1, double y1)
{
    struct avf_shaders_target t = {0};
    bool ok = avf_shader_target(frame_w, frame_h, p_w, p_h, layer_w, layer_h,
                                gravity, max_dim, &t);
    if (!ok || t.w != w || t.h != h || fabs(t.x0 - x0) > 1e-6 ||
        fabs(t.y0 - y0) > 1e-6 || fabs(t.x1 - x1) > 1e-6 || fabs(t.y1 - y1) > 1e-6)
    {
        printf("FAIL %s: got %s %dx%d [%.3f,%.3f %.3f,%.3f], want %dx%d "
               "[%.3f,%.3f %.3f,%.3f]\n", name, ok ? "ok" : "none", t.w, t.h,
               t.x0, t.y0, t.x1, t.y1, w, h, x0, y0, x1, y1);
        failures++;
    }
}

static void check_none(const char *name, int frame_w, int frame_h, int p_w, int p_h,
                       int layer_w, int layer_h, enum avf_gravity gravity, int max_dim)
{
    struct avf_shaders_target t = {0};
    if (avf_shader_target(frame_w, frame_h, p_w, p_h, layer_w, layer_h, gravity,
                          max_dim, &t))
    {
        printf("FAIL %s: got %dx%d, want no target\n", name, t.w, t.h);
        failures++;
    }
}

int main(void)
{
    const enum avf_gravity fit = AVF_GRAVITY_ASPECT;
    const enum avf_gravity fill = AVF_GRAVITY_ASPECT_FILL;
    const enum avf_gravity resize = AVF_GRAVITY_RESIZE;

    // Aspect fit: the whole frame, at the size the layer shows it.
    check("1080p on a 4K layer", 1920, 1080, 1, 1, 3840, 2160, fit, 8192,
          3840, 2160, 0, 0, 1920, 1080);
    check("scope on a 4K layer keeps its bars out", 1920, 800, 1, 1, 3840, 2160, fit, 8192,
          3840, 1600, 0, 0, 1920, 800);
    check("4:3 pillarboxed", 1440, 1080, 1, 1, 3840, 2160, fit, 8192,
          2880, 2160, 0, 0, 1440, 1080);
    check("4K on a 1080p layer renders at the shown size", 3840, 2160, 1, 1, 1920, 1080,
          fit, 8192, 1920, 1080, 0, 0, 3840, 2160);
    check("portrait layer", 1920, 1080, 1, 1, 1080, 1920, fit, 8192,
          1080, 608, 0, 0, 1920, 1080);
    check("odd layer rounds down to even sides", 1920, 1080, 1, 1, 1001, 1001, fit, 8192,
          1000, 562, 0, 0, 1920, 1080);

    // The sample aspect is applied: the output has square pixels.
    check("HDV 1440x1080 at 4:3 pixels is 16:9", 1440, 1080, 4, 3, 3840, 2160, fit, 8192,
          3840, 2160, 0, 0, 1440, 1080);
    check("NTSC DVD widescreen", 720, 480, 32, 27, 1920, 1080, fit, 8192,
          1920, 1080, 0, 0, 720, 480);
    check("PAL DVD 4:3", 720, 576, 16, 15, 1920, 1080, fit, 8192,
          1440, 1080, 0, 0, 720, 576);
    check("an unknown sample aspect is square", 1920, 1080, 0, 0, 3840, 2160, fit, 8192,
          3840, 2160, 0, 0, 1920, 1080);

    // Aspect fill: the centred part fill shows, at the layer's size.
    check("fill crops 4:3 top and bottom", 1440, 1080, 1, 1, 3840, 2160, fill, 8192,
          3840, 2160, 0, 135, 1440, 945);
    check("fill crops scope left and right", 1920, 800, 1, 1, 3840, 2160, fill, 8192,
          3840, 2160, 248.888889, 0, 1671.111111, 800);
    check("fill crop is in the frame's own pixels", 720, 576, 16, 15, 1920, 1080, fill, 8192,
          1920, 1080, 0, 72, 720, 504);
    check("fill of a matching shape crops nothing", 1920, 1080, 1, 1, 3840, 2160, fill, 8192,
          3840, 2160, 0, 0, 1920, 1080);

    // Resize: the whole frame stretched to the layer.
    check("resize stretches to the layer", 1920, 800, 1, 1, 3840, 2160, resize, 8192,
          3840, 2160, 0, 0, 1920, 800);
    check("resize ignores the sample aspect", 720, 480, 32, 27, 1280, 720, resize, 8192,
          1280, 720, 0, 0, 720, 480);

    // The longest side stays within max_dim, keeping the shape.
    check("8K cap on a huge layer", 1920, 1080, 1, 1, 16000, 9000, fit, 8192,
          8192, 4608, 0, 0, 1920, 1080);
    check("8K cap keeps a stretched shape", 1920, 1080, 1, 1, 10000, 5000, resize, 8192,
          8192, 4096, 0, 0, 1920, 1080);
    check("8K cap on fill keeps the layer's shape", 1440, 1080, 1, 1, 9600, 5400, fill, 8192,
          8192, 4608, 0, 135, 1440, 945);

    // Nothing to render into.
    check_none("layer without a size", 1920, 1080, 1, 1, 0, 0, fit, 8192);
    check_none("a one-pixel-high layer rounds to nothing", 1920, 1080, 1, 1, 3840, 1, fit, 8192);
    check_none("frame without a size", 0, 1080, 1, 1, 3840, 2160, fit, 8192);
    check_none("one-pixel layer", 1920, 1080, 1, 1, 1, 1, fit, 8192);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("avf shader target: all checks passed\n");
    return 0;
}
