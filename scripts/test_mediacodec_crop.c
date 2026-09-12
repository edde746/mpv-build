// Exercise the production format parser with the configured and decoded
// MediaFormats emitted by MediaTek. No Android runtime is needed.
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AVERROR_EXTERNAL (-1)
#define AVERROR(value) (-(value))
#define AV_PIX_FMT_NONE (-1)
#define FFALIGN(value, alignment) (((value) + (alignment) - 1) & ~((alignment) - 1))
#define av_log(...) ((void)0)

typedef struct { int num, den; } AVRational;
typedef struct {
    int width, height, pix_fmt, color_range, colorspace, color_primaries, color_trc;
    AVRational sample_aspect_ratio;
} AVCodecContext;

typedef struct {
    const char *key;
    int32_t value;
} Field;
typedef struct {
    Field fields[16];
    int count, has_rect;
    int32_t left, top, right, bottom;
} Format;
typedef struct {
    Format *format;
    const char *codec_name;
    int use_ndk_codec, width, height, stride, slice_height, color_format;
    int crop_left, crop_top, crop_right, crop_bottom, display_width, display_height;
} MediaCodecDecContext;

static int ff_AMediaFormat_getInt32(Format *format, const char *key, int32_t *value)
{
    for (int i = 0; i < format->count; ++i) {
        if (!strcmp(format->fields[i].key, key)) {
            *value = format->fields[i].value;
            return 1;
        }
    }
    return 0;
}

static int ff_AMediaFormat_getRect(Format *format, const char *key,
                                   int32_t *left, int32_t *top,
                                   int32_t *right, int32_t *bottom)
{
    if (!format->has_rect)
        return 0;
    *left = format->left;
    *top = format->top;
    *right = format->right;
    *bottom = format->bottom;
    return 1;
}

static char *ff_AMediaFormat_toString(Format *format)
{
    char *description = malloc(1);
    *description = '\0';
    return description;
}

static void av_freep(char **value) { free(*value); *value = NULL; }
static int mcdec_map_color_format(AVCodecContext *avctx, MediaCodecDecContext *s, int value) { return value; }
static AVRational av_div_q(AVRational a, AVRational b) { return (AVRational){a.num * b.den, a.den * b.num}; }
static void ff_set_sar(AVCodecContext *avctx, AVRational sar) { avctx->sample_aspect_ratio = sar; }
static int ff_AMediaFormatColorRange_to_AVColorRange(int value) { return value; }
static int ff_AMediaFormatColorStandard_to_AVColorSpace(int value) { return value; }
static int ff_AMediaFormatColorStandard_to_AVColorPrimaries(int value) { return value; }
static int ff_AMediaFormatColorTransfer_to_AVColorTransfer(int value) { return value; }
static int ff_set_dimensions(AVCodecContext *avctx, int width, int height)
{
    avctx->width = width;
    avctx->height = height;
    return 0;
}

#include "mediacodec_crop.inc"

static Format format(int width, int height)
{
    return (Format){
        .fields = {{"width", width}, {"height", height}, {"color-format", 1}},
        .count = 3,
    };
}

static int dimensions(const char *name, AVCodecContext *avctx, MediaCodecDecContext *s,
                      int changed, int width, int height)
{
    if (mediacodec_dec_parse_video_format(avctx, s, changed) < 0 ||
        avctx->width != width || avctx->height != height) {
        fprintf(stderr, "FAIL: %s: expected %dx%d, got %dx%d\n",
                name, width, height, avctx->width, avctx->height);
        return 0;
    }
    return 1;
}

int main(void)
{
    AVCodecContext avctx = {.width = 3840, .height = 1604};
    Format media = format(3840, 1604);
    media.has_rect = 1;
    media.right = 318;
    media.bottom = 238;
    MediaCodecDecContext s = {
        .format = &media, .codec_name = "c2.mtk.dvhe.sth.decoder", .use_ndk_codec = 1,
    };
    if (!dimensions("configured placeholder crop", &avctx, &s, 0, 3840, 1604)) return 1;

    media.fields[0].value = 4096;
    media.fields[1].value = 2176;
    media.right = 3839;
    media.bottom = 1603;
    if (!dimensions("decoded crop excludes buffer padding", &avctx, &s, 1, 3840, 1604)) return 1;

    media.fields[0].value = 2048;
    media.fields[1].value = 1088;
    media.left = 32;
    media.top = 4;
    media.right = 1951;
    media.bottom = 1083;
    if (!dimensions("resolution change with crop offsets", &avctx, &s, 1, 1920, 1080)) return 1;

    // The Java keys take precedence, even if the NDK rectangle differs.
    media.fields[media.count++] = (Field){"crop-left", 0};
    media.fields[media.count++] = (Field){"crop-top", 0};
    media.fields[media.count++] = (Field){"crop-right", 1279};
    media.fields[media.count++] = (Field){"crop-bottom", 719};
    if (!dimensions("individual crop keys retain precedence", &avctx, &s, 1, 1280, 720)) return 1;
    s.use_ndk_codec = 0;
    if (!dimensions("Java decoder crop remains unchanged", &avctx, &s, 1, 1280, 720)) return 1;

    // A decoder with no crop metadata retains its reported dimensions.
    media = format(1920, 1080);
    s.use_ndk_codec = 1;
    if (!dimensions("missing crop metadata", &avctx, &s, 1, 1920, 1080)) return 1;
    media.fields[media.count++] = (Field){"crop-width", 1280};
    media.fields[media.count++] = (Field){"crop-height", 720};
    if (!dimensions("legacy crop dimensions", &avctx, &s, 1, 1280, 720)) return 1;

    puts("PASS: MediaCodec configured crop, padded output, resolution changes and legacy formats");
    return 0;
}
