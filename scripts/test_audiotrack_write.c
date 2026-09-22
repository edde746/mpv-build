// Compile the production worker, JNI write boundary, reset/recreate and clock.
// The injected device can ignore pause while write is gated; it reads the live
// buffer again at completion and enforces JNI reference lifetime throughout.
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define MP_TIME_MS_TO_NS(v) ((int64_t)((v) * 1000000LL))
#define MP_TIME_S_TO_NS(v) ((int64_t)((v) * 1000000000.0))
#define MP_TIME_NS_TO_S(v) ((v) / 1000000000.0)
#define MP_ARRAY_SIZE(v) ((int)(sizeof(v) / sizeof((v)[0])))
#define MPMIN(a, b) ((a) < (b) ? (a) : (b))
#define MPMAX(a, b) ((a) > (b) ? (a) : (b))
#define MPCLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define MP_THREAD_VOID void *
#define MP_THREAD_RETURN() return NULL
#define AF_FORMAT_S_DTS 99
#define AF_FORMAT_S_DTSHD 98

typedef pthread_t mp_thread;
typedef pthread_mutex_t mp_mutex;
typedef pthread_cond_t mp_cond;
typedef int jint, jmethodID;
struct object {
    bool track, released;
    int refs, state, position, underruns;
    uint8_t *data;
    uint8_t sink[256];
    int sink_size;
    // Simulated HAL counters. head is what getPlaybackHeadPosition reports;
    // head_resets models whether flush() zeroes it (false = the Xiaomi-style
    // HAL that keeps the render position across a flush). ts_fpos/ts_nanos
    // are what getTimestamp publishes while timestamp_ok.
    uint32_t head;
    bool head_resets;
    uint32_t ts_fpos;
    int64_t ts_nanos;
    bool timestamp_ok;
};
typedef struct object *jobject, *jbyteArray, *jshortArray, *jfloatArray;
struct jni;
typedef const struct jni *JNIEnv;
struct jni {
    jobject (*NewLocalRef)(JNIEnv *, jobject);
    void (*SetShortArrayRegion)(JNIEnv *, jobject, int, int, const void *);
    void (*SetFloatArrayRegion)(JNIEnv *, jobject, int, int, const void *);
    void (*SetByteArrayRegion)(JNIEnv *, jobject, int, int, const void *);
};
struct ao { void *priv; int samplerate, sstride, format; };

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static atomic_llong now_ns;
static int entered, permitted, idle, head_queries, pauses, failures, warnings;
static int underrun_queries;
static char last_warning[256];
static int response, input_bytes;
static bool pause_after_write;
static uint8_t input[256];
static struct object tracks[16];
static int track_count;

static void mp_mutex_lock(mp_mutex *m) { assert(!pthread_mutex_lock(m)); }
static void mp_mutex_unlock(mp_mutex *m) { assert(!pthread_mutex_unlock(m)); }
static void mp_cond_signal(mp_cond *c) { assert(!pthread_cond_signal(c)); }
static void mp_thread_set_name(const char *name) { (void)name; }
#define mp_strerror(e) strerror(e)
static int64_t mp_time_ns(void) { return atomic_load(&now_ns); }
static int64_t mp_raw_time_ns(void) { return mp_time_ns(); }
static int64_t mp_time_ns_from_raw_time(int64_t ns) { return ns; }
// Verbose lines are discarded except the playhead-step diagnostic, which the
// step check reads back.
static int step_lines;
static char last_step_line[512];
static void test_log(const char *fmt, ...)
{
    if (strncmp(fmt, "playhead step:", 14))
        return;
    va_list args;
    va_start(args, fmt);
    mp_mutex_lock(&gate);
    vsnprintf(last_step_line, sizeof(last_step_line), fmt, args);
    step_lines++;
    mp_mutex_unlock(&gate);
    va_end(args);
}
static void warn_log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    mp_mutex_lock(&gate);
    vsnprintf(last_warning, sizeof(last_warning), fmt, args);
    warnings++;
    pthread_cond_broadcast(&changed);
    mp_mutex_unlock(&gate);
    va_end(args);
}
#define MP_VERBOSE(ao, ...) test_log(__VA_ARGS__)
#define MP_WARN(ao, ...) warn_log(__VA_ARGS__)
#define MP_ERR(ao, ...) test_log(__VA_ARGS__)

static void mp_cond_timedwait(mp_cond *c, mp_mutex *m, int64_t ns)
{
    // The worker publishes its completed accounting before entering this wait.
    mp_mutex_lock(&gate);
    idle++;
    pthread_cond_broadcast(&changed);
    mp_mutex_unlock(&gate);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ns / 1000000000;
    until.tv_nsec += ns % 1000000000;
    until.tv_sec += until.tv_nsec / 1000000000;
    until.tv_nsec %= 1000000000;
    pthread_cond_timedwait(c, m, &until);
}

static struct {
    int writeBufferV21, writeShortV23, writeFloat, write;
    int getPlayState, getPlaybackHeadPosition, getLatency, getTimestamp;
    int release, pause, flush, play, getUnderrunCount;
    int PLAYSTATE_PLAYING, PLAYSTATE_PAUSED, WRITE_BLOCKING;
} AudioTrack = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 3, 2, 0};
static const struct { int ENCODING_IEC61937, ENCODING_PCM_FLOAT; }
    AudioFormat = {13, 4};
static const struct { int ERROR_DEAD_OBJECT; } AudioManager = {-6};
static const struct { int clear; } ByteBuffer = {1};
static const struct { int framePosition, nanoTime; } AudioTimestamp = {1, 2};

static jobject local_ref(JNIEnv *env, jobject obj)
{
    (void)env;
    mp_mutex_lock(&gate);
    assert(obj && obj->refs > 0);
    obj->refs++;
    mp_mutex_unlock(&gate);
    return obj;
}

static void drop_ref(JNIEnv *env, jobject *obj)
{
    (void)env;
    mp_mutex_lock(&gate);
    if (*obj) {
        assert((*obj)->refs > 0);
        (*obj)->refs--;
        *obj = NULL;
    }
    mp_mutex_unlock(&gate);
}

