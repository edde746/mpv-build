// Regression test for where vo_avfoundation places its inline OSD layer
// (patches/mpv/pool/0004-avfoundation-video-output.patch).
//
// layer_untransformed_frame() is compiled VERBATIM from the patch; see
// test_avf_osd_layer_geometry.sh. It runs against real Core Animation layers,
// laid out the way vo_avfoundation lays them out: the OSD layer is a sibling of
// the host's display layer, anchored at its centre, given the display layer's
// geometry as its frame, and it stretches a subtitle canvas sized from the
// display layer's bounds over its own bounds (AVLayerVideoGravityResize).
//
// What is asserted: a transform on the display layer -- a host's viewer zoom --
// never reaches the OSD layer, which stays over the untransformed picture with
// a canvas that maps 1:1 onto it, so a subtitle mpv laid out on screen stays
// on screen (plezy#2693). Without a transform the result is the display
// layer's frame, which is what the OSD layer was given before. The copied
// `frame` is run on the zoomed inputs too, to document the bug being fixed.

#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>
#include <math.h>
#include <stdio.h>

#include "osd_layer_geometry.inc"

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        failures++; \
        printf("FAIL: " __VA_ARGS__); \
        printf("  [%s]\n", #cond); \
    } \
} while (0)

static bool near(CGFloat a, CGFloat b)
{
    return fabs(a - b) < 1e-9;
}

static bool rect_near(CGRect a, CGRect b)
{
    return near(a.origin.x, b.origin.x) && near(a.origin.y, b.origin.y) &&
           near(a.size.width, b.size.width) && near(a.size.height, b.size.height);
}

static void print_rect(const char *label, CGRect r)
{
    printf("  %-26s (%.1f, %.1f, %.1f x %.1f)\n", label,
           r.origin.x, r.origin.y, r.size.width, r.size.height);
}

// The OSD layer as ensure_osd_layer() creates it beside `display`.
static AVSampleBufferDisplayLayer *make_osd_layer(CALayer *display, CGRect frame)
{
    AVSampleBufferDisplayLayer *osd = [[AVSampleBufferDisplayLayer alloc] init];
    osd.frame = frame;
    osd.anchorPoint = CGPointMake(0.5, 0.5);
    osd.videoGravity = AVLayerVideoGravityResize;
    [display.superlayer addSublayer:osd];
    return osd;
}

// Where a point of the subtitle canvas lands in the host's container: the
// canvas is sized from the display layer's bounds and the OSD layer stretches
// it over its own bounds.
static CGPoint canvas_point_on_screen(CALayer *osd, CGSize canvas, CGPoint p)
{
    CGRect b = osd.bounds;
    CGPoint in_layer = CGPointMake(b.origin.x + p.x * b.size.width / canvas.width,
                                   b.origin.y + p.y * b.size.height / canvas.height);
    return [osd convertPoint:in_layer toLayer:osd.superlayer];
}

