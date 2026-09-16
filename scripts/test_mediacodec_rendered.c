// Behavioral regressions for the production rendered-frame feedback ring
// extracted by test_mediacodec_rendered.sh: the codec-thread producer, the
// consumer av_mediacodec_drain_rendered hands the VO, and the source string.
// The ring is the only thing under test; the codec context is a fake carrying
// its fields.
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AVERROR(e) (-(e))
#define FF_MEDIACODEC_RENDERED_RING 64

typedef struct AVMediaCodecRendered {
    int64_t media_time_us;
    int64_t system_nano;
} AVMediaCodecRendered;

typedef struct MediaCodecDecContext {
    const char *rendered_source;
    AVMediaCodecRendered rendered[FF_MEDIACODEC_RENDERED_RING];
    atomic_uint rendered_head;
    atomic_uint rendered_tail;
} MediaCodecDecContext;

typedef struct MediaCodecBuffer {
    MediaCodecDecContext *ctx;
} AVMediaCodecBuffer;

#include "mediacodec_rendered.inc"

static void check(bool ok, const char *scenario)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", scenario);
        exit(1);
    }
}

static MediaCodecDecContext ctx;
static AVMediaCodecBuffer buffer = { &ctx };

static void reset(const char *source)
{
    memset(&ctx, 0, sizeof(ctx));
    ctx.rendered_source = source;
}

static void test_reports_drain_in_delivery_order(void)
{
    AVMediaCodecRendered out[8];
    reset("ndk");
    check(av_mediacodec_drain_rendered(&buffer, out, 8) == 0,
          "an idle ring drains nothing");
    for (int n = 0; n < 5; n++)
        check(mediacodec_dec_rendered_push(&ctx, 1000 * n, 2000 * n) == 1,
              "a report fits while the ring has room");
    check(av_mediacodec_drain_rendered(&buffer, out, 3) == 3 &&
          out[0].media_time_us == 0 && out[2].media_time_us == 2000 &&
          out[2].system_nano == 4000,
          "a bounded drain returns the oldest reports first");
    check(av_mediacodec_drain_rendered(&buffer, out, 8) == 2 &&
          out[0].media_time_us == 3000 && out[1].media_time_us == 4000,
          "the next drain continues where the bounded one stopped");
    check(av_mediacodec_drain_rendered(&buffer, out, 8) == 0,
          "a drained ring is empty");
    check(strcmp(av_mediacodec_rendered_source(&buffer), "ndk") == 0,
          "the source names the registered callback");
}

static void test_full_ring_drops_the_newest(void)
{
    AVMediaCodecRendered out[FF_MEDIACODEC_RENDERED_RING + 8];
    reset("ndk");
    for (int n = 0; n < FF_MEDIACODEC_RENDERED_RING + 3; n++)
        mediacodec_dec_rendered_push(&ctx, n, n);
    int n = av_mediacodec_drain_rendered(&buffer, out, FF_MEDIACODEC_RENDERED_RING + 8);
    check(n == FF_MEDIACODEC_RENDERED_RING &&
          out[0].media_time_us == 0 &&
          out[FF_MEDIACODEC_RENDERED_RING - 1].media_time_us == FF_MEDIACODEC_RENDERED_RING - 1,
          "the ring keeps the oldest reports, so a stalled consumer still sees what came first");
    check(mediacodec_dec_rendered_push(&ctx, 500, 500) == 1 &&
          av_mediacodec_drain_rendered(&buffer, out, 8) == 1 && out[0].media_time_us == 500,
          "the ring wraps and accepts reports again once drained");
}

static void test_feedback_off_is_enosys(void)
{
    AVMediaCodecRendered out[4];
    reset("off:java");
    mediacodec_dec_rendered_push(&ctx, 1, 1);
    check(av_mediacodec_drain_rendered(&buffer, out, 4) == AVERROR(ENOSYS),
          "a codec without feedback answers ENOSYS, whatever the ring holds");
    check(strcmp(av_mediacodec_rendered_source(&buffer), "off:java") == 0,
          "the source explains why feedback is off");
    reset("ndk");
    check(av_mediacodec_drain_rendered(&buffer, out, -1) == AVERROR(EINVAL),
          "a negative capacity is rejected");
}

// One producer racing one consumer across many wraps: every report drained is
// one that was pushed, in push order, with none repeated. The producer
// retries a full ring, which the production callback never does; the retry
// only keeps the sequence dense so gaps would be visible.
static void *producer(void *arg)
{
    for (int n = 0; n < 20000; n++) {
        while (!mediacodec_dec_rendered_push(&ctx, n, n))
            sched_yield();
    }
    return arg;
}

static void test_single_producer_single_consumer_is_lossless_when_drained(void)
{
    AVMediaCodecRendered out[16];
    pthread_t thread;
    reset("ndk");
    check(pthread_create(&thread, NULL, producer, NULL) == 0, "producer thread starts");
    int64_t expect = 0;
    while (expect < 20000) {
        int n = av_mediacodec_drain_rendered(&buffer, out, 16);
        for (int i = 0; i < n; i++) {
            check(out[i].media_time_us == expect && out[i].system_nano == expect,
                  "reports drain in push order without duplicates or gaps");
            expect++;
        }
        if (!n)
            sched_yield();
    }
    pthread_join(thread, NULL);
    check(av_mediacodec_drain_rendered(&buffer, out, 16) == 0,
          "nothing is left once every pushed report has drained");
}

int main(void)
{
    test_reports_drain_in_delivery_order();
    test_full_ring_drops_the_newest();
    test_feedback_off_is_enosys();
    test_single_producer_single_consumer_is_lossless_when_drained();
    puts("PASS: rendered-frame reports drain in order, a full ring drops the "
         "newest, and a codec without feedback answers ENOSYS");
    return 0;
}