static void array_region(JNIEnv *env, jobject array, int offset, int len,
                         const void *data, int unit)
{
    (void)env;
    assert(array->refs > 0);
    memcpy(array->data + offset * unit, data, len * unit);
}
static void short_region(JNIEnv *e, jobject a, int o, int n, const void *d)
{ array_region(e, a, o, n, d, 2); }
static void float_region(JNIEnv *e, jobject a, int o, int n, const void *d)
{ array_region(e, a, o, n, d, sizeof(float)); }
static void byte_region(JNIEnv *e, jobject a, int o, int n, const void *d)
{ array_region(e, a, o, n, d, 1); }
static const struct jni jni_table = {local_ref, short_region, float_region, byte_region};
static JNIEnv environment = &jni_table;

static jobject jni_clear(JNIEnv *env, jobject buffer, int method)
{
    assert(method == ByteBuffer.clear);
    buffer->position = 0;
    return local_ref(env, buffer);
}

static jint jni_int(JNIEnv *env, jobject track, int method, ...)
{
    (void)env;
    mp_mutex_lock(&gate);
    assert(track && track->track && track->refs > 0);
    if (method == AudioTrack.getPlayState) {
        int state = track->state;
        mp_mutex_unlock(&gate);
        return state;
    }
    if (method == AudioTrack.getUnderrunCount) {
        int count = track->underruns;
        underrun_queries++;
        pthread_cond_broadcast(&changed);
        mp_mutex_unlock(&gate);
        return count;
    }
    if (method == AudioTrack.getPlaybackHeadPosition || method == AudioTrack.getLatency) {
        if (method == AudioTrack.getPlaybackHeadPosition) {
            head_queries++;
            pthread_cond_broadcast(&changed);
        }
        int head = method == AudioTrack.getPlaybackHeadPosition ? track->head : 0;
        mp_mutex_unlock(&gate);
        return head;
    }
    va_list args;
    va_start(args, method);
    jobject buffer = va_arg(args, jobject);
    int count = va_arg(args, int);
    if (method != AudioTrack.writeBufferV21) {
        assert(count == 0); // array offset
        count = va_arg(args, int);
    }
    va_end(args);
    int unit = method == AudioTrack.writeShortV23 ? 2 :
               method == AudioTrack.writeFloat ? (int)sizeof(float) : 1;
    int offset = method == AudioTrack.writeBufferV21 ? buffer->position : 0;
    assert(count * unit <= 256 && buffer->refs > 0);
    uint8_t snapshot[256];
    memcpy(snapshot, buffer->data + offset, count * unit);
    int call = ++entered;
    pthread_cond_broadcast(&changed);
    while (permitted < call)
        pthread_cond_wait(&changed, &gate);
    // A deleted global is fine only while the writer retains a local track.
    // Reset/recreate must not reuse/free the bytes a blocked HAL still reads.
    assert(track->refs > 0 && buffer->refs > 0);
    assert(!memcmp(snapshot, buffer->data + offset, count * unit));
    int result = response;
    assert(result <= count);
    if (result > 0) {
        int bytes = result * unit;
        assert(track->sink_size + bytes <= (int)sizeof(track->sink));
        memcpy(track->sink + track->sink_size, buffer->data + offset, bytes);
        track->sink_size += bytes;
        if (method == AudioTrack.writeBufferV21)
            buffer->position += bytes;
    }
    if (pause_after_write)
        track->state = AudioTrack.PLAYSTATE_PAUSED;
    mp_mutex_unlock(&gate);
    return result;
}

static void jni_void(JNIEnv *env, jobject track, int method)
{
    (void)env;
    mp_mutex_lock(&gate);
    assert(track && track->refs > 0);
    if (method == AudioTrack.release) {
        track->released = true;
        track->state = AudioTrack.PLAYSTATE_PAUSED;
    } else if (method == AudioTrack.pause) {
        track->state = AudioTrack.PLAYSTATE_PAUSED;
        pauses++;
    } else if (method == AudioTrack.play) {
        track->state = AudioTrack.PLAYSTATE_PLAYING;
    } else {
        assert(method == AudioTrack.flush);
        // A healthy HAL zeroes its counters on flush; a broken one keeps them.
        if (track->head_resets) {
            track->head = 0;
            track->ts_fpos = 0;
        }
    }
    // pause/flush deliberately do not complete the outstanding write.
    pthread_cond_broadcast(&changed);
    mp_mutex_unlock(&gate);
}

#define MP_JNI_GET_ENV(ao) (&environment)
#define MP_JNI_CALL_INT(obj, method, ...) jni_int(env, obj, method, ##__VA_ARGS__)
#define MP_JNI_CALL_VOID(obj, method) jni_void(env, obj, method)
#define MP_JNI_CALL_OBJECT(obj, method) jni_clear(env, obj, method)
#define MP_JNI_LOCAL_FREEP(obj) drop_ref(env, obj)
#define MP_JNI_GLOBAL_FREEP(obj) drop_ref(env, obj)
#define MP_JNI_EXCEPTION_LOG(ao) ((void)(ao), 0)
#define MP_JNI_CALL_BOOL(obj, method, stamp) jni_bool(env, obj, method, stamp)
#define MP_JNI_GET_LONG(obj, field) jni_long(env, obj, field)

static bool jni_bool(JNIEnv *env, jobject track, int method, jobject stamp)
{
    (void)env;
    assert(method == AudioTrack.getTimestamp);
    mp_mutex_lock(&gate);
    assert(track && track->track && stamp);
    stamp->ts_fpos = track->ts_fpos;
    stamp->ts_nanos = track->ts_nanos;
    bool ok = track->timestamp_ok;
    mp_mutex_unlock(&gate);
    return ok;
}

static int64_t jni_long(JNIEnv *env, jobject stamp, int field)
{
    (void)env;
    return field == AudioTimestamp.framePosition ? stamp->ts_fpos : stamp->ts_nanos;
}
static int AudioTrack_New(struct ao *ao);
static void AudioTrack_recreateOrFail(struct ao *ao);
static void AudioTrack_checkRouteChange(struct ao *ao) { (void)ao; }
static void ao_request_failure(struct ao *ao) { (void)ao; failures++; }
static int ao_read_data(struct ao *, void **, int, int64_t, void *, bool, bool);
#include "audiotrack_write.inc"

