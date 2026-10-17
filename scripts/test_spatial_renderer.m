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
        void *sink = mp_spatial_create(codec, (__bridge void *)clock, 1 << 20);
        if (!sink) return 3;
        mp_spatial_volume(sink, 0);
        for (int seek = 0; seek < 3; seek++) {
            mp_spatial_begin(sink, seek * INT64_C(1000000000));
            bool ready = false, final = false;
            int64_t covered = INT64_MIN, end = INT64_MIN;
            int failure = MP_SPATIAL_OK;
            // Split across IEC headers and payloads, as mpv may do, and offer
            // again what the bounded buffer did not take yet.
            NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:30];
            for (size_t offset = 0; offset < bursts.length && failure == MP_SPATIAL_OK;) {
                size_t size = MIN(8191, bursts.length - offset);
                size_t taken = mp_spatial_write(sink, (const char *)bursts.bytes + offset, size);
                offset += taken;
                failure = mp_spatial_poll(sink, &ready, &covered, &end, &final);
                if (!taken) {
                    if (deadline.timeIntervalSinceNow < 0) {
                        fprintf(stderr, "the sink stopped taking input\n");
                        mp_spatial_destroy(sink);
                        return 6;
                    }
                    pump(0.01);
                }
            }
            mp_spatial_end(sink);
            deadline = [NSDate dateWithTimeIntervalSinceNow:10];
            while (!final && failure == MP_SPATIAL_OK && deadline.timeIntervalSinceNow > 0) {
                pump(0.01);
                failure = mp_spatial_poll(sink, &ready, &covered, &end, &final);
            }
            if ((failure != MP_SPATIAL_OK) != expect_failure || (!expect_failure && (!ready || !final))) {
                char *reason = mp_spatial_failure(sink);
                fprintf(stderr, "unexpected readiness/failure after timeline %d: %s\n", seek, reason ? reason : "none");
                free(reason);
                mp_spatial_destroy(sink);
                return 4;
            }
            if (expect_failure) break;
            printf("timeline %d: enqueued to %.3f s\n", seek, end / 1e9);
        }
        mp_spatial_destroy(sink);
        pump(0.2);
        // Closing before the main-actor activator setup runs must be safe.
        for (int iteration = 0; iteration < 20; iteration++) {
            sink = mp_spatial_create(codec, (__bridge void *)clock, 1 << 20);
            if (!sink) return 5;
            mp_spatial_begin(sink, 0);
            mp_spatial_destroy(sink);
        }
        pump(0.5);
        puts("real renderer passed");
    }
    return 0;
}
