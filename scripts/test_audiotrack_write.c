// Compile the production worker, JNI write boundary, reset/recreate and clock.
// The injected device can ignore pause while write is gated; it reads the live
// buffer again at completion and enforces JNI reference lifetime throughout.
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MP_TIME_MS_TO_NS(v) ((int64_t)((v) * 1000000LL))
#define MP_TIME_S_TO_NS(v) ((int64_t)((v) * 1000000000.0))
#define MP_TIME_NS_TO_S(v) ((v) / 1000000000.0)
#define MP_ARRAY_SIZE(v) ((int)(sizeof(v) / sizeof((v)[0])))
#define MPMIN(a, b) ((a) < (b) ? (a) : (b))
#define MPCLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define MP_THREAD_VOID void *
#define MP_THREAD_RETURN() return NULL
#define AF_FORMAT_S_DTS 99

typedef pthread_t mp_thread;
typedef pthread_mutex_t mp_mutex;
typedef pthread_cond_t mp_cond;
typedef int jint, jmethodID;
struct object {
    bool track, released;
    int refs, state, position;
    uint8_t *data;
    uint8_t sink[256];
    int sink_size;
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
static int entered, permitted, idle, head_queries, pauses, failures;
static int response, input_bytes;
static bool pause_after_write;
static uint8_t input[256];
static struct object tracks[16];
static int track_count;

static void mp_mutex_lock(mp_mutex *m) { assert(!pthread_mutex_lock(m)); }
static void mp_mutex_unlock(mp_mutex *m) { assert(!pthread_mutex_unlock(m)); }
static void mp_cond_signal(mp_cond *c) { assert(!pthread_cond_signal(c)); }
static void mp_thread_set_name(const char *name) { (void)name; }
static int64_t mp_time_ns(void) { return atomic_load(&now_ns); }
static int64_t mp_raw_time_ns(void) { return mp_time_ns(); }
static int64_t mp_time_ns_from_raw_time(int64_t ns) { return ns; }
static void test_log(const char *fmt, ...) { (void)fmt; }
#define MP_VERBOSE(ao, ...) test_log(__VA_ARGS__)
#define MP_WARN(ao, ...) test_log(__VA_ARGS__)
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
    int release, pause, flush, play;
    int PLAYSTATE_PLAYING, PLAYSTATE_PAUSED, WRITE_BLOCKING;
} AudioTrack = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 3, 2, 0};
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
    if (method == AudioTrack.getPlaybackHeadPosition || method == AudioTrack.getLatency) {
        if (method == AudioTrack.getPlaybackHeadPosition) {
            head_queries++;
            pthread_cond_broadcast(&changed);
        }
        mp_mutex_unlock(&gate);
        return 0;
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
#define MP_JNI_CALL_BOOL(obj, method, stamp) ((void)env, false)
#define MP_JNI_GET_LONG(obj, field) ((void)AudioTimestamp, (int64_t)0)

static int AudioTrack_New(struct ao *ao);
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
                             .state = AudioTrack.PLAYSTATE_PAUSED};
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
    struct object buffer;
    uint8_t chunk[256], backing[264];
};

static void setup(struct fixture *f, bool raw, int format, bool direct)
{
    memset(f, 0, sizeof(*f));
    entered = permitted = idle = head_queries = pauses = failures = 0;
    response = input_bytes = track_count = 0;
    pause_after_write = true;
    atomic_store(&now_ns, MP_TIME_S_TO_NS(1));
    AudioTrack.writeBufferV21 = direct ? 1 : 0;
    f->ao = (struct ao){.priv = &f->p, .samplerate = 48000, .sstride = 2};
    f->buffer = (struct object){.refs = 1,
        .data = direct && !raw ? f->chunk : f->backing};
    f->p.raw_passthrough = raw;
    f->p.raw_rate_mult = 1;
    f->p.format = format;
    f->p.chunksize = sizeof(f->chunk);
    f->p.chunk = f->chunk;
    f->p.rawbuf = f->backing;
    f->p.bbuf = f->p.shortarray = f->p.floatarray = f->p.bytearray = &f->buffer;
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

static void expect_delay(struct fixture *f, uint32_t frames)
{
    mp_mutex_lock(&f->p.lock);
    int64_t sample_time = 0;
    double delay = AudioTrack_getLatency(&f->ao, &sample_time);
    assert(f->p.written_frames == frames);
    assert(fabs(delay - frames / 48000.0) < 1e-12);
    mp_mutex_unlock(&f->p.lock);
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

int main(void)
{
    alarm(15); // fail a regression that holds the reset lock, rather than hang CI
    check_raw_partial_reset();
    check_pcm_and_iec(1, true, 6, 6);
    check_pcm_and_iec(1, false, 6, 6);
    check_pcm_and_iec(AudioFormat.ENCODING_PCM_FLOAT, false, 2, 8);
    check_pcm_and_iec(AudioFormat.ENCODING_IEC61937, true, 3, 6);
    puts("PASS: PCM direct/byte/float and IEC partial counts survive reset without stale credit");
    check_dead_object();
    check_watchdog_after_reset();
    return 0;
}
