// Link the real Swift/Rust sink, then feed IEC 61937 produced by FFmpeg.
// The clock stays stopped and the renderer is muted: this test emits no sound.
#import <AVFoundation/AVFoundation.h>
#include "audio/out/ao_spatial.h"

static void pump(double seconds)
{
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 4) return 2;
        int codec = atoi(argv[1]);
        bool expect_failure = atoi(argv[3]);
        NSData *bursts = [NSData dataWithContentsOfFile:@(argv[2])];
        if (!bursts.length) return 2;
        AVSampleBufferRenderSynchronizer *clock = [AVSampleBufferRenderSynchronizer new];
        void *sink = mp_spatial_create(codec, (__bridge void *)clock);
        if (!sink) return 3;
        mp_spatial_volume(sink, 0);
        for (int seek = 0; seek < 3; seek++) {
            mp_spatial_begin(sink, seek * INT64_C(1000000000));
            // Split across IEC headers and payloads, as mpv may do.
            for (size_t offset = 0; offset < bursts.length; offset += 8191) {
                size_t size = MIN(8191, bursts.length - offset);
                mp_spatial_write(sink, (const char *)bursts.bytes + offset, size);
            }
            mp_spatial_end(sink);
            NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:10];
            while (!mp_spatial_ready(sink) && !mp_spatial_failed(sink) && deadline.timeIntervalSinceNow > 0)
                pump(0.01);
            if (mp_spatial_failed(sink) != expect_failure || (!expect_failure && !mp_spatial_ready(sink))) {
                fprintf(stderr, "unexpected readiness/failure after timeline %d\n", seek);
                mp_spatial_destroy(sink);
                return 4;
            }
            if (expect_failure) break;
        }
        mp_spatial_destroy(sink);
        pump(0.2);
        // Closing before the main-actor activator setup runs must be safe.
        for (int iteration = 0; iteration < 20; iteration++) {
            sink = mp_spatial_create(codec, (__bridge void *)clock);
            if (!sink) return 5;
            mp_spatial_begin(sink, 0);
            mp_spatial_destroy(sink);
        }
        pump(0.5);
        puts("real renderer passed");
    }
    return 0;
}