int main(void)
{
    @autoreleasepool {
        // 1. Viewer zoom, as Plezy applies it on iOS, tvOS and macOS: the
        //    display layer fills the container and is scaled about its centre
        //    (0.25x to 4x). The OSD layer must cover the unzoomed picture with
        //    the canvas mapped 1:1, so the bottom line of a two-line subtitle
        //    stays where mpv put it. The zoomed `frame` the VO used to copy
        //    stretches the canvas by the zoom and, zooming in, pushes that line
        //    below the container (plezy#2693: 120% on a 1000x461 point view).
        const CGFloat zooms[] = {0.25, 0.5, 1.2, 2.0, 4.0};
        for (size_t i = 0; i < sizeof(zooms) / sizeof(zooms[0]); i++) {
            CGFloat zoom = zooms[i];
            CALayer *container = [CALayer layer];
            container.bounds = CGRectMake(0, 0, 1000, 461);
            AVSampleBufferDisplayLayer *display = [[AVSampleBufferDisplayLayer alloc] init];
            display.bounds = container.bounds;
            display.position = CGPointMake(500, 230.5);
            [container addSublayer:display];
            display.transform = CATransform3DMakeScale(zoom, zoom, 1);

            CGRect placed = layer_untransformed_frame(display);
            AVSampleBufferDisplayLayer *osd = make_osd_layer(display, placed);
            AVSampleBufferDisplayLayer *old = make_osd_layer(display, display.frame);

            CGSize canvas = display.bounds.size;
            CGPoint line = CGPointMake(500, 425);
            CGPoint fixed_line = canvas_point_on_screen(osd, canvas, line);
            CGPoint old_line = canvas_point_on_screen(old, canvas, line);

            printf("zoom %.2fx:\n", zoom);
            print_rect("display frame (old copy)", display.frame);
            print_rect("osd frame", osd.frame);
            printf("  bottom line y=%.1f: osd %.1f, old copy %.1f (container %.0f)\n",
                   line.y, fixed_line.y, old_line.y, container.bounds.size.height);

            CHECK(!rect_near(display.frame, container.bounds),
                  "zoom %.2fx: the scenario must transform the display layer\n", zoom);
            CHECK(rect_near(osd.frame, container.bounds),
                  "zoom %.2fx: the OSD layer must cover the unzoomed picture\n", zoom);
            CHECK(near(osd.bounds.size.width, canvas.width) &&
                  near(osd.bounds.size.height, canvas.height),
                  "zoom %.2fx: the canvas must map 1:1 onto the OSD layer\n", zoom);
            CHECK(near(fixed_line.x, line.x) && near(fixed_line.y, line.y),
                  "zoom %.2fx: a subtitle must land where mpv laid it out\n", zoom);
            if (zoom > 1.0) {
                CHECK(old_line.y > container.bounds.size.height,
                      "zoom %.2fx: the copied frame must reproduce the off-screen line\n", zoom);
            }
        }

        // 2. No transform: the result is exactly the display layer's frame,
        //    the value the OSD layer was given before, for a display layer
        //    that does not fill its container (letterboxed host view).
        {
            CALayer *container = [CALayer layer];
            container.bounds = CGRectMake(0, 0, 1280, 720);
            AVSampleBufferDisplayLayer *display = [[AVSampleBufferDisplayLayer alloc] init];
            display.bounds = CGRectMake(0, 0, 960, 540);
            display.position = CGPointMake(520, 300);
            [container addSublayer:display];

            CGRect placed = layer_untransformed_frame(display);
            printf("identity:\n");
            print_rect("display frame", display.frame);
            print_rect("placed", placed);
            CHECK(rect_near(placed, display.frame),
                  "without a transform the OSD layer must get the display frame\n");
        }

        // 3. Any anchor point and bounds origin, under a transform that also
        //    translates: the frame is still the one the layer would have with
        //    the transform removed. The bounds origin scrolls sublayers and
        //    must not move the layer itself.
        {
            CALayer *container = [CALayer layer];
            container.bounds = CGRectMake(0, 0, 1280, 720);
            AVSampleBufferDisplayLayer *display = [[AVSampleBufferDisplayLayer alloc] init];
            display.anchorPoint = CGPointMake(0, 0);
            display.bounds = CGRectMake(25, 10, 800, 450);
            display.position = CGPointMake(40, 30);
            [container addSublayer:display];
            CGRect untransformed = display.frame;

            display.transform = CATransform3DTranslate(CATransform3DMakeScale(2, 2, 1), 30, -12, 0);
            CGRect placed = layer_untransformed_frame(display);
            printf("anchor (0,0), bounds origin (25,10), scale+translate:\n");
            print_rect("untransformed frame", untransformed);
            print_rect("display frame", display.frame);
            print_rect("placed", placed);
            CHECK(rect_near(untransformed, CGRectMake(40, 30, 800, 450)),
                  "the bounds origin must not move the layer\n");
            CHECK(!rect_near(display.frame, untransformed),
                  "the scenario must transform the display layer\n");
            CHECK(rect_near(placed, untransformed),
                  "the OSD layer must get the frame without the transform\n");
        }
    }

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }

    printf("\nall checks passed\n");
    return 0;
}