static int AudioTrack_New(struct ao *ao)
{
    struct priv *p = ao->priv;
    mp_mutex_lock(&gate);
    assert(track_count < (int)MP_ARRAY_SIZE(tracks));
    jobject track = &tracks[track_count++];
    *track = (struct object){.track = true, .refs = 1,
                             .state = AudioTrack.PLAYSTATE_PAUSED,
                             .head_resets = true};
    p->audiotrack = track;
    mp_mutex_unlock(&gate);
    return 0;
}

static int ao_read_data(struct ao *ao, void **data, int samples, int64_t end,
                        void *pts, bool pad, bool block)
{
    (void)end; (void)pts; (void)pad; (void)block;
    mp_mutex_lock(&gate);
    assert(input_bytes <= samples * ao->sstride);
    memcpy(*data, input, input_bytes);
    int n = input_bytes / ao->sstride;
    input_bytes = 0;
    mp_mutex_unlock(&gate);
    return n;
}

struct fixture {
    struct ao ao;
    struct priv p;
    struct object buffer, stamp;
    uint8_t chunk[256], backing[264];
};

static void setup(struct fixture *f, bool raw, int format, bool direct)
{
    memset(f, 0, sizeof(*f));
    entered = permitted = idle = head_queries = pauses = failures = warnings = 0;
    response = input_bytes = track_count = underrun_queries = step_lines = 0;
    pause_after_write = true;
    atomic_store(&now_ns, MP_TIME_S_TO_NS(1));
    f->buffer = (struct object){.refs = 1,
        .data = direct && !raw ? f->chunk : f->backing};
    AudioTrack.writeBufferV21 = direct ? 1 : 0;
    f->ao = (struct ao){.priv = &f->p, .samplerate = 48000, .sstride = 2};
    f->p.raw_passthrough = raw;
    f->p.raw_rate_mult = 1;
    f->p.format = format;
    f->p.chunksize = sizeof(f->chunk);
    f->p.size = sizeof(f->chunk);
    f->p.chunk = f->chunk;
    f->p.rawbuf = f->backing;
    f->p.bbuf = f->p.shortarray = f->p.floatarray = f->p.bytearray = &f->buffer;
    f->stamp = (struct object){.refs = 1};
    f->p.timestamp = &f->stamp;
    pthread_mutex_init(&f->p.lock, NULL);
    pthread_mutex_init(&f->p.track_lock, NULL);
    pthread_mutex_init(&f->p.monitor_lock, NULL);
    pthread_cond_init(&f->p.wakeup, NULL);
    pthread_cond_init(&f->p.monitor_wakeup, NULL);
    assert(!AudioTrack_New(&f->ao));
    AudioTrack_resetClock(&f->p);
    assert(!pthread_create(&f->p.thread, NULL, ao_thread, &f->ao));
}

static void wait_counter(int *counter, int target)
{
    mp_mutex_lock(&gate);
    while (*counter < target)
        pthread_cond_wait(&changed, &gate);
    mp_mutex_unlock(&gate);
}

static void begin_write(struct fixture *f, const uint8_t *bytes, int len)
{
    mp_mutex_lock(&gate);
    int target = entered + 1;
    if (len)
        memcpy(input, bytes, len);
    input_bytes = len;
    mp_mutex_unlock(&gate);
    start(&f->ao);
    wait_counter(&entered, target);
}

// A track left PLAYING parks the worker inside a zero-length JNI write that
// teardown cannot reach; pause it and release the blocked call first.
static void park_worker(struct fixture *f)
{
    mp_mutex_lock(&gate);
    for (int n = 0; n < track_count; n++)
        tracks[n].state = AudioTrack.PLAYSTATE_PAUSED;
    permitted = entered;
    pthread_cond_broadcast(&changed);
    mp_mutex_unlock(&gate);
    mp_mutex_lock(&f->p.lock);
    mp_mutex_unlock(&f->p.lock);
}

static void finish_write(struct fixture *f, int result)
{
    mp_mutex_lock(&gate);
    int target = idle + 1;
    response = result;
    permitted = entered;
    pthread_cond_broadcast(&changed);
    mp_mutex_unlock(&gate);
    wait_counter(&idle, target);
    // Wait until the worker has published its completion and dropped its lock.
    mp_mutex_lock(&f->p.lock);
    mp_mutex_unlock(&f->p.lock);
}

static void expect_delay_played(struct fixture *f, uint32_t written,
                                uint32_t played)
{
    mp_mutex_lock(&f->p.lock);
    int64_t sample_time = 0;
    double delay = AudioTrack_getLatency(&f->ao, &sample_time);
    assert(f->p.written_frames == written);
    assert(fabs(delay - (written - played) / 48000.0) < 1e-12);
    mp_mutex_unlock(&f->p.lock);
}

static void expect_delay(struct fixture *f, uint32_t frames)
{
    expect_delay_played(f, frames, 0);
}

static void teardown(struct fixture *f)
{
    atomic_store(&f->p.thread_terminate, true);
    atomic_store(&f->p.monitor_terminate, true);
    mp_mutex_lock(&f->p.lock);
    mp_cond_signal(&f->p.wakeup);
    mp_mutex_unlock(&f->p.lock);
    assert(!pthread_join(f->p.thread, NULL));
    if (f->p.monitor_created) {
        mp_mutex_lock(&f->p.monitor_lock);
        mp_cond_signal(&f->p.monitor_wakeup);
        mp_mutex_unlock(&f->p.monitor_lock);
        assert(!pthread_join(f->p.monitor, NULL));
    }
    JNIEnv *env = MP_JNI_GET_ENV(&f->ao);
    MP_JNI_GLOBAL_FREEP(&f->p.audiotrack);
    for (int n = 0; n < track_count; n++)
        assert(tracks[n].refs == 0);
    assert(f->buffer.refs == 1);
    pthread_cond_destroy(&f->p.wakeup);
    pthread_cond_destroy(&f->p.monitor_wakeup);
    pthread_mutex_destroy(&f->p.lock);
    pthread_mutex_destroy(&f->p.track_lock);
    pthread_mutex_destroy(&f->p.monitor_lock);
}

static const uint8_t burst[] = {
    0x72, 0xf8, 0x1f, 0x4e, 0x01, 0, 48, 0,
    0x77, 0x0b, 0x34, 0x12, 0x78, 0x56,
};
static const uint8_t payload[] = {0x0b, 0x77, 0x12, 0x34, 0x56, 0x78};

