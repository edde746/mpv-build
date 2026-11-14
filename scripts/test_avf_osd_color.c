// Behavioral regression for vo_avfoundation's overlay colour, compiled
// verbatim from the final Apple series by test_avf_osd_color.sh. The
// compositor converts each sample from the colour it carries, so an overlay
// buffer shown over SDR video must carry the video's colour, all of it but
// the YCbCr matrix: then a subtitle pixel converts exactly as the picture's
// pixel of the same RGB, and a sign or mask matched to the picture stays
// matched on screen. Over HDR video, or video that carries no colour, the
// overlay carries none, whatever an earlier file left on a pool buffer.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <stdbool.h>
#include <stdio.h>

#include "avf_osd_color.inc"

static int failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static CVPixelBufferRef buffer(OSType format)
{
    CVPixelBufferRef pb = NULL;
    CVPixelBufferCreate(NULL, 64, 36, format, NULL, &pb);
    return pb;
}

static void set(CVPixelBufferRef pb, CFStringRef key, CFTypeRef value)
{
    if (value)
        CVBufferSetAttachment(pb, key, value, kCVAttachmentMode_ShouldPropagate);
}

// A decoded frame as FFmpeg's VideoToolbox hwaccel or the VO's upload tags
// it: its stream's primaries, transfer, gamma and matrix, and the colour
// space CoreVideo derives from them.
static CVPixelBufferRef video(CFStringRef primaries, CFStringRef transfer,
                              CFStringRef matrix, float gamma)
{
    CVPixelBufferRef pb = buffer(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    set(pb, kCVImageBufferColorPrimariesKey, primaries);
    set(pb, kCVImageBufferTransferFunctionKey, transfer);
    set(pb, kCVImageBufferYCbCrMatrixKey, matrix);
    if (gamma) {
        CFNumberRef level = CFNumberCreate(NULL, kCFNumberFloat32Type, &gamma);
        set(pb, kCVImageBufferGammaLevelKey, level);
        CFRelease(level);
    }
    CFDictionaryRef att = CVBufferGetAttachments(pb, kCVAttachmentMode_ShouldPropagate);
    CGColorSpaceRef cs = att ? CVImageBufferCreateColorSpaceFromAttachments(att) : NULL;
    if (cs) {
        set(pb, kCVImageBufferCGColorSpaceKey, cs);
        CGColorSpaceRelease(cs);
    }
    return pb;
}

static bool same(CVPixelBufferRef a, CVPixelBufferRef b, CFStringRef key)
{
    CFTypeRef x = CVBufferGetAttachment(a, key, NULL);
    CFTypeRef y = CVBufferGetAttachment(b, key, NULL);
    return x == y || (x && y && CFEqual(x, y));
}

// The overlay as the VO shows it over v: the colour avf_osd_color_create
// takes from v, given to a pool buffer.
static void show_over(CVPixelBufferRef overlay, CVPixelBufferRef v, bool hdr)
{
    CFDictionaryRef color = avf_osd_color_create(v, hdr);
    avf_osd_color_apply(color, overlay);
    if (color)
        CFRelease(color);
}

static bool carries_colour(CVPixelBufferRef pb)
{
    const CFStringRef keys[] = AVF_OSD_COLOR_KEYS;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (CVBufferGetAttachment(pb, keys[i], NULL))
            return true;
    }
    return false;
}

int main(void)
{
    CVPixelBufferRef overlay = buffer(kCVPixelFormatType_32BGRA);

    // SDR: BT.709 (the issue's anime release), and a gamma-2.2 stream, whose
    // transfer means nothing without its level.
    struct {
        const char *name;
        CVPixelBufferRef v;
    } sdr[] = {
        {"BT.709", video(kCVImageBufferColorPrimaries_ITU_R_709_2,
                         kCVImageBufferTransferFunction_ITU_R_709_2,
                         kCVImageBufferYCbCrMatrix_ITU_R_709_2, 0)},
        {"gamma 2.2", video(kCVImageBufferColorPrimaries_ITU_R_709_2,
                            kCVImageBufferTransferFunction_UseGamma,
                            kCVImageBufferYCbCrMatrix_ITU_R_601_4, 2.2f)},
    };
    for (size_t n = 0; n < sizeof(sdr) / sizeof(sdr[0]); n++) {
        show_over(overlay, sdr[n].v, false);
        char what[160];
        snprintf(what, sizeof(what), "%s: the overlay has the video's colour space", sdr[n].name);
        check(CVImageBufferGetColorSpace(overlay) &&
              same(overlay, sdr[n].v, kCVImageBufferCGColorSpaceKey), what);
        snprintf(what, sizeof(what), "%s: the overlay has the video's primaries, transfer and gamma",
                 sdr[n].name);
        check(same(overlay, sdr[n].v, kCVImageBufferColorPrimariesKey) &&
              same(overlay, sdr[n].v, kCVImageBufferTransferFunctionKey) &&
              same(overlay, sdr[n].v, kCVImageBufferGammaLevelKey), what);
        snprintf(what, sizeof(what), "%s: the overlay, RGB, has no YCbCr matrix", sdr[n].name);
        check(!CVBufferGetAttachment(overlay, kCVImageBufferYCbCrMatrixKey, NULL), what);
    }

    // The same pool buffer over the next files: HDR, then a stream without
    // colour. Nothing of the SDR colour may stay behind.
    CVPixelBufferRef pq = video(kCVImageBufferColorPrimaries_ITU_R_2020,
                                kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ,
                                kCVImageBufferYCbCrMatrix_ITU_R_2020, 0);
    show_over(overlay, pq, true);
    check(!carries_colour(overlay), "over PQ video the overlay carries no colour");

    show_over(overlay, sdr[0].v, false);
    CVPixelBufferRef untagged = video(NULL, NULL, NULL, 0);
    show_over(overlay, untagged, false);
    check(!carries_colour(overlay), "over video without colour the overlay carries none");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("avf osd color: all checks passed\n");
    return 0;
}
