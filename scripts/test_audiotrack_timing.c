// Drive the production clock/query/publication code with a deterministic JNI
// clock. A slow query must not move the audible deadline of an already-known
// block. In particular PCM timestamps describe the pre-query extrapolation,
// whereas the playback head and passthrough smoothing describe a later instant.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define MP_TIME_MS_TO_NS(v) ((int64_t)((v) * 1000000LL))
#define MP_TIME_S_TO_NS(v) ((int64_t)((v) * 1000000000.0))
#define MP_TIME_NS_TO_S(v) ((v) / 1000000000.0)
#define MP_ARRAY_SIZE(v) ((int)(sizeof(v) / sizeof((v)[0])))
#define MPMIN(a, b) ((a) < (b) ? (a) : (b))
#define MPCLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define MP_VERBOSE(...) ((void)0)

typedef void *jobject, *jbyteArray, *jshortArray, *jfloatArray;
typedef int jint, JNIEnv, mp_thread, mp_mutex, mp_cond;
struct ao { void *priv; int samplerate, sstride; };

static int64_t now_ns, state_delay_ns, head_delay_ns, latency_delay_ns;
// Deliberately different epochs: mixing Android nanoTime with mp_time_ns must
// fail even when both clocks advance at precisely the same rate.
static const int64_t raw_epoch = 987654321000LL;
static int hardware_rate, hidden_latency_ms;
static uint32_t frame_bias;
static bool zero_head;
static int64_t published_deadline;
static JNIEnv environment;

static int64_t mp_raw_time_ns(void) { return now_ns + raw_epoch; }
static int64_t mp_time_ns_from_raw_time(int64_t raw) { return raw - raw_epoch; }
static int64_t mp_time_ns(void) { return mp_time_ns_from_raw_time(mp_raw_time_ns()); }

static const struct {
    int getPlayState, getPlaybackHeadPosition, getLatency, getTimestamp;
    int PLAYSTATE_PLAYING;
} AudioTrack = {1, 2, 3, 4, 3};
static const struct { int ENCODING_IEC61937; } AudioFormat = {13};
static const struct { int framePosition, nanoTime; } AudioTimestamp = {1, 2};

static uint32_t hardware_head(void)
{
    return zero_head ? 0 : frame_bias + (uint32_t)(now_ns * hardware_rate / 1000000000LL);
}

static int64_t jni_int(JNIEnv *env, int method)
{
    (void)env;
    switch (method) {
    case 1:
        now_ns += state_delay_ns;
        return AudioTrack.PLAYSTATE_PLAYING;
    case 2:
        now_ns += head_delay_ns;
        return hardware_head();
    case 3:
        now_ns += latency_delay_ns;
        return hidden_latency_ms;
    default:
        abort();
    }
}

static int64_t jni_long(JNIEnv *env, int field)
{
    (void)env;
    return field == AudioTimestamp.framePosition ? hardware_rate / 2 : raw_epoch + 500000000;
}

#define MP_JNI_GET_ENV(ao) (&environment)
#define MP_JNI_CALL_INT(track, method) jni_int(env, method)
#define MP_JNI_CALL_BOOL(track, method, stamp) ((void)env, false)
#define MP_JNI_GET_LONG(stamp, field) jni_long(env, field)

#include "audiotrack_clock.inc"

static int ao_read_data(struct ao *ao, void **data, int samples, int64_t end,
                        void *pts, bool pad, bool block)
{
    (void)ao; (void)data; (void)pts; (void)pad; (void)block;
    published_deadline = end;
    return samples;
}

static void publish_block(struct ao *ao)
{
    struct priv *p = ao->priv;
#include "audiotrack_read.inc"
}

static void expect_deadline(const char *name, int64_t expected)
{
    if (llabs(published_deadline - expected) > 1000) {
        fprintf(stderr, "%s: audible block deadline moved by %.3f ms\n", name,
                (published_deadline - expected) / 1000000.0);
        exit(1);
    }
    printf("PASS: %s\n", name);
}

static void check_query_stall(const char *name, bool raw, bool iec, bool timestamp,
                              int multiplier, uint32_t bias, bool starting)
{
    now_ns = MP_TIME_S_TO_NS(1);
    hardware_rate = 48000;
    frame_bias = bias;
    zero_head = starting;
    hidden_latency_ms = raw || iec || timestamp ? 0 : 120;
    state_delay_ns = MP_TIME_MS_TO_NS(23);
    head_delay_ns = MP_TIME_MS_TO_NS(7);
    // A second blocking query must not replace the head sample's time.
    latency_delay_ns = MP_TIME_MS_TO_NS(70);
    published_deadline = 0;

    const int rate = hardware_rate * multiplier;
    struct priv p = {
        .audiotrack = &environment,
        .format = iec ? AudioFormat.ENCODING_IEC61937 : 1,
        .raw_passthrough = raw,
        .raw_rate_mult = multiplier,
        .timestamp_set = timestamp,
        .timestamp_stable = 20,
        .timestamp_fetched = mp_raw_time_ns(),
        .written_frames = hardware_head() * (uint32_t)multiplier + rate * 3 / 10,
        .chunksize = rate / 50 * 4,
    };
    struct ao ao = {.priv = &p, .samplerate = rate, .sstride = 4};
    // 300 ms already queued, followed by the next 20 ms block. The simulated
    // hardware continues playing while JNI stalls, so this deadline is fixed.
    int64_t expected = now_ns + MP_TIME_MS_TO_NS(320 + hidden_latency_ms);
    if (starting) // head still zero: playback has not begun during the query
        expected += state_delay_ns + head_delay_ns;
    publish_block(&ao);
    expect_deadline(name, expected);
}

int main(void)
{
    check_query_stall("raw AC3 stays on the media timeline across a slow query",
                      true, false, false, 1, 0, false);
    check_query_stall("IEC 61937 uses the smoothed sample instant",
                      false, true, false, 1, 0, false);
    check_query_stall("E-AC3 carrier scaling survives the 32-bit head wrap",
                      true, false, false, 4, UINT32_MAX - 48000, false);
    check_query_stall("PCM timestamp retains its extrapolation time and mpv epoch",
                      false, false, true, 1, 0, false);
    check_query_stall("PCM fallback retains head time across hidden-latency query",
                      false, false, false, 1, 0, false);
    check_query_stall("zero startup head still supplies a deadline reference",
                      true, false, false, 1, 0, true);
    return 0;
}