static void check_raw_partial_reset(void)
{
    struct fixture f;
    setup(&f, true, 5, true);
    begin_write(&f, burst, sizeof(burst));
    finish_write(&f, 2);
    expect_delay(&f, 0); // no carrier credit until the entire raw chunk drains
    begin_write(&f, NULL, 0);
    finish_write(&f, 0);
    expect_delay(&f, 0);
    begin_write(&f, NULL, 0);
    finish_write(&f, 4);
    expect_delay(&f, sizeof(burst) / 2);
    assert(tracks[0].sink_size == sizeof(payload));
    assert(!memcmp(tracks[0].sink, payload, sizeof(payload)));

    // Reset with both an unwritten raw tail and a parser awaiting more IEC
    // payload; the next generation must start at a fresh burst preamble.
    begin_write(&f, burst, sizeof(burst) - 2);
    finish_write(&f, 2);
    begin_write(&f, NULL, 0); // raw tail still belongs to this blocked JNI call
    stop(&f.ao);             // must complete BEFORE the write gate is released
    expect_delay(&f, 0);
    mp_mutex_lock(&f.p.lock);
    assert(!AudioTrack_Recreate(&f.ao)); // replacement while old completion is gated
    mp_mutex_unlock(&f.p.lock);
    assert(tracks[0].released && tracks[0].refs == 1);
    finish_write(&f, 2);
    expect_delay(&f, 0);
    assert(tracks[0].refs == 0 && !f.p.raw_pending);
    assert(!atomic_load(&f.p.write_outstanding));
    assert(atomic_load(&f.p.recovery_attempts) == 0);

    begin_write(&f, burst, sizeof(burst));
    finish_write(&f, sizeof(payload));
    expect_delay(&f, sizeof(burst) / 2);
    assert(tracks[1].sink_size == sizeof(payload));
    assert(!memcmp(tracks[1].sink, payload, sizeof(payload)));
    teardown(&f);
    puts("PASS: partial raw writes survive stop/reset/recreate and late completion");
}

// One DTS type IV burst as libavformat's spdif muxer emits it: LE preamble
// words, then the byte-swapped payload (10-byte start code, big-endian packet
// size, the packet) padded to Pd, then carrier zeros up to the next burst.
// The packet is a DTS core header whose SFREQ code is `sfreq`, plus a byte.
static int dtshd_burst(uint8_t *out, int sfreq)
{
    static const uint8_t start_code[10] = {1, 0, 0, 0, 0, 0, 0, 0, 0xfe, 0xfe};
    uint8_t packet[10] = {0x7f, 0xfe, 0x80, 0x01, 0x3f, 0xc0, 0x40, 0x00, sfreq << 2, 0xab};
    uint8_t payload[24] = {0}; // 12 + 10 packet bytes, then 2 bytes of Pd padding
    memcpy(payload, start_code, sizeof(start_code));
    payload[10] = 0;
    payload[11] = sizeof(packet);
    memcpy(payload + 12, packet, sizeof(packet));
    const uint16_t preamble[4] = {0xf872, 0x4e1f, 0x11, sizeof(payload)};
    int n = 0;
    for (int i = 0; i < 4; i++) {
        out[n++] = preamble[i] & 0xff;
        out[n++] = preamble[i] >> 8;
    }
    for (int i = 0; i < (int)sizeof(payload); i += 2) {
        out[n++] = payload[i + 1];
        out[n++] = payload[i];
    }
    for (int i = 0; i < 6; i++)
        out[n++] = 0;
    return n;
}

static void check_raw_dtshd(int sfreq, int expected_rate, int expected_tracks)
{
    struct fixture f;
    setup(&f, true, 5, true);
    f.ao.format = AF_FORMAT_S_DTSHD;
    f.ao.samplerate = 192000;
    f.p.samplerate = 48000;
    f.p.raw_rate_mult = 4;
    uint8_t stream[64];
    const int len = dtshd_burst(stream, sfreq);
    const uint8_t packet[10] = {0x7f, 0xfe, 0x80, 0x01, 0x3f, 0xc0, 0x40, 0x00, sfreq << 2, 0xab};

    begin_write(&f, stream, len);
    finish_write(&f, sizeof(packet));
    // The whole burst - preamble, header, packet and padding - is credited
    // once the packet alone has drained, on the track the core rate chose.
    mp_mutex_lock(&f.p.lock);
    assert(f.p.written_frames == (uint32_t)(len / f.ao.sstride));
    assert(f.p.raw_rate_verified);
    assert(f.p.samplerate == expected_rate);
    assert(f.p.raw_rate_mult == 192000 / expected_rate);
    mp_mutex_unlock(&f.p.lock);
    assert(track_count == expected_tracks && !failures);
    struct object *track = &tracks[track_count - 1];
    assert(track->sink_size == (int)sizeof(packet));
    assert(!memcmp(track->sink, packet, sizeof(packet)));

    // A later burst goes straight to the same track without another probe.
    begin_write(&f, stream, len);
    finish_write(&f, sizeof(packet));
    assert(track_count == expected_tracks);
    assert(track->sink_size == 2 * (int)sizeof(packet));
    assert(!memcmp(track->sink + sizeof(packet), packet, sizeof(packet)));
    teardown(&f);
}

// The parser keeps its place across arbitrarily fragmented chunks: fed one
// byte at a time, two bursts still unwrap to exactly two bare packets.
static void check_dtshd_unwrap_fragmented(void)
{
    struct priv p = {0};
    struct ao ao = {.priv = &p, .samplerate = 192000, .sstride = 16,
                    .format = AF_FORMAT_S_DTSHD};
    uint8_t stream[128];
    const int len = dtshd_burst(stream, 13);
    memcpy(stream + len, stream, len);
    uint8_t out[64];
    int n = 0;
    for (int i = 0; i < 2 * len; i++)
        n += AudioTrack_unwrapIEC61937(&ao, out + n, stream + i, 1);
    const uint8_t packet[10] = {0x7f, 0xfe, 0x80, 0x01, 0x3f, 0xc0, 0x40, 0x00, 13 << 2, 0xab};
    assert(n == 2 * (int)sizeof(packet));
    assert(!memcmp(out, packet, sizeof(packet)));
    assert(!memcmp(out + sizeof(packet), packet, sizeof(packet)));
    assert(p.raw_state == RAW_SYNC_PA && !warnings);
}

static void check_pcm_and_iec(int format, bool direct, int returned, int bytes)
{
    struct fixture f;
    setup(&f, false, format, direct);
    const uint8_t data[16] = {1, 2, 3, 4, 5, 6, 7, 8};
    begin_write(&f, data, sizeof(data));
    finish_write(&f, returned);
    expect_delay(&f, bytes / f.ao.sstride);
    assert(tracks[0].sink_size == bytes);
    assert(!memcmp(tracks[0].sink, data, bytes));
    begin_write(&f, data, sizeof(data));
    stop(&f.ao);
    expect_delay(&f, 0);
    finish_write(&f, returned);
    expect_delay(&f, 0);
    teardown(&f);
}

static void check_dead_object(void)
{
    struct fixture f;
    setup(&f, true, 5, true);
    begin_write(&f, burst, sizeof(burst));
    stop(&f.ao);
    finish_write(&f, AudioManager.ERROR_DEAD_OBJECT);
    assert(track_count == 1 && !failures);
    assert(!atomic_load(&f.p.recovery_attempts));
    expect_delay(&f, 0);

    // A current-generation error still recreates, clears the raw parser/clock,
    // and consumes the existing watchdog budget exactly once.
    begin_write(&f, burst, sizeof(burst));
    atomic_store(&f.p.play_requested, false);
    finish_write(&f, AudioManager.ERROR_DEAD_OBJECT);
    assert(track_count == 2 && !failures);
    assert(atomic_load(&f.p.recovery_attempts) == 1);
    expect_delay(&f, 0);
    begin_write(&f, burst, sizeof(burst));
    finish_write(&f, AudioManager.ERROR_DEAD_OBJECT);
    assert(track_count == 2 && failures == 1 && atomic_load(&f.p.failed));
    teardown(&f);
    puts("PASS: stale ERROR_DEAD_OBJECT is ignored; live errors retain recovery budget");
}

// One monitor sample, then the verdict it produced: the monitor logs after
// the counter query that fed it, so the state after query N is settled once
// query N+1 has been observed. Only the monitor reads the counter, so a
// worker spinning on a playing track cannot satisfy the wait.
static void sample_monitor(struct fixture *f)
{
    mp_mutex_lock(&gate);
    int target = underrun_queries + 2;
    mp_mutex_unlock(&gate);
    mp_cond_signal(&f->p.monitor_wakeup);
    wait_counter(&underrun_queries, target);
}

static void check_underrun_reporting(void)
{
    struct fixture f;
    setup(&f, true, 5, true);
    atomic_store(&f.p.play_requested, true); // the core wants audio; no write in flight
    assert(!pthread_create(&f.p.monitor, NULL, monitor_thread, &f.ao));
    f.p.monitor_created = true;

    sample_monitor(&f);
    assert(warnings == 0);

    mp_mutex_lock(&gate);
    tracks[0].underruns = 2;
    mp_mutex_unlock(&gate);
    sample_monitor(&f);
    assert(warnings == 1);
    assert(strstr(last_warning, "+2, 2 on this track"));

    // Within the same second: counted, not logged.
    mp_mutex_lock(&gate);
    tracks[0].underruns = 3;
    mp_mutex_unlock(&gate);
    sample_monitor(&f);
    assert(warnings == 1);

    atomic_fetch_add(&now_ns, MP_TIME_S_TO_NS(1));
    mp_mutex_lock(&gate);
    tracks[0].underruns = 4;
    mp_mutex_unlock(&gate);
    sample_monitor(&f);
    assert(warnings == 2);
    assert(strstr(last_warning, "+1, 4 on this track"));

    // A replacement track counts from zero; its first sample is not a drop.
    mp_mutex_lock(&f.p.lock);
    assert(!AudioTrack_Recreate(&f.ao));
    mp_mutex_unlock(&f.p.lock);
    mp_mutex_lock(&gate);
    tracks[1].state = AudioTrack.PLAYSTATE_PAUSED; // recreate resumed it; idle the worker
    mp_mutex_unlock(&gate);
    atomic_fetch_add(&now_ns, MP_TIME_S_TO_NS(1));
    sample_monitor(&f);
    assert(warnings == 2);
    mp_mutex_lock(&gate);
    tracks[1].underruns = 1;
    mp_mutex_unlock(&gate);
    sample_monitor(&f);
    assert(warnings == 3);
    assert(strstr(last_warning, "+1, 1 on this track"));

    // Dry, not dead: the recovery budget and the failure path are untouched.
    assert(atomic_load(&f.p.recovery_attempts) == 0 && !failures);
    assert(!atomic_load(&f.p.recreate_requested));
    atomic_store(&f.p.play_requested, false);
    teardown(&f);
    puts("PASS: underruns are logged per track, rate-limited, and never charged as stalls");
}

static void check_watchdog_after_reset(void)
{
    struct fixture f;
    setup(&f, true, 5, true);
    begin_write(&f, burst, sizeof(burst));
    stop(&f.ao);
    expect_delay(&f, 0);
    start(&f.ao); // still blocked in the old write, which pause did not interrupt
    assert(!pthread_create(&f.p.monitor, NULL, monitor_thread, &f.ao));
    f.p.monitor_created = true;
    mp_mutex_lock(&gate);
    int sampled = head_queries + 2;
    int interrupted = pauses + 1;
    mp_mutex_unlock(&gate);
    wait_counter(&head_queries, sampled);
    atomic_fetch_add(&now_ns, STALL_TIMEOUT_NS + 1);
    mp_cond_signal(&f.p.monitor_wakeup);
    wait_counter(&pauses, interrupted);
    assert(atomic_load(&f.p.recreate_requested));
    assert(atomic_load(&f.p.recovery_attempts) == 1);
    // Stop the monitor before observing the worker's idle/completion boundary.
    atomic_store(&f.p.monitor_terminate, true);
    mp_cond_signal(&f.p.monitor_wakeup);
    assert(!pthread_join(f.p.monitor, NULL));
    f.p.monitor_created = false;
    atomic_store(&f.p.play_requested, false);
    finish_write(&f, sizeof(payload));
    assert(track_count == 2);
    expect_delay(&f, 0);
    teardown(&f);
    puts("PASS: restart keeps an old blocked write visible to the existing watchdog");
}

// The reported bug: on a Xiaomi Pad 8 Pro, flush() leaves the render
// position untouched, so the head keeps its pre-seek value. Unanchored,
// written - head wraps to ~2^32 frames and the delay collapses to 0; the
// epoch anchor must absorb the stale counter instead.
static void check_pcm_stale_head_after_flush(void)
{
    struct fixture f;
    setup(&f, false, 1, true);
    const uint8_t data[16] = {1, 2, 3, 4, 5, 6, 7, 8};
    begin_write(&f, data, sizeof(data));
    finish_write(&f, 8);
    expect_delay(&f, 4);

    mp_mutex_lock(&gate);
    tracks[0].head_resets = false;
    tracks[0].head = 1000; // stale render position survives the flush
    mp_mutex_unlock(&gate);
    stop(&f.ao);

    begin_write(&f, data, sizeof(data));
    finish_write(&f, 8);
    expect_delay(&f, 4); // anchored: the delay is the new buffer, not 0
    assert(f.p.head_offset == 1000);
    assert(!warnings);
    teardown(&f);
    puts("PASS: a head that survives flush() anchors instead of wrapping the delay");
}

// The other half of the quirk: the counter resets to 0 only after the first
// post-flush read already anchored a stale value. The backward jump drops the
// anchor but keeps written_frames, which is real buffered audio.
static void check_pcm_delayed_head_reset(void)
{
    struct fixture f;
    setup(&f, false, 1, true);
    const uint8_t data[16] = {1, 2, 3, 4, 5, 6, 7, 8};
    begin_write(&f, data, sizeof(data));
    finish_write(&f, 8);

    mp_mutex_lock(&gate);
    tracks[0].head_resets = false;
    tracks[0].head = 1000;
    mp_mutex_unlock(&gate);
    stop(&f.ao);
    expect_delay(&f, 0); // anchors at the stale 1000
    assert(f.p.head_offset == 1000);

    mp_mutex_lock(&gate);
    tracks[0].head = 5; // the HAL resets late, already partway into the epoch
    mp_mutex_unlock(&gate);
    begin_write(&f, data, sizeof(data));
    finish_write(&f, 8);
    expect_delay_played(&f, 4, 4); // head 5 vs written 4: clamped, not wrapped
    assert(!f.p.head_offset);
    teardown(&f);
    puts("PASS: a delayed post-flush head reset drops the anchor, keeps the writes");
}

// A backward jump at the uint32 boundary is a wrap, not a reset: the anchor
// must survive or the position collapses.
static void check_pcm_head_wrap(void)
{
    struct fixture f;
    setup(&f, false, 1, true);
    const uint8_t data[16] = {1, 2, 3, 4, 5, 6, 7, 8};
    begin_write(&f, data, sizeof(data));
    finish_write(&f, 8);

    mp_mutex_lock(&gate);
    tracks[0].head_resets = false;
    tracks[0].head = 1000;
    mp_mutex_unlock(&gate);
    stop(&f.ao);
    expect_delay(&f, 0); // anchored at 1000

    mp_mutex_lock(&gate);
    tracks[0].head = UINT32_MAX - 100;
    mp_mutex_unlock(&gate);
    expect_delay_played(&f, 0, UINT32_MAX - 100 - 1000);
    mp_mutex_lock(&gate);
    tracks[0].head = 50; // wraps past UINT32_MAX
    mp_mutex_unlock(&gate);
    expect_delay_played(&f, 0, (uint32_t)(50 - 1000));
    assert(f.p.head_offset == 1000); // the anchor rode the wrap
    assert(!warnings);
    teardown(&f);
    puts("PASS: a head wrap keeps the epoch anchor and the delay arithmetic");
}

// A write stop() could not wait for lands in the track after the flush; the
// generation guard drops its written_frames credit, but the frames still
// play. The anchor plus the bounded overshoot clamp keep the delay at 0
// instead of wrapping it.
static void check_pcm_late_write_after_flush(void)
{
    struct fixture f;
    setup(&f, false, 1, true);
    const uint8_t data[16] = {1, 2, 3, 4, 5, 6, 7, 8};
    begin_write(&f, data, sizeof(data));
    finish_write(&f, 8);

    mp_mutex_lock(&gate);
    tracks[0].head_resets = false;
    mp_mutex_unlock(&gate);
    begin_write(&f, data, sizeof(data)); // blocked inside the JNI write
    mp_mutex_lock(&gate);
    tracks[0].head = 1000; // the counter goes stale only once flush() runs
    mp_mutex_unlock(&gate);
    stop(&f.ao);
    finish_write(&f, 8); // the late write completes post-flush, uncredited
    mp_mutex_lock(&gate);
    tracks[0].head = 1004; // and its frames actually play
    mp_mutex_unlock(&gate);
    expect_delay(&f, 0); // head 4 over written 0: absorbed, not wrapped
    teardown(&f);
    puts("PASS: a write landing after flush() is bounded by the epoch anchor");
}

// Same broken HAL through the timestamp source: framePosition survives the
// flush and keeps advancing. The pending anchor locks it to the head's
// epoch (ts_offset = fpos - head_played), so the timestamp path engages
// with the stale bias absorbed instead of being rejected or flapping.
static void check_pcm_stale_timestamp_after_flush(void)
{
    struct fixture f;
    setup(&f, false, 1, true);
    const uint8_t data[16] = {1, 2, 3, 4, 5, 6, 7, 8};
    begin_write(&f, data, sizeof(data));
    finish_write(&f, 8);

    mp_mutex_lock(&gate);
    tracks[0].head_resets = false;
    tracks[0].head = 700;
    tracks[0].timestamp_ok = true;
    tracks[0].ts_fpos = 700; // stale framePosition, still advancing
    tracks[0].ts_nanos = atomic_load(&now_ns);
    mp_mutex_unlock(&gate);
    stop(&f.ao);

    begin_write(&f, data, sizeof(data));
    finish_write(&f, 8);
    mp_mutex_lock(&gate);
    response = 0; // a parked zero-write must not credit stale frames
    mp_mutex_unlock(&gate);

    // Both counters advance together; the timestamp anchors to the head's
    // epoch on the first fetch and is trusted from the second.
    for (int n = 0; n < 4; n++) {
        atomic_fetch_add(&now_ns, MP_TIME_MS_TO_NS(60));
        mp_mutex_lock(&gate);
        tracks[0].head += 1;
        tracks[0].ts_fpos += 1;
        tracks[0].ts_nanos = atomic_load(&now_ns);
        tracks[0].state = AudioTrack.PLAYSTATE_PLAYING;
        mp_mutex_unlock(&gate);
        expect_delay_played(&f, 4, n + 1);
    }
    assert(f.p.timestamp_set);
    assert(!f.p.ts_reset_pending);
    assert(f.p.ts_offset == 700); // the stale bias, not the played frames
    park_worker(&f);
    teardown(&f);
    puts("PASS: a framePosition that survives flush() re-anchors to the head epoch");
}

static void check_pcm_healthy_counters_after_flush(void)
{
    struct fixture f;
    setup(&f, false, 1, true);
    const uint8_t data[16] = {1, 2, 3, 4, 5, 6, 7, 8};
    begin_write(&f, data, sizeof(data));
    finish_write(&f, 8);

    mp_mutex_lock(&gate);
    tracks[0].timestamp_ok = true;
    tracks[0].state = AudioTrack.PLAYSTATE_PLAYING;
    mp_mutex_unlock(&gate);
    stop(&f.ao); // head_resets: head and ts_fpos return to 0
    mp_mutex_lock(&gate);
    tracks[0].state = AudioTrack.PLAYSTATE_PLAYING;
    mp_mutex_unlock(&gate);

    begin_write(&f, data, sizeof(data));
    finish_write(&f, 8);
    mp_mutex_lock(&gate);
    response = 0; // a parked zero-write must not credit stale frames
    mp_mutex_unlock(&gate);
    for (int n = 0; n < 4; n++) {
        atomic_fetch_add(&now_ns, MP_TIME_MS_TO_NS(60));
        mp_mutex_lock(&gate);
        tracks[0].head += 1;
        tracks[0].ts_fpos += 1;
        tracks[0].ts_nanos = atomic_load(&now_ns);
        tracks[0].state = AudioTrack.PLAYSTATE_PLAYING;
        mp_mutex_unlock(&gate);
        // The timestamp path must report the same delay the head does:
        // an anchor biased by pre-fetch playback would skew it permanently.
        expect_delay_played(&f, 4, n + 1);
    }
    assert(f.p.timestamp_set); // trusted again by the second fetch
    assert(!f.p.head_offset); // a reset counter anchors at zero
    assert(f.p.ts_offset <= 4); // anchored against the head, not the fpos
    park_worker(&f);
    teardown(&f);
    puts("PASS: counters that reset on flush() keep exact delays and timestamps");
}

// Passthrough through the smoother: a delayed head reset must reset the
// (head - clock) ring too, or the estimate crawls toward truth at the 10%
// drift limit instead of snapping to the new epoch.
static void check_iec_delayed_reset_smoothing(void)
{
    struct fixture f;
    setup(&f, false, AudioFormat.ENCODING_IEC61937, true);

    mp_mutex_lock(&gate);
    tracks[0].head_resets = false;
    tracks[0].head = 4000;
    mp_mutex_unlock(&gate);
    stop(&f.ao);
    mp_mutex_lock(&f.p.lock);
    f.p.written_frames = 10000; // post-flush writes
    mp_mutex_unlock(&f.p.lock);
    expect_delay(&f, 10000); // anchored at 4000; head 0 reports nothing played

    // Played frames track the clock (40 ms at 48 kHz per step) so the
    // smoother's (head - clock) ring holds a steady offset.
    for (int n = 0; n < 3; n++) {
        atomic_fetch_add(&now_ns, MP_TIME_MS_TO_NS(40));
        mp_mutex_lock(&gate);
        tracks[0].head = 4000 + 1920 * (n + 1);
        mp_mutex_unlock(&gate);
        expect_delay_played(&f, 10000, 1920 * (n + 1));
    }

    // The HAL resets late; the smoother must not carry the pre-reset ring.
    atomic_fetch_add(&now_ns, MP_TIME_MS_TO_NS(40));
    mp_mutex_lock(&gate);
    tracks[0].head = 3;
    mp_mutex_unlock(&gate);
    expect_delay_played(&f, 10000, 3);
    teardown(&f);
    puts("PASS: a delayed head reset re-anchors the passthrough smoother");
}

// One passthrough clock poll.
static void poll_clock(struct fixture *f)
{
    mp_mutex_lock(&f->p.lock);
    int64_t sample_time = 0;
    AudioTrack_getLatency(&f->ao, &sample_time);
    mp_mutex_unlock(&f->p.lock);
}

static void stall_polls(struct fixture *f, int polls)
{
    for (int n = 0; n < polls; n++) {
        atomic_fetch_add(&now_ns, MP_TIME_MS_TO_NS(30));
        poll_clock(f);
    }
}

#define STEP_LINE_FORMAT "playhead step: est=%dms head=%dms over %dms; " \
    "raw=%u written=%u smoothed=%u polls=%u limited=%u resets=%u " \
    "ts=%d fpos=%u nano=%lld tsAgeMs=%d tsAdvancing=%d"

// The passthrough clock's step diagnostic: a HAL head that stops advancing
// moves the smoothed estimate against real time at the drift limiter's pace,
// and the line fires once that movement passes 20 ms -- naming the raw head,
// the written frames, the estimate, the limiter's share, and a getTimestamp
// probe that feeds nothing. Two steps inside the 5 s window are one line.
static void check_iec_playhead_step_line(void)
{
    struct fixture f;
    setup(&f, false, AudioFormat.ENCODING_IEC61937, true);

    mp_mutex_lock(&gate);
    tracks[0].head_resets = false;
    tracks[0].head = 4000;
    mp_mutex_unlock(&gate);
    stop(&f.ao);
    mp_mutex_lock(&f.p.lock);
    f.p.written_frames = 50000;
    mp_mutex_unlock(&f.p.lock);
    expect_delay(&f, 50000); // head 0: nothing to anchor on yet

    // Steady: the head follows the clock (1920 frames per 40 ms poll).
    for (int n = 0; n < 4; n++) {
        atomic_fetch_add(&now_ns, MP_TIME_MS_TO_NS(40));
        mp_mutex_lock(&gate);
        tracks[0].head = 4000 + 1920 * (n + 1);
        mp_mutex_unlock(&gate);
        expect_delay_played(&f, 50000, 1920 * (n + 1));
    }
    assert(step_lines == 0);

    // The head stalls. The limiter lets the estimate fall 144 frames (3 ms)
    // behind the clock per 30 ms poll: 18 ms after six polls is not yet a
    // step, 21 ms after the seventh is.
    stall_polls(&f, 6);
    assert(step_lines == 0);
    stall_polls(&f, 1);
    assert(step_lines == 1);
    int est_ms, head_ms, over_ms, ts_ok, ts_age, ts_adv;
    unsigned raw, written, smoothed, polls, limited, resets, fpos;
    long long nano;
    assert(sscanf(last_step_line, STEP_LINE_FORMAT, &est_ms, &head_ms, &over_ms,
                  &raw, &written, &smoothed, &polls, &limited, &resets, &ts_ok,
                  &fpos, &nano, &ts_age, &ts_adv) == 14);
    assert(est_ms == -21);        // seven limited polls of 3 ms
    assert(head_ms == -210);      // the raw head stood still for seven polls
    assert(over_ms == 3 * 40 + 7 * 30); // three steady polls, then seven stalled
    assert(raw == 4000 + 1920 * 4);
    assert(written == 50000);
    assert(smoothed == 7680 + 7 * (1440 - 144));
    assert(polls == 10 && limited == 7 && resets == 0);
    assert(ts_ok == 0 && ts_adv == -1 && ts_age == -1);

    // The next poll re-anchors; another 21 ms of movement inside the 5 s
    // window stays silent...
    mp_mutex_lock(&gate);
    tracks[0].timestamp_ok = true;
    tracks[0].ts_fpos = 123;
    tracks[0].ts_nanos = atomic_load(&now_ns) - MP_TIME_MS_TO_NS(7);
    mp_mutex_unlock(&gate);
    stall_polls(&f, 8);
    assert(step_lines == 1);
    // ...and the line after the window reports the whole movement since the
    // previous one (the limiter's 10 % of a 5 s poll included), with the
    // probe's first reading. Writes kept up meanwhile, so nothing overshoots.
    mp_mutex_lock(&f.p.lock);
    f.p.written_frames += 5 * 48000;
    mp_mutex_unlock(&f.p.lock);
    atomic_fetch_add(&now_ns, MP_TIME_S_TO_NS(5));
    poll_clock(&f);
    assert(step_lines == 2);
    assert(sscanf(last_step_line, STEP_LINE_FORMAT, &est_ms, &head_ms, &over_ms,
                  &raw, &written, &smoothed, &polls, &limited, &resets, &ts_ok,
                  &fpos, &nano, &ts_age, &ts_adv) == 14);
    assert(est_ms == -(7 * 3 + 500) && head_ms == -(7 * 30 + 5000));
    assert(over_ms == 7 * 30 + 5000 && polls == 8 && limited == 8 && resets == 0);
    assert(ts_ok == 1 && fpos == 123 && ts_adv == -1);
    assert(ts_age == 7 + 8 * 30 + 5000);
    // The probe fed nothing: the clock is still the head, untouched.
    assert(!f.p.timestamp_set && !f.p.timestamp_fetched && !f.p.timestamp_last_fpos);
    assert(f.p.smooth_reported == (int64_t)smoothed);

    // A probe that sees framePosition move reports it advancing.
    mp_mutex_lock(&gate);
    tracks[0].ts_fpos = 4567;
    mp_mutex_unlock(&gate);
    stall_polls(&f, 1); // re-anchors
    mp_mutex_lock(&f.p.lock);
    f.p.written_frames += 5 * 48000;
    mp_mutex_unlock(&f.p.lock);
    atomic_fetch_add(&now_ns, MP_TIME_S_TO_NS(5));
    poll_clock(&f);
    assert(step_lines == 3);
    assert(strstr(last_step_line, " fpos=4567 ") && strstr(last_step_line, "tsAdvancing=1"));

    teardown(&f);
    puts("PASS: a stalled passthrough head logs one rate-limited step line with a "
         "getTimestamp probe that feeds nothing");
}

// A recreated track arms the same anchors: its first read cannot trip the
// backward-jump path or warn.
static void check_recreate_anchors_fresh_track(void)
{
    struct fixture f;
    setup(&f, false, 1, true);
    const uint8_t data[16] = {1, 2, 3, 4, 5, 6, 7, 8};
    begin_write(&f, data, sizeof(data));
    finish_write(&f, 8);

    mp_mutex_lock(&gate);
    tracks[0].head_resets = false;
    tracks[0].head = 1000;
    mp_mutex_unlock(&gate);
    stop(&f.ao);
    expect_delay(&f, 0); // anchor held on the old track

    mp_mutex_lock(&f.p.lock);
    assert(!AudioTrack_Recreate(&f.ao));
    mp_mutex_unlock(&f.p.lock);
    expect_delay(&f, 0); // fresh track, fresh anchor, no spurious warn
    assert(!warnings);
    teardown(&f);
    puts("PASS: recreate re-anchors without tripping reset detection");
}

int main(void)
{
    alarm(15); // fail a regression that holds the reset lock, rather than hang CI
    check_raw_partial_reset();
    check_raw_dtshd(13, 48000, 1);
    check_raw_dtshd(14, 96000, 2);
    check_dtshd_unwrap_fragmented();
    puts("PASS: DTS-HD bursts unwrap to the bare packet and a 96 kHz core reopens the track");
    check_pcm_and_iec(1, true, 6, 6);
    check_pcm_and_iec(1, false, 6, 6);
    check_pcm_and_iec(AudioFormat.ENCODING_PCM_FLOAT, false, 2, 8);
    check_pcm_and_iec(AudioFormat.ENCODING_IEC61937, true, 3, 6);
    puts("PASS: PCM direct/byte/float and IEC partial counts survive reset without stale credit");
    check_dead_object();
    check_watchdog_after_reset();
    check_underrun_reporting();
    check_pcm_stale_head_after_flush();
    check_pcm_delayed_head_reset();
    check_pcm_head_wrap();
    check_pcm_late_write_after_flush();
    check_pcm_stale_timestamp_after_flush();
    check_pcm_healthy_counters_after_flush();
    check_iec_delayed_reset_smoothing();
    check_iec_playhead_step_line();
    check_recreate_anchors_fresh_track();
    return 0;
}
