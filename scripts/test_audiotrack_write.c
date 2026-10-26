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
#define AF_FORMAT_S_TRUEHD 97
#define AF_FORMAT_S_EAC3 96
#define AF_FORMAT_S_AC3 95

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
struct ao { void *priv; int samplerate, sstride, format; bool stream_silence; };

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static atomic_llong now_ns;
static int entered, permitted, idle, head_queries, pauses, plays, failures, warnings, reloads;
static int new_failures; // AudioTrack_New fails this many times: the device is away
static int underrun_queries;
static char last_warning[256];
static int response, input_bytes;
static int last_write_count; // bytes the latest JNI write asked the track to take
static bool pause_after_write;
static uint8_t input[256];
static struct object tracks[16];
static int track_count;

// A test can have a thread other than the worker run hook_fn once, just
// before that thread takes hook_lock: this is how the write the device holds
// is completed at the moment the monitor reaches for track_lock. The hook is
// armed by the warning the monitor logs first (warn_log, hook_after_warning).
static mp_mutex *_Atomic hook_lock;
static mp_mutex *hook_after_warning;
static pthread_t hook_worker;
static void (*hook_fn)(void);
// A test can count the attempts every thread but `watched_excluded` makes on
// `watched_lock` (each is counted before it blocks): how a hook knows that
// the worker, or a lifecycle call on another thread, has reached a lock the
// hook's own thread holds.
static mp_mutex *_Atomic watched_lock;
static pthread_t watched_excluded;
static int watched_waits;
// A test can have the fake AudioTrack.pause run pause_hook_fn once on its
// caller (with gate held), the fake AudioTrack_New run new_hook_fn once on
// the worker, and the worker run unlock_hook_fn once as it releases
// unlock_hook_lock.
static void (*pause_hook_fn)(void);
static void (*new_hook_fn)(void);
static mp_mutex *_Atomic unlock_hook_lock;
static void (*unlock_hook_fn)(void);
static void mp_mutex_lock(mp_mutex *m)
{
    if (m == atomic_load(&hook_lock) && !pthread_equal(pthread_self(), hook_worker)) {
        atomic_store(&hook_lock, NULL);
        hook_fn();
    }
    if (m == atomic_load(&watched_lock) && !pthread_equal(pthread_self(), watched_excluded)) {
        assert(!pthread_mutex_lock(&gate));
        watched_waits++;
        pthread_cond_broadcast(&changed);
        assert(!pthread_mutex_unlock(&gate));
    }
    assert(!pthread_mutex_lock(m));
}
static void mp_mutex_unlock(mp_mutex *m)
{
    assert(!pthread_mutex_unlock(m));
    if (m == atomic_load(&unlock_hook_lock) && pthread_equal(pthread_self(), hook_worker)) {
        atomic_store(&unlock_hook_lock, NULL);
        unlock_hook_fn();
    }
}
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
    if (hook_after_warning) {
        atomic_store(&hook_lock, hook_after_warning);
        hook_after_warning = NULL;
    }
    pthread_cond_broadcast(&changed);
    mp_mutex_unlock(&gate);
    va_end(args);
}
static int errors; // lines logged at the error level
static void err_log(const char *fmt, ...)
{
    (void)fmt;
    mp_mutex_lock(&gate);
    errors++;
    mp_mutex_unlock(&gate);
}
#define MP_VERBOSE(ao, ...) test_log(__VA_ARGS__)
#define MP_WARN(ao, ...) warn_log(__VA_ARGS__)
#define MP_ERR(ao, ...) err_log(__VA_ARGS__)

static int64_t last_wait_ns; // what the worker asked its latest wait for

static void mp_cond_timedwait(mp_cond *c, mp_mutex *m, int64_t ns)
{
    // The worker publishes its completed accounting before entering this wait.
    mp_mutex_lock(&gate);
    idle++;
    last_wait_ns = ns;
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
    last_write_count = count * unit;
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
        if (pause_hook_fn) {
            // Before the pause takes effect, with gate held: the hook waits
            // on `changed`.
            void (*fn)(void) = pause_hook_fn;
            pause_hook_fn = NULL;
            fn();
        }
        track->state = AudioTrack.PLAYSTATE_PAUSED;
        pauses++;
    } else if (method == AudioTrack.play) {
        track->state = AudioTrack.PLAYSTATE_PLAYING;
        plays++;
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

// No Java exception is ever pending here. A call rather than a comma
// expression: ao_audiotrack.c also uses the macro as a bare statement, which
// gcc rejects as a statement with no effect under -Werror=unused-value.
static int jni_exception_log(const void *ao)
{
    (void)ao;
    return 0;
}

#define MP_JNI_GET_ENV(ao) (&environment)
#define MP_JNI_CALL_INT(obj, method, ...) jni_int(env, obj, method, ##__VA_ARGS__)
#define MP_JNI_CALL_VOID(obj, method) jni_void(env, obj, method)
#define MP_JNI_CALL_OBJECT(obj, method) jni_clear(env, obj, method)
#define MP_JNI_LOCAL_FREEP(obj) drop_ref(env, obj)
#define MP_JNI_GLOBAL_FREEP(obj) drop_ref(env, obj)
#define MP_JNI_EXCEPTION_LOG(ao) jni_exception_log(ao)
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
// The fake AudioTrack_New below attaches no loudness effect, so there is none
// for AudioTrack_Recreate to release.
static void AudioTrack_releaseNormalizer(struct ao *ao) { (void)ao; }
static void ao_request_failure(struct ao *ao) { (void)ao; failures++; }
static void ao_request_reload(struct ao *ao) { (void)ao; reloads++; }
static const char *af_fmt_to_str(int format) { (void)format; return "spdif"; }
static bool af_fmt_is_spdif(int format) { return format >= AF_FORMAT_S_AC3; }
static int ao_read_data(struct ao *, void **, int, int64_t, void *, bool, bool);
#include "audiotrack_write.inc"

static int AudioTrack_New(struct ao *ao)
{
    struct priv *p = ao->priv;
    if (new_hook_fn) {
        // On the worker, with p->lock and track_lock held, as in production.
        void (*fn)(void) = new_hook_fn;
        new_hook_fn = NULL;
        fn();
    }
    mp_mutex_lock(&gate);
    if (new_failures) {
        // AudioFlinger has no output for the format while HDMI re-plugs.
        new_failures--;
        mp_mutex_unlock(&gate);
        return -1;
    }
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
    (void)end; (void)pts;
    // A non-blocking read that misses the core's lock returns 0, which a
    // stream-silence track cannot tell from an empty queue.
    assert(block == ao->stream_silence);
    mp_mutex_lock(&gate);
    assert(input_bytes <= samples * ao->sstride);
    memcpy(*data, input, input_bytes);
    // A padded read fills the rest of the chunk with silence and still
    // returns what it copied, as buffer.c does.
    if (pad)
        memset((uint8_t *)*data + input_bytes, 0, samples * ao->sstride - input_bytes);
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
    entered = permitted = idle = head_queries = pauses = plays = failures = warnings = reloads = 0;
    errors = 0;
    new_failures = 0;
    response = input_bytes = track_count = underrun_queries = step_lines = 0;
    last_write_count = 0;
    atomic_store(&hook_lock, NULL);
    hook_after_warning = NULL;
    atomic_store(&watched_lock, NULL);
    watched_waits = 0;
    pause_hook_fn = new_hook_fn = NULL;
    atomic_store(&unlock_hook_lock, NULL);
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

// The worker is waiting for its next retry of a failed replacement: no
// track, nothing failed, the retry armed. `wakeups` idle waits later it is
// still waiting (the fake clock has not reached the retry time).
static void expect_retry_pending(struct fixture *f, int wakeups)
{
    for (int n = 0; n < wakeups; n++) {
        mp_mutex_lock(&gate);
        int target = idle + 1;
        mp_mutex_unlock(&gate);
        mp_mutex_lock(&f->p.lock);
        mp_cond_signal(&f->p.wakeup);
        mp_mutex_unlock(&f->p.lock);
        wait_counter(&idle, target);
    }
    mp_mutex_lock(&f->p.lock);
    assert(!f->p.audiotrack);
    assert(f->p.recreate_deadline_ns == MP_TIME_S_TO_NS(1) + RECREATE_GRACE_NS);
    mp_mutex_unlock(&f->p.lock);
    assert(atomic_load(&f->p.recreate_requested));
    assert(!atomic_load(&f->p.failed) && !failures);
}

// The fake clock moves `ns` on and the worker is woken to look at it.
static void advance_and_wake(struct fixture *f, int64_t ns)
{
    atomic_fetch_add(&now_ns, ns);
    mp_mutex_lock(&f->p.lock);
    mp_cond_signal(&f->p.wakeup);
    mp_mutex_unlock(&f->p.lock);
}

// A replacement that cannot be created - the HDMI audio device is gone for
// ~100 ms around a display mode switch - is retried from the top of the
// worker loop every RECREATE_RETRY_NS, with no track meanwhile and nothing
// failed, and succeeds once the device is back. The recovery budget is
// charged once, for the episode.
static void check_recreate_retries_while_device_away(void)
{
    struct fixture f;
    setup(&f, true, 5, true);
    begin_write(&f, burst, sizeof(burst));
    mp_mutex_lock(&gate);
    new_failures = 1;
    mp_mutex_unlock(&gate);
    atomic_store(&f.p.play_requested, false); // the replacement is not started
    finish_write(&f, AudioManager.ERROR_DEAD_OBJECT);
    assert(track_count == 1 && atomic_load(&f.p.recovery_attempts) == 1);
    assert(warnings == 2 && strstr(last_warning, "retrying for 3000 ms"));
    expect_retry_pending(&f, 1);

    // Half the interval on: still waiting. The rest of it: the device is
    // back, the track is recreated, and the worker idles on it (paused).
    advance_and_wake(&f, RECREATE_RETRY_NS / 2);
    expect_retry_pending(&f, 2);
    mp_mutex_lock(&gate);
    int target = idle + 1;
    mp_mutex_unlock(&gate);
    advance_and_wake(&f, RECREATE_RETRY_NS / 2);
    wait_counter(&idle, target);
    mp_mutex_lock(&f.p.lock);
    assert(f.p.audiotrack == &tracks[1] && !f.p.recreate_deadline_ns);
    mp_mutex_unlock(&f.p.lock);
    assert(track_count == 2 && !atomic_load(&f.p.recreate_requested));
    assert(warnings == 3 && strstr(last_warning, "recreated once the output device was back"));
    assert(!atomic_load(&f.p.failed) && !failures);
    assert(atomic_load(&f.p.recovery_attempts) == 1);
    expect_delay(&f, 0);
    teardown(&f);
    puts("PASS: a failed track replacement is retried after 100 ms and succeeds "
         "once the device is back");
}

// The device does not come back within RECREATE_GRACE_NS: the retry that
// finds the grace over gives up, and the AO fails as before.
static void check_recreate_gives_up_after_grace(void)
{
    struct fixture f;
    setup(&f, true, 5, true);
    begin_write(&f, burst, sizeof(burst));
    mp_mutex_lock(&gate);
    new_failures = 1000;
    mp_mutex_unlock(&gate);
    atomic_store(&f.p.play_requested, false);
    finish_write(&f, AudioManager.ERROR_DEAD_OBJECT);
    expect_retry_pending(&f, 1);

    mp_mutex_lock(&gate);
    int target = idle + 1;
    mp_mutex_unlock(&gate);
    advance_and_wake(&f, RECREATE_GRACE_NS);
    wait_counter(&idle, target);
    assert(atomic_load(&f.p.failed) && failures == 1);
    assert(track_count == 1 && !atomic_load(&f.p.recreate_requested));
    mp_mutex_lock(&f.p.lock);
    assert(!f.p.audiotrack && !f.p.recreate_deadline_ns);
    mp_mutex_unlock(&f.p.lock);
    assert(new_failures == 998); // the first attempt and the one retry
    teardown(&f);
    puts("PASS: a replacement still failing after the grace period fails the AO");
}

// The replacement's stall clock is its own. The write the device held on the
// old track and the retries the replacement took are not counted against
// the new track: its first write opens a fresh window, and the monitor
// passes a verdict on it STALL_TIMEOUT_NS after that write, not after the
// old one.
static void check_recreate_retry_restarts_stall_clock(void)
{
    struct fixture f;
    setup(&f, true, 5, true);
    begin_write(&f, burst, sizeof(burst)); // outstanding since t=1 s
    mp_mutex_lock(&gate);
    new_failures = 1;
    mp_mutex_unlock(&gate);
    finish_write(&f, AudioManager.ERROR_DEAD_OBJECT);
    expect_retry_pending(&f, 1);
    assert(!atomic_load(&f.p.write_outstanding)); // nothing to hold during the retries

    // The device is back: the replacement is created playing (the core
    // still wants playback) and the device holds its first write.
    mp_mutex_lock(&gate);
    int target = entered + 1;
    memcpy(input, burst, sizeof(burst));
    input_bytes = sizeof(burst);
    mp_mutex_unlock(&gate);
    advance_and_wake(&f, RECREATE_RETRY_NS);
    wait_counter(&entered, target);
    mp_mutex_lock(&gate);
    assert(track_count == 2 && tracks[1].state == AudioTrack.PLAYSTATE_PLAYING);
    mp_mutex_unlock(&gate);
    assert(atomic_load(&f.p.write_outstanding));
    assert(atomic_load(&f.p.write_progress_ns) == MP_TIME_S_TO_NS(1) + RECREATE_RETRY_NS);

    assert(!pthread_create(&f.p.monitor, NULL, monitor_thread, &f.ao));
    f.p.monitor_created = true;
    mp_mutex_lock(&gate);
    int sampled = head_queries + 2;
    int interrupted = pauses;
    mp_mutex_unlock(&gate);
    wait_counter(&head_queries, sampled);
    // STALL_TIMEOUT_NS and more since the old track's write, less since the
    // replacement's: no verdict.
    atomic_fetch_add(&now_ns, STALL_TIMEOUT_NS - RECREATE_RETRY_NS / 2);
    mp_mutex_lock(&gate);
    sampled = head_queries + 2;
    mp_mutex_unlock(&gate);
    mp_cond_signal(&f.p.monitor_wakeup);
    wait_counter(&head_queries, sampled);
    mp_mutex_lock(&gate);
    assert(pauses == interrupted);
    mp_mutex_unlock(&gate);
    assert(!atomic_load(&f.p.recreate_requested) && !atomic_load(&f.p.failed));
    assert(atomic_load(&f.p.recovery_attempts) == 1 && !failures);
    // STALL_TIMEOUT_NS since the replacement's write: the verdict, on a
    // budget cleared as STALL_RECOVERED_NS of health would clear it.
    atomic_store(&f.p.recovery_attempts, 0);
    atomic_fetch_add(&now_ns, RECREATE_RETRY_NS);
    mp_cond_signal(&f.p.monitor_wakeup);
    wait_counter(&pauses, interrupted + 1);
    assert(atomic_load(&f.p.recreate_requested));
    assert(atomic_load(&f.p.recovery_attempts) == 1);
    assert(strstr(last_warning, "accepted no audio for 5050 ms")); // since the new write
    atomic_store(&f.p.monitor_terminate, true);
    mp_cond_signal(&f.p.monitor_wakeup);
    assert(!pthread_join(f.p.monitor, NULL));
    f.p.monitor_created = false;
    atomic_store(&f.p.play_requested, false);
    finish_write(&f, sizeof(payload));
    assert(track_count == 3 && !failures);
    expect_delay(&f, 0);
    teardown(&f);
    puts("PASS: a replacement made after retries gets a stall window of its own");
}

// The DTS-HD core-rate reopen (AudioTrack_adoptDtsHdRate) runs inside the
// write with the first packet saved. When its replacement fails, the write
// returns without waiting, the packet stays pending, the retry recreates
// the track without clearing it again, and the packet is the first thing
// written to the new track.
static void check_raw_dtshd_reopen_retry(void)
{
    struct fixture f;
    setup(&f, true, 5, true);
    f.ao.format = AF_FORMAT_S_DTSHD;
    f.ao.samplerate = 192000;
    f.p.samplerate = 48000;
    f.p.raw_rate_mult = 4;
    uint8_t stream[64];
    const int len = dtshd_burst(stream, 14);
    const uint8_t packet[10] = {0x7f, 0xfe, 0x80, 0x01, 0x3f, 0xc0, 0x40, 0x00, 14 << 2, 0xab};

    mp_mutex_lock(&gate);
    new_failures = 1;
    int target = idle + 1;
    memcpy(input, stream, len);
    input_bytes = len;
    mp_mutex_unlock(&gate);
    start(&f.ao);
    wait_counter(&idle, target);
    expect_retry_pending(&f, 1);
    mp_mutex_lock(&f.p.lock);
    assert(f.p.raw_pending == (int)sizeof(packet)); // the saved packet
    assert(f.p.samplerate == 96000 && f.p.raw_rate_mult == 2 && f.p.raw_rate_verified);
    mp_mutex_unlock(&f.p.lock);
    assert(track_count == 1 && !atomic_load(&f.p.recovery_attempts));
    assert(warnings == 1 && strstr(last_warning, "retrying for 3000 ms"));

    mp_mutex_lock(&gate);
    target = entered + 1;
    mp_mutex_unlock(&gate);
    advance_and_wake(&f, RECREATE_RETRY_NS);
    wait_counter(&entered, target);
    assert(track_count == 2 && warnings == 2);
    assert(strstr(last_warning, "recreated once the output device was back"));
    finish_write(&f, sizeof(packet));
    assert(tracks[1].sink_size == (int)sizeof(packet));
    assert(!memcmp(tracks[1].sink, packet, sizeof(packet)));
    mp_mutex_lock(&f.p.lock);
    assert(f.p.written_frames == (uint32_t)(len / f.ao.sstride));
    assert(!f.p.raw_pending);
    mp_mutex_unlock(&f.p.lock);
    assert(!failures);
    teardown(&f);
    puts("PASS: a DTS-HD reopen whose replacement fails keeps the packet for the retried track");
}

// The core seeks and resumes while the replacement is pending. stop() finds
// no track and says so below the error level, and still resets the clock:
// the packet saved for the replacement goes with it, as the core has dropped
// its side of the seek. start() finds no track and leaves the start to the
// retry, which makes the replacement playing. The replacement's first write
// is the audio after the seek, not the packet from before it.
static void check_recreate_stop_start_during_retry(void)
{
    struct fixture f;
    setup(&f, true, 5, true);
    f.ao.format = AF_FORMAT_S_DTSHD;
    f.ao.samplerate = 192000;
    f.p.samplerate = 48000;
    f.p.raw_rate_mult = 4;
    uint8_t stream[64];
    const int len = dtshd_burst(stream, 14);
    const uint8_t packet[10] = {0x7f, 0xfe, 0x80, 0x01, 0x3f, 0xc0, 0x40, 0x00, 14 << 2, 0xab};

    mp_mutex_lock(&gate);
    new_failures = 1;
    int target = idle + 1;
    memcpy(input, stream, len);
    input_bytes = len;
    mp_mutex_unlock(&gate);
    start(&f.ao);
    wait_counter(&idle, target);
    expect_retry_pending(&f, 1);
    mp_mutex_lock(&f.p.lock);
    assert(f.p.raw_pending == (int)sizeof(packet)); // the saved packet
    mp_mutex_unlock(&f.p.lock);

    stop(&f.ao);  // the seek
    start(&f.ao); // and the resume
    assert(!errors && warnings == 1);
    mp_mutex_lock(&f.p.lock);
    assert(!f.p.audiotrack && !f.p.raw_pending);
    mp_mutex_unlock(&f.p.lock);
    assert(atomic_load(&f.p.recreate_requested));
    assert(atomic_load(&f.p.play_requested));
    expect_retry_pending(&f, 1);

    // The device is back: the replacement is made playing, and the first
    // write to it is the burst the core has after the seek.
    mp_mutex_lock(&gate);
    target = entered + 1;
    memcpy(input, stream, len);
    input_bytes = len;
    mp_mutex_unlock(&gate);
    advance_and_wake(&f, RECREATE_RETRY_NS);
    wait_counter(&entered, target);
    mp_mutex_lock(&gate);
    assert(track_count == 2 && tracks[1].state == AudioTrack.PLAYSTATE_PLAYING);
    mp_mutex_unlock(&gate);
    assert(!atomic_load(&f.p.recreate_requested));
    finish_write(&f, sizeof(packet));
    assert(tracks[1].sink_size == (int)sizeof(packet));
    assert(!memcmp(tracks[1].sink, packet, sizeof(packet)));
    mp_mutex_lock(&f.p.lock);
    assert(f.p.written_frames == (uint32_t)(len / f.ao.sstride));
    assert(!f.p.raw_pending);
    mp_mutex_unlock(&f.p.lock);
    assert(!errors && !failures);
    teardown(&f);
    puts("PASS: a seek and a resume while the replacement is pending drop the "
         "saved packet and leave the start to the retry");
}

// The worker's write in progress completes (the device takes it whole), and
// the hook waits for the worker's next write before it returns.
static int completed_len; // what that write asked the track to take
static void complete_write_then_wait_for_the_next(void)
{
    mp_mutex_lock(&gate);
    response = completed_len;
    permitted = entered;
    int target = entered + 1;
    pthread_cond_broadcast(&changed);
    while (entered < target)
        pthread_cond_wait(&changed, &gate);
    mp_mutex_unlock(&gate);
}

// The monitor's recreate request and its pause are one step under
// track_lock. Stored before the lock, the request could be answered in the
// gap: the device takes the write it held, the worker sees the request,
// replaces the track and plays the replacement, and the monitor's pause then
// lands on that replacement, which stays paused with nothing outstanding and
// so no verdict to come, and with --audio-stream-silence no stop()/start()
// from the core either. The hook has the device take the write at the moment
// the monitor reaches for track_lock, and returns once the worker is in its
// next write: on this code the worker finds no request yet and writes an
// empty read to the old track; stored first, it would make the replacement
// before the pause.
static void check_recreate_request_meets_completed_write(void)
{
    struct fixture f;
    setup(&f, false, 1, true);
    pause_after_write = false; // the track plays on once the device takes the write
    static const uint8_t audio[6] = {1, 2, 3, 4, 5, 6};
    completed_len = sizeof(audio);
    begin_write(&f, audio, sizeof(audio)); // the device holds it; the head stays at 0

    assert(!pthread_create(&f.p.monitor, NULL, monitor_thread, &f.ao));
    f.p.monitor_created = true;
    mp_mutex_lock(&gate);
    int sampled = head_queries + 2;
    int interrupted = pauses + 1;
    hook_worker = f.p.thread;
    hook_fn = complete_write_then_wait_for_the_next;
    hook_after_warning = &f.p.track_lock;
    mp_mutex_unlock(&gate);
    wait_counter(&head_queries, sampled);
    atomic_fetch_add(&now_ns, STALL_TIMEOUT_NS + 1);
    mp_cond_signal(&f.p.monitor_wakeup);
    wait_counter(&pauses, interrupted);
    assert(!atomic_load(&hook_lock) && !hook_after_warning);
    assert(warnings == 1 && strstr(last_warning, "recreating the AudioTrack (attempt 1/1)"));
    assert(atomic_load(&f.p.recreate_requested));
    // Stop the monitor before observing the worker.
    atomic_store(&f.p.monitor_terminate, true);
    mp_cond_signal(&f.p.monitor_wakeup);
    assert(!pthread_join(f.p.monitor, NULL));
    f.p.monitor_created = false;

    // The pause released the worker's write (nothing taken). The worker
    // answers the request now: the old track goes, the replacement is made
    // playing, and the worker's next write goes to it.
    mp_mutex_lock(&gate);
    int writes = entered, polls = idle;
    response = 0;
    permitted = entered;
    pthread_cond_broadcast(&changed);
    while (entered == writes && idle == polls)
        pthread_cond_wait(&changed, &gate);
    assert(track_count == 2 && tracks[0].released);
    assert(tracks[1].state == AudioTrack.PLAYSTATE_PLAYING);
    assert(entered > writes); // writing to the replacement, not idling on a paused one
    mp_mutex_unlock(&gate);
    assert(!atomic_load(&f.p.recreate_requested));
    assert(atomic_load(&f.p.recovery_attempts) == 1 && !failures);
    assert(warnings == 1 && !errors);
    park_worker(&f);
    teardown(&f);
    puts("PASS: a recreate request that meets the completed write replaces the "
         "track after the pause, and the replacement plays");
}

// The core's stop() and start() while a retried attempt is being made: the
// request stands through the attempt, so both find the replacement pending
// (verbose) rather than the track missing (an error). The attempt runs on the
// worker with p->lock and track_lock held, so stop() waits for track_lock;
// the hook in the fake AudioTrack_New has the core call it then, and the
// worker's release of track_lock lets it through up to p->lock before the
// worker goes on to re-arm the request: were the request cleared before the
// attempt, stop() would find neither a track nor a request in that window.
static struct fixture *attempt_f;
static pthread_t attempt_caller;
static void *stop_and_start(void *arg)
{
    struct ao *ao = arg;
    stop(ao);  // the seek
    start(ao); // and the resume
    return NULL;
}
static void seek_during_the_attempt(void)
{
    mp_mutex_lock(&gate);
    int target = watched_waits + 1;
    mp_mutex_unlock(&gate);
    watched_excluded = pthread_self();
    atomic_store(&watched_lock, &attempt_f->p.track_lock);
    assert(!pthread_create(&attempt_caller, NULL, stop_and_start, &attempt_f->ao));
    wait_counter(&watched_waits, target); // stop() waits for track_lock
    atomic_store(&watched_lock, NULL);
}
static void let_the_seek_through(void)
{
    mp_mutex_lock(&gate);
    int target = watched_waits + 1;
    mp_mutex_unlock(&gate);
    atomic_store(&watched_lock, &attempt_f->p.lock);
    wait_counter(&watched_waits, target); // stop() is through track_lock, at p->lock
    atomic_store(&watched_lock, NULL);
}

static void check_recreate_stop_start_during_attempt(void)
{
    struct fixture f;
    setup(&f, true, 5, true);
    begin_write(&f, burst, sizeof(burst));
    mp_mutex_lock(&gate);
    new_failures = 1;
    mp_mutex_unlock(&gate);
    finish_write(&f, AudioManager.ERROR_DEAD_OBJECT);
    assert(track_count == 1 && atomic_load(&f.p.recovery_attempts) == 1);
    assert(warnings == 2 && strstr(last_warning, "retrying for 3000 ms"));
    expect_retry_pending(&f, 1);

    // The retry: the device is still away, and the core seeks and resumes
    // while the attempt is being made.
    mp_mutex_lock(&gate);
    new_failures = 1;
    attempt_f = &f;
    hook_worker = f.p.thread;
    new_hook_fn = seek_during_the_attempt;
    unlock_hook_fn = let_the_seek_through;
    atomic_store(&unlock_hook_lock, &f.p.track_lock);
    int target = idle + 1;
    mp_mutex_unlock(&gate);
    advance_and_wake(&f, RECREATE_RETRY_NS);
    wait_counter(&idle, target);
    assert(!pthread_join(attempt_caller, NULL));
    assert(!new_hook_fn && !atomic_load(&unlock_hook_lock));
    assert(!errors && warnings == 2);
    mp_mutex_lock(&f.p.lock);
    assert(!f.p.audiotrack && !f.p.raw_pending);
    assert(f.p.recreate_deadline_ns == MP_TIME_S_TO_NS(1) + RECREATE_GRACE_NS);
    mp_mutex_unlock(&f.p.lock);
    assert(atomic_load(&f.p.recreate_requested));
    assert(atomic_load(&f.p.play_requested));

    // The device is back: the replacement is made playing, and the first
    // write to it is the burst the core has after the seek.
    mp_mutex_lock(&gate);
    target = entered + 1;
    memcpy(input, burst, sizeof(burst));
    input_bytes = sizeof(burst);
    mp_mutex_unlock(&gate);
    advance_and_wake(&f, RECREATE_RETRY_NS);
    wait_counter(&entered, target);
    mp_mutex_lock(&gate);
    assert(track_count == 2 && tracks[1].state == AudioTrack.PLAYSTATE_PLAYING);
    mp_mutex_unlock(&gate);
    assert(!atomic_load(&f.p.recreate_requested));
    assert(warnings == 3 && strstr(last_warning, "recreated once the output device was back"));
    finish_write(&f, sizeof(payload));
    assert(tracks[1].sink_size == (int)sizeof(payload));
    assert(!memcmp(tracks[1].sink, payload, sizeof(payload)));
    expect_delay(&f, sizeof(burst) / 2);
    assert(!errors && !failures);
    teardown(&f);
    puts("PASS: a seek and a resume during a retried attempt find the replacement "
         "pending, and the retry still makes it");
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

// --audio-stream-silence makes the core start the AO before it has audio. A
// bitstream track with stream silence is left paused by start() and started
// by the worker once its first frame has been written: nothing reaches the
// track before that frame, it starts primed rather than empty, and a
// replacement made meanwhile is not started either. Once feeding, a step
// that takes KA_STALL_LOG_NS or longer is logged by name.
static void check_stream_silence_deferred_start(void)
{
    struct fixture f;
    setup(&f, true, 5, true);
    mp_mutex_lock(&f.p.lock);
    f.ao.stream_silence = true;
    f.p.ka_enabled = true;
    f.p.ka_unit_51 = silence_ac3_51;
    f.p.ka_unit_51_len = 16; // a short unit keeps the fake buffers in bounds
    f.p.ka_unit_frames = 1536;
    mp_mutex_unlock(&f.p.lock);

    // The core starts the AO with nothing to play: the track stays paused
    // and the worker polls for audio instead of writing silence.
    mp_mutex_lock(&gate);
    int target = idle + 1;
    mp_mutex_unlock(&gate);
    start(&f.ao);
    wait_counter(&idle, target);
    mp_mutex_lock(&gate);
    assert(tracks[0].state == AudioTrack.PLAYSTATE_PAUSED && entered == 0);
    mp_mutex_unlock(&gate);
    assert(atomic_load(&f.p.play_deferred));

    mp_mutex_lock(&f.p.lock);
    assert(!AudioTrack_Recreate(&f.ao));
    mp_mutex_unlock(&f.p.lock);
    mp_mutex_lock(&gate);
    assert(track_count == 2 && tracks[1].state == AudioTrack.PLAYSTATE_PAUSED);

    // The first burst is written to the still-paused track, which starts
    // once that write has completed.
    target = entered + 1;
    memcpy(input, burst, sizeof(burst));
    input_bytes = sizeof(burst);
    mp_mutex_unlock(&gate);
    wait_counter(&entered, target);
    mp_mutex_lock(&gate);
    assert(tracks[1].state == AudioTrack.PLAYSTATE_PAUSED);
    mp_mutex_unlock(&gate);
    assert(atomic_load(&f.p.play_deferred));
    // Let the write complete with the track left playing. The core has
    // nothing more, so the worker goes straight on to a silent frame, and
    // that write (held by the device in turn) is the signal the first one
    // has completed.
    mp_mutex_lock(&gate);
    pause_after_write = false;
    response = sizeof(payload);
    permitted = entered;
    target = entered + 1;
    pthread_cond_broadcast(&changed);
    mp_mutex_unlock(&gate);
    wait_counter(&entered, target);
    mp_mutex_lock(&gate);
    assert(tracks[1].state == AudioTrack.PLAYSTATE_PLAYING);
    assert(tracks[1].sink_size == sizeof(payload));
    assert(!memcmp(tracks[1].sink, payload, sizeof(payload)));
    mp_mutex_unlock(&gate);
    assert(!atomic_load(&f.p.play_deferred));
    expect_delay(&f, sizeof(burst) / 2);
    assert(!warnings && !failures);

    // The fake clock advances 200 ms while the device holds the silent
    // frame, and the worker names the slow step once the write returns.
    atomic_fetch_add(&now_ns, MP_TIME_MS_TO_NS(200));
    pause_after_write = true;
    finish_write(&f, 16);
    assert(warnings == 1);
    assert(strstr(last_warning, "the silence write took 200 ms"));
    park_worker(&f);
    teardown(&f);
    puts("PASS: a stream-silence track starts primed with its first frame, and a slow feed step is named");
}

// A stream-silence track as AudioTrack_initKeepAlive leaves it. The silent
// frames are cut to a few bytes so that they fit the fake device, and the
// device takes every write without pausing the track, as a playing HAL does.
static void setup_stream_silence(struct fixture *f, bool raw, int format,
                                 const uint8_t *unit_20, const uint8_t *unit_51,
                                 int unit_frames)
{
    setup(f, raw, format, true);
    pause_after_write = false;
    mp_mutex_lock(&f->p.lock);
    f->ao.stream_silence = true;
    f->p.ka_enabled = true;
    f->p.ka_unit_20 = unit_20;
    f->p.ka_unit_20_len = 8;
    f->p.ka_unit_51 = unit_51;
    f->p.ka_unit_51_len = 16;
    f->p.ka_unit_frames = unit_frames;
    f->p.cfg_silence_limit = 60;
    mp_mutex_unlock(&f->p.lock);
}

// The device takes the outstanding write whole, `result` units of it, and
// the worker goes on to its next write, which the device holds in turn. The
// core hands out `next` meanwhile, or nothing: then the worker feeds silence.
// On return the sink holds the completed write alone.
static void take_write(int result, const uint8_t *next, int next_len)
{
    mp_mutex_lock(&gate);
    for (int n = 0; n < track_count; n++)
        tracks[n].sink_size = 0;
    if (next_len)
        memcpy(input, next, next_len);
    input_bytes = next_len;
    response = result;
    permitted = entered;
    int target = entered + 1;
    pthread_cond_broadcast(&changed);
    mp_mutex_unlock(&gate);
    wait_counter(&entered, target);
}

// As take_write, but the worker is expected to wait instead of writing: the
// pacing wait is an idle wait, and two of them prove a whole poll ran after
// the write completed.
static void take_write_then_wait(int result)
{
    mp_mutex_lock(&gate);
    for (int n = 0; n < track_count; n++)
        tracks[n].sink_size = 0;
    response = result;
    permitted = entered;
    int writes = entered, target = idle + 2;
    pthread_cond_broadcast(&changed);
    mp_mutex_unlock(&gate);
    wait_counter(&idle, target);
    mp_mutex_lock(&gate);
    assert(entered == writes);
    mp_mutex_unlock(&gate);
}

// The pacing wait the worker has just entered went half the way to the
// queue (`queued` frames at 48 kHz) draining to KA_AHEAD_NS, and no less
// than a poll: a wait of the whole way would bet on the smoothed head.
static void expect_pacing_wait(uint32_t queued)
{
    const int64_t drained_in = queued * MP_TIME_S_TO_NS(1) / 48000 - KA_AHEAD_NS;
    mp_mutex_lock(&gate);
    const int64_t wait = last_wait_ns;
    mp_mutex_unlock(&gate);
    assert(wait >= KA_POLL_NS && wait <= MPMAX(drained_in / 2, KA_POLL_NS));
}

// One IEC 61937 burst around a raw codec frame, as libavformat's spdif muxer
// emits it: little-endian preamble words with Pd in bits (AC-3, DTS) or in
// bytes (E-AC-3), then the byte-swapped frame. `n` is even.
static int raw_burst(uint8_t *out, int pc, const uint8_t *frame, int n)
{
    const uint16_t preamble[4] = {0xf872, 0x4e1f, pc,
                                  pc == IEC61937_EAC3 ? n : n * 8};
    int len = 0;
    for (int i = 0; i < 4; i++) {
        out[len++] = preamble[i] & 0xff;
        out[len++] = preamble[i] >> 8;
    }
    for (int i = 0; i < n; i += 2) {
        out[len++] = frame[i + 1];
        out[len++] = frame[i];
    }
    return len;
}

// park_worker completes the silence write the device holds; make room for
// it in the sink first.
static void park_fed_worker(struct fixture *f)
{
    mp_mutex_lock(&gate);
    for (int n = 0; n < track_count; n++)
        tracks[n].sink_size = 0;
    mp_mutex_unlock(&gate);
    park_worker(f);
}

static void expect_sink(int track, const uint8_t *bytes, int len)
{
    mp_mutex_lock(&gate);
    assert(tracks[track].sink_size == len);
    assert(!memcmp(tracks[track].sink, bytes, len));
    mp_mutex_unlock(&gate);
}

static void expect_sink_zeros(int track, int len)
{
    mp_mutex_lock(&gate);
    assert(tracks[track].sink_size == len);
    for (int i = 0; i < len; i++)
        assert(!tracks[track].sink[i]);
    mp_mutex_unlock(&gate);
}

// The raw codecs a track can stream silence for, with a 5.1 and a 2.0 frame
// header of each: AC-3 carries acmod after the syncinfo, E-AC-3 in its fourth
// header byte, DTS its amode across the seventh and eighth.
static const struct layout {
    const char *codec;
    int format, pc, unit_frames;
    const uint8_t *unit_20, *unit_51;
    uint8_t frame_51[10], frame_20[10];
} layouts[] = {
    {"AC-3", 5, IEC61937_AC3, 1536, silence_ac3_20, silence_ac3_51,
     {0x0b, 0x77, 0, 0, 0, 0x40, 0xe0, 0, 0, 0},
     {0x0b, 0x77, 0, 0, 0, 0x40, 0x40, 0, 0, 0}},
    {"E-AC-3", 6, IEC61937_EAC3, 1536, silence_eac3_20, silence_eac3_51,
     {0x0b, 0x77, 0, 0, 0x0e, 0x80, 0, 0, 0, 0},
     {0x0b, 0x77, 0, 0, 0x04, 0x80, 0, 0, 0, 0}},
    {"DTS", 7, IEC61937_DTS1, 512, silence_dts_20, silence_dts_51,
     {0x7f, 0xfe, 0x80, 0x01, 0, 0, 0, 0x02, 0x40, 0},
     {0x7f, 0xfe, 0x80, 0x01, 0, 0, 0, 0x00, 0x80, 0}},
};

// A raw track's silence follows the layout of the frames it carries: 5.1
// frames get the 5.1 silent frame, 2.0 frames the 2.0 one, read off each
// frame's header. Audio coming back ends the feed; the next gap starts one.
static void check_stream_silence_layout(const struct layout *l)
{
    const uint8_t *frame_51 = l->frame_51, *frame_20 = l->frame_20;
    const uint8_t *unit_20 = l->unit_20, *unit_51 = l->unit_51;
    const int unit_frames = l->unit_frames, pc = l->pc;
    struct fixture f;
    setup_stream_silence(&f, true, l->format, unit_20, unit_51, unit_frames);
    uint8_t wire[32];
    int len = raw_burst(wire, pc, frame_51, 10);

    // The first frame starts the track; the core then has nothing, and the
    // silence that follows is the 5.1 frame.
    begin_write(&f, wire, len);
    take_write(10, NULL, 0);
    expect_sink(0, frame_51, 10);
    mp_mutex_lock(&f.p.lock);
    assert(!f.p.ka_unit_stereo && f.p.ka_feeding && f.p.ka_feed_units == 1);
    mp_mutex_unlock(&f.p.lock);
    take_write(16, NULL, 0);
    expect_sink(0, unit_51, 16);
    expect_delay(&f, len / 2 + unit_frames);
    mp_mutex_lock(&f.p.lock);
    assert(f.p.ka_feed_units == 2);
    mp_mutex_unlock(&f.p.lock);

    // A 2.0 frame arrives: it ends the feed and the next gap gets 2.0 silence.
    len = raw_burst(wire, pc, frame_20, 10);
    take_write(16, wire, len);
    expect_sink(0, unit_51, 16);
    mp_mutex_lock(&f.p.lock);
    assert(f.p.ka_unit_stereo && !f.p.ka_feeding);
    mp_mutex_unlock(&f.p.lock);
    take_write(10, NULL, 0);
    expect_sink(0, frame_20, 10);
    mp_mutex_lock(&f.p.lock);
    assert(f.p.ka_feeding && f.p.ka_feed_units == 1);
    mp_mutex_unlock(&f.p.lock);
    take_write(8, NULL, 0);
    expect_sink(0, unit_20, 8);
    assert(!warnings && !failures);
    park_fed_worker(&f);
    teardown(&f);
    printf("PASS: %s silence follows the frames' layout, and audio ends the feed\n",
           l->codec);
}

// On the IEC 61937 carrier silence is zero words, written so that a burst a
// seek cut short is completed first and whole bursts of zeros follow, each
// in reads of the chunk; zeros between bursts leave the burst grid alone, so
// the next real burst is read on it.
static void check_stream_silence_burst_grid(void)
{
    struct fixture f;
    setup_stream_silence(&f, false, AudioFormat.ENCODING_IEC61937, NULL, NULL,
                         2048 / 2);
    // A DTS type I burst: 2048 bytes on the wire, of which the core delivers
    // one chunk and then 100 bytes before a seek cuts it.
    uint8_t head[256], tail[100];
    memset(head, 0x5a, sizeof(head));
    memcpy(head, iec_preamble, sizeof(iec_preamble));
    head[4] = IEC61937_DTS1;
    head[5] = 0;
    memset(tail, 0x5a, sizeof(tail));

    begin_write(&f, head, sizeof(head));
    take_write(128, tail, sizeof(tail));
    expect_sink(0, head, sizeof(head));
    mp_mutex_lock(&f.p.lock);
    assert(f.p.iec_burst == 2048 && f.p.iec_pos == 256 && !f.p.iec_lost);
    mp_mutex_unlock(&f.p.lock);
    take_write(50, NULL, 0);
    expect_sink(0, tail, sizeof(tail));
    mp_mutex_lock(&f.p.lock);
    assert(f.p.iec_pos == 356 && f.p.ka_feeding);
    mp_mutex_unlock(&f.p.lock);

    // 1692 bytes complete the burst: six chunks of zeros, then the 156 that
    // remain, and the grid is back at a boundary.
    for (int n = 1; n <= 6; n++) {
        take_write(128, NULL, 0);
        expect_sink_zeros(0, 256);
        mp_mutex_lock(&f.p.lock);
        assert(f.p.iec_pos == 356 + 256 * n);
        mp_mutex_unlock(&f.p.lock);
    }
    take_write(78, NULL, 0);
    expect_sink_zeros(0, 156);
    mp_mutex_lock(&f.p.lock);
    assert(f.p.iec_pos == 0 && !f.p.iec_lost);
    mp_mutex_unlock(&f.p.lock);

    // Between bursts, a whole chunk of zeros at a time, off the grid; the
    // next real burst then opens on a boundary and is tracked as one.
    take_write(128, head, sizeof(head));
    expect_sink_zeros(0, 256);
    mp_mutex_lock(&f.p.lock);
    assert(f.p.iec_pos == 0 && !f.p.ka_feeding);
    mp_mutex_unlock(&f.p.lock);
    take_write(128, NULL, 0);
    expect_sink(0, head, sizeof(head));
    mp_mutex_lock(&f.p.lock);
    assert(f.p.iec_pos == 256 && !f.p.iec_lost && f.p.ka_feeding);
    mp_mutex_unlock(&f.p.lock);
    expect_delay(&f, (256 + 100 + 1692 + 256 + 256) / 2);
    assert(!warnings && !failures);
    park_fed_worker(&f);
    teardown(&f);
    puts("PASS: carrier silence completes a cut burst, then keeps whole bursts "
         "on the grid");
}

// Pacing: silence goes in until KA_AHEAD_NS is queued ahead of the playback
// head, then the worker waits. A head that has not moved for
// KA_HEAD_STILL_NS is fed regardless, since a direct track that AudioFlinger
// holds back starts only on a full buffer; once it moves, pacing resumes.
static void expect_idle(void);

static void check_stream_silence_pacing(void)
{
    struct fixture f;
    setup_stream_silence(&f, true, 5, silence_ac3_20, silence_ac3_51, 1536);
    uint8_t wire[32];
    const int len = raw_burst(wire, IEC61937_AC3, layouts[0].frame_51, 10);
    const uint32_t ahead = 48000 * 300 / 1000;

    // The head sits at 0: ten 32 ms frames reach 300 ms, and the eleventh
    // waits.
    begin_write(&f, wire, len);
    take_write(10, NULL, 0);
    uint32_t written = len / 2;
    for (int n = 1; n < 10; n++) {
        take_write(16, NULL, 0);
        written += 1536;
        assert(written < ahead);
    }
    take_write_then_wait(16);
    written += 1536;
    assert(written >= ahead);
    expect_pacing_wait(written);
    expect_delay(&f, written);
    mp_mutex_lock(&f.p.lock);
    assert(f.p.ka_feeding && f.p.ka_feed_units == 10);
    mp_mutex_unlock(&f.p.lock);

    // 150 ms pass inside a pacing wait: a wait meant to take its time is
    // not a slow step, and the head has not stood still for long enough.
    atomic_fetch_add(&now_ns, MP_TIME_MS_TO_NS(150));
    expect_idle();
    assert(!warnings);

    // The clock passes KA_HEAD_STILL_NS with the head unmoved: the feed
    // resumes at 250 ms, not before.
    for (int step = 4; step <= 5; step++) {
        atomic_fetch_add(&now_ns, MP_TIME_MS_TO_NS(50));
        mp_mutex_lock(&gate);
        int writes = entered, polls = idle + 2;
        while (entered == writes && idle < polls)
            pthread_cond_wait(&changed, &gate);
        bool wrote = entered > writes;
        mp_mutex_unlock(&gate);
        assert(wrote == (step == 5));
    }
    take_write(16, NULL, 0); // and keeps going while it stands still
    written += 1536;
    expect_sink(0, silence_ac3_51, 16);
    take_write(16, NULL, 0);
    written += 1536;
    mp_mutex_lock(&f.p.lock);
    assert(f.p.ka_feed_units == 13); // the one the device holds included
    mp_mutex_unlock(&f.p.lock);

    // The head moves: 200 ms played leaves 184 ms queued; the frame the
    // device holds and three more reach 300 ms, and pacing waits again.
    mp_mutex_lock(&gate);
    tracks[0].head = 9600;
    mp_mutex_unlock(&gate);
    for (int n = 0; n < 3; n++) {
        take_write(16, NULL, 0);
        written += 1536;
        assert(written - 9600 < ahead);
    }
    take_write_then_wait(16);
    written += 1536;
    assert(written - 9600 >= ahead);
    expect_pacing_wait(written - 9600);
    expect_delay_played(&f, written, 9600);
    assert(!warnings && !failures);
    park_fed_worker(&f);
    teardown(&f);
    puts("PASS: silence is paced to 300 ms ahead of a moving head and fed to a "
         "still one");
}

// Two idle polls with no write between them: the worker is waiting for the
// core and nothing goes to the track.
static void expect_idle(void)
{
    mp_mutex_lock(&gate);
    int writes = entered, target = idle + 2;
    while (idle < target)
        pthread_cond_wait(&changed, &gate);
    assert(entered == writes);
    mp_mutex_unlock(&gate);
}

// A track stood down is paused and empty, armed for a deferred start, with
// no write outstanding and the clock cleared; nothing has failed or been
// charged to the recovery budget.
static void expect_stood_down(struct fixture *f, int track)
{
    mp_mutex_lock(&f->p.lock);
    assert(f->p.ka_stood_down && !atomic_load(&f->p.ka_feeding));
    mp_mutex_unlock(&f->p.lock);
    mp_mutex_lock(&gate);
    assert(tracks[track].state == AudioTrack.PLAYSTATE_PAUSED);
    assert(tracks[track].head == 0 && tracks[track].ts_fpos == 0); // flushed
    mp_mutex_unlock(&gate);
    assert(atomic_load(&f->p.play_deferred));
    assert(!atomic_load(&f->p.write_outstanding));
    assert(!atomic_load(&f->p.recreate_requested));
    assert(!atomic_load(&f->p.recovery_attempts) && !failures);
    assert(track_count == track + 1);
    expect_delay(f, 0);
    expect_idle();
}

// The core has audio again for a track stood down: the first burst is
// written to the paused track, which starts primed once it is in, and the
// feed resumes behind it as it did at the first start. Returns with the
// device holding the second silent frame.
static void resume_stood_down(struct fixture *f, int track, const uint8_t *wire,
                              int len)
{
    mp_mutex_lock(&gate);
    int target = entered + 1;
    memcpy(input, wire, len);
    input_bytes = len;
    mp_mutex_unlock(&gate);
    wait_counter(&entered, target);
    mp_mutex_lock(&gate);
    assert(tracks[track].state == AudioTrack.PLAYSTATE_PAUSED);
    mp_mutex_unlock(&gate);
    take_write(10, NULL, 0);
    mp_mutex_lock(&gate);
    assert(tracks[track].state == AudioTrack.PLAYSTATE_PLAYING);
    mp_mutex_unlock(&gate);
    assert(!atomic_load(&f->p.play_deferred));
    mp_mutex_lock(&f->p.lock);
    assert(!f->p.ka_stood_down && atomic_load(&f->p.ka_feeding));
    assert(f->p.ka_feed_units == 1);
    mp_mutex_unlock(&f->p.lock);
    expect_delay(f, len / 2);
    // The device takes that unit, a 5.1 frame after the burst's layout, and
    // holds the next; park_fed_worker completes it whole.
    take_write(16, NULL, 0);
    expect_sink(track, silence_ac3_51, 16);
}

// The feed is bounded by cfg_silence_limit seconds of silence per pause.
// When it is up the worker stands the track down: pauses and flushes it,
// clears the clock, arms the deferred start and writes nothing more until
// the core has audio, which then starts the track primed.
static void check_stream_silence_limit(void)
{
    struct fixture f;
    setup_stream_silence(&f, true, 5, silence_ac3_20, silence_ac3_51, 1536);
    mp_mutex_lock(&f.p.lock);
    f.p.cfg_silence_limit = 2;
    mp_mutex_unlock(&f.p.lock);
    uint8_t wire[32];
    const int len = raw_burst(wire, IEC61937_AC3, layouts[0].frame_51, 10);

    begin_write(&f, wire, len);
    take_write(10, NULL, 0); // the burst is in; the feed begins at t=1 s
    take_write(16, NULL, 0); // unit 1 is in; unit 2 is held by the device
    mp_mutex_lock(&f.p.lock);
    assert(atomic_load(&f.p.ka_feeding) && f.p.ka_feed_units == 2);
    assert(f.p.ka_feed_started_ns == MP_TIME_S_TO_NS(1));
    mp_mutex_unlock(&f.p.lock);

    // The device holds unit 2 for the rest of the limit (which the worker
    // names as a slow step) and plays some of the queue meanwhile; the
    // pass after it stands the track down instead of feeding unit 3, and
    // the flush takes the head back to 0.
    atomic_fetch_add(&now_ns, MP_TIME_S_TO_NS(2));
    mp_mutex_lock(&gate);
    int paused = pauses + 1;
    tracks[0].head = 480;
    mp_mutex_unlock(&gate);
    finish_write(&f, 16);
    wait_counter(&pauses, paused);
    assert(warnings == 1 && strstr(last_warning, "the silence write took 2000 ms"));
    expect_stood_down(&f, 0);

    resume_stood_down(&f, 0, wire, len);
    assert(warnings == 1 && !failures);
    park_fed_worker(&f);
    teardown(&f);
    puts("PASS: the silence limit stands the track down, and the next audio "
         "starts it primed");
}

// The device stops taking the silence (the receiver was turned off during
// the pause). The monitor's stall verdict stands the feed down instead of
// recreating the track: it pauses the track to release the worker from the
// write, charges nothing to the recovery budget, and the worker stands the
// track down; the next audio starts it primed and nothing has failed.
static void check_stream_silence_stall_stands_down(void)
{
    struct fixture f;
    setup_stream_silence(&f, true, 5, silence_ac3_20, silence_ac3_51, 1536);
    uint8_t wire[32];
    const int len = raw_burst(wire, IEC61937_AC3, layouts[0].frame_51, 10);
    begin_write(&f, wire, len);
    take_write(10, NULL, 0); // feeding; the device holds unit 1 and never takes it

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
    assert(atomic_load(&f.p.ka_stop_requested));
    assert(!atomic_load(&f.p.recreate_requested));
    assert(!atomic_load(&f.p.recovery_attempts));
    assert(warnings == 1 && strstr(last_warning, "standing the feed down"));
    assert(strstr(last_warning, "accepted no audio for 5000 ms"));
    // Stop the monitor before observing the worker's idle/completion boundary.
    atomic_store(&f.p.monitor_terminate, true);
    mp_cond_signal(&f.p.monitor_wakeup);
    assert(!pthread_join(f.p.monitor, NULL));
    f.p.monitor_created = false;

    // The interrupted write returns with nothing taken (one warning for the
    // short write, one for the slow step), and the worker stands down.
    finish_write(&f, 0);
    assert(warnings == 3 && strstr(last_warning, "the silence write took 5000 ms"));
    expect_stood_down(&f, 0);

    resume_stood_down(&f, 0, wire, len);
    assert(warnings == 3 && !failures);
    park_fed_worker(&f);
    teardown(&f);
    puts("PASS: a device that stops taking silence stands the feed down "
         "instead of recreating the track");
}

// A silence write that fails - the track died with the output device, as
// it does on HALs that invalidate the track when the receiver goes - stands
// the feed down as a stall does: nothing is charged to the recovery budget
// and nothing fails while the user is away. The next audio finds the dead
// track and recovers it the usual way, on the budget the silence left whole.
static void check_stream_silence_write_error_stands_down(void)
{
    struct fixture f;
    setup_stream_silence(&f, true, 5, silence_ac3_20, silence_ac3_51, 1536);
    uint8_t wire[32];
    const int len = raw_burst(wire, IEC61937_AC3, layouts[0].frame_51, 10);
    begin_write(&f, wire, len);
    take_write(10, NULL, 0); // feeding; the device holds unit 1
    mp_mutex_lock(&gate);
    int paused = pauses + 1;
    tracks[0].head = 480; // some of the queue played before the track died
    mp_mutex_unlock(&gate);
    finish_write(&f, AudioManager.ERROR_DEAD_OBJECT);
    wait_counter(&pauses, paused);
    assert(!warnings);
    expect_stood_down(&f, 0);

    // The next audio goes to the dead track, and that write's error is
    // recovered as any is: the track is replaced, one attempt charged.
    mp_mutex_lock(&gate);
    int target = entered + 1;
    memcpy(input, wire, len);
    input_bytes = len;
    mp_mutex_unlock(&gate);
    wait_counter(&entered, target);
    finish_write(&f, AudioManager.ERROR_DEAD_OBJECT);
    assert(track_count == 2 && !failures && !atomic_load(&f.p.failed));
    assert(atomic_load(&f.p.recovery_attempts) == 1);
    assert(atomic_load(&f.p.play_deferred)); // the replacement starts with its audio
    expect_delay(&f, 0);
    park_fed_worker(&f);
    teardown(&f);
    puts("PASS: a silence write that fails stands the track down, and the next "
         "audio recovers it on a whole budget");
}

// The monitor's stand-down is two steps, the ka_feeding check and the pause,
// and the device can take the write it holds between them, for the worker to
// go on meanwhile. The hook completes that silence write at the moment the
// monitor reaches for track_lock, with audio from the core behind it, and
// `completed` writes in all, so that the worker's answer to the request falls
// on either side of the feed ending for the audio:
//
// - 1: the pause cuts the burst's write short, not a silence write. The
//   worker finds the feed over, resumes the track rather than standing it
//   down, nothing is flushed, and the feed starts again behind the burst.
// - 2: the burst is in and the feed is on again when the pause lands. The
//   worker stands the track down, and the next audio starts it primed.
//
// Either way the track is not left paused with playback wanted and nothing
// to restart it, which is where a pause landing after the worker's answer
// would leave it: the request and the pause are one step under track_lock,
// and both answers take that lock.
static uint8_t race_wire[32];
static int race_len, race_completed;
static void complete_writes_with_audio(void)
{
    mp_mutex_lock(&gate);
    memcpy(input, race_wire, race_len);
    input_bytes = race_len;
    for (int n = 0; n < race_completed; n++) {
        response = n == 0 ? 16 : 10; // the silent frame, then the burst
        permitted = entered;
        int target = entered + 1;
        pthread_cond_broadcast(&changed);
        while (entered < target)
            pthread_cond_wait(&changed, &gate);
    }
    mp_mutex_unlock(&gate);
}

static void check_stream_silence_stand_down_meets_audio(int completed)
{
    struct fixture f;
    setup_stream_silence(&f, true, 5, silence_ac3_20, silence_ac3_51, 1536);
    race_len = raw_burst(race_wire, IEC61937_AC3, layouts[0].frame_51, 10);
    race_completed = completed;
    begin_write(&f, race_wire, race_len);
    take_write(10, NULL, 0); // feeding; the device holds unit 1
    mp_mutex_lock(&gate);
    tracks[0].head = 480;
    mp_mutex_unlock(&gate);

    assert(!pthread_create(&f.p.monitor, NULL, monitor_thread, &f.ao));
    f.p.monitor_created = true;
    mp_mutex_lock(&gate);
    int sampled = head_queries + 2;
    int interrupted = pauses + 1;
    hook_worker = f.p.thread;
    hook_fn = complete_writes_with_audio;
    hook_after_warning = &f.p.track_lock;
    mp_mutex_unlock(&gate);
    wait_counter(&head_queries, sampled);
    atomic_fetch_add(&now_ns, STALL_TIMEOUT_NS + 1);
    mp_cond_signal(&f.p.monitor_wakeup);
    wait_counter(&pauses, interrupted);
    assert(!atomic_load(&hook_lock) && !hook_after_warning);
    assert(warnings == 2 && strstr(last_warning, "the silence write took 5000 ms"));
    // Stop the monitor before observing the worker.
    atomic_store(&f.p.monitor_terminate, true);
    mp_cond_signal(&f.p.monitor_wakeup);
    assert(!pthread_join(f.p.monitor, NULL));
    f.p.monitor_created = false;

    if (completed == 1) {
        // The burst's write completes under the pause; the worker resumes
        // the track and feeds again behind the burst.
        take_write(10, NULL, 0);
        mp_mutex_lock(&gate);
        assert(tracks[0].state == AudioTrack.PLAYSTATE_PLAYING);
        assert(tracks[0].head == 480); // not flushed
        mp_mutex_unlock(&gate);
        assert(!atomic_load(&f.p.ka_stop_requested));
        assert(!atomic_load(&f.p.play_deferred));
        mp_mutex_lock(&f.p.lock);
        assert(!f.p.ka_stood_down && atomic_load(&f.p.ka_feeding));
        assert(f.p.ka_feed_units == 1); // the feed behind the burst, unit 1 held
        mp_mutex_unlock(&f.p.lock);
        expect_delay_played(&f, race_len / 2 + 1536 + race_len / 2, 480);
        take_write(16, NULL, 0); // the device takes that unit whole and holds the next
        expect_sink(0, silence_ac3_51, 16);
    } else {
        // The second feed's first unit completes under the pause; the
        // worker stands the track down, and the next audio starts it primed.
        finish_write(&f, 16);
        expect_stood_down(&f, 0);
        resume_stood_down(&f, 0, race_wire, race_len);
    }
    assert(warnings == 2 && !failures && !atomic_load(&f.p.recovery_attempts));
    park_fed_worker(&f);
    teardown(&f);
    printf("PASS: a stand-down request that meets the audio coming back %s\n",
           completed == 1 ? "resumes the track instead"
                          : "stands the track down for the next audio to start");
}

// The worker's answer to the request with the feed over for audio is play(),
// under track_lock. The monitor holds that lock from storing the request
// through the pause, so the answer waits for the pause and the track ends up
// playing; without the lock the answer could land between the two, and the
// pause then leave the track paused with playback wanted and nothing to
// restart it. Hook A (on the monitor reaching for track_lock, before the
// request): the device takes the silence write it held, with the core's
// audio behind it, so the feed ends and the worker is in the burst's write
// when the request is stored. Hook B (inside the monitor's pause, under
// track_lock): the device takes the burst's write, and the hook returns once
// the worker's answer is waiting for track_lock - or has called play(),
// which is the failure: the pause then lands on a track playing again.
static void resume_f_track_lock_reached(void)
{
    // With gate held: the hook's own waits release it.
    const int played = plays;
    response = 10; // the burst's write completes
    permitted = entered;
    pthread_cond_broadcast(&changed);
    while (!watched_waits && plays == played)
        pthread_cond_wait(&changed, &gate);
    atomic_store(&watched_lock, NULL);
}

static void check_stream_silence_resume_answer_waits_for_the_pause(void)
{
    struct fixture f;
    setup_stream_silence(&f, true, 5, silence_ac3_20, silence_ac3_51, 1536);
    race_len = raw_burst(race_wire, IEC61937_AC3, layouts[0].frame_51, 10);
    race_completed = 1;
    begin_write(&f, race_wire, race_len);
    take_write(10, NULL, 0); // feeding; the device holds unit 1
    mp_mutex_lock(&gate);
    tracks[0].head = 480;
    mp_mutex_unlock(&gate);

    assert(!pthread_create(&f.p.monitor, NULL, monitor_thread, &f.ao));
    f.p.monitor_created = true;
    mp_mutex_lock(&gate);
    int sampled = head_queries + 2;
    int interrupted = pauses + 1;
    hook_worker = f.p.thread;
    hook_fn = complete_writes_with_audio;
    hook_after_warning = &f.p.track_lock;
    pause_hook_fn = resume_f_track_lock_reached;
    watched_excluded = f.p.monitor;
    atomic_store(&watched_lock, &f.p.track_lock);
    int answered = entered + 2; // the burst's write, then the feed's next unit
    mp_mutex_unlock(&gate);
    wait_counter(&head_queries, sampled);
    atomic_fetch_add(&now_ns, STALL_TIMEOUT_NS + 1);
    mp_cond_signal(&f.p.monitor_wakeup);
    wait_counter(&pauses, interrupted);
    assert(!atomic_load(&hook_lock) && !hook_after_warning && !pause_hook_fn);
    assert(warnings == 2 && strstr(last_warning, "the silence write took 5000 ms"));
    atomic_store(&f.p.monitor_terminate, true);
    mp_cond_signal(&f.p.monitor_wakeup);
    assert(!pthread_join(f.p.monitor, NULL));
    f.p.monitor_created = false;

    // The pause has landed and the answer followed it: the track plays, the
    // feed is on again behind the burst, and the device holds its unit. (A
    // track left paused would have the worker idle instead.)
    mp_mutex_lock(&gate);
    int polls = idle;
    while (entered < answered && idle == polls)
        pthread_cond_wait(&changed, &gate);
    assert(entered == answered);
    assert(watched_waits == 1 && plays == 2); // the answer waited for the lock
    assert(tracks[0].state == AudioTrack.PLAYSTATE_PLAYING);
    assert(tracks[0].head == 480); // not flushed
    mp_mutex_unlock(&gate);
    assert(!atomic_load(&f.p.ka_stop_requested));
    assert(!atomic_load(&f.p.play_deferred));
    mp_mutex_lock(&f.p.lock);
    assert(!f.p.ka_stood_down && atomic_load(&f.p.ka_feeding));
    assert(f.p.ka_feed_units == 1);
    mp_mutex_unlock(&f.p.lock);
    expect_delay_played(&f, race_len / 2 + 1536 + race_len / 2, 480);
    take_write(16, NULL, 0); // the device takes that unit whole and holds the next
    expect_sink(0, silence_ac3_51, 16);
    assert(warnings == 2 && !failures && !atomic_load(&f.p.recovery_attempts));
    park_fed_worker(&f);
    teardown(&f);
    puts("PASS: the resumption that answers a stand-down request waits for the "
         "monitor's pause, and the track plays");
}

// A PCM track with the option on is padded by the core: a read with no audio
// comes back as a whole chunk of silence, written and credited whole. The
// padding counts as feeding: a device that stops taking it stands the track
// down instead of recreating it, a track that dies under it is stood down
// too, with nothing charged, and the next audio starts the track again,
// primed, or recovers a dead one on a whole budget.
static void check_stream_silence_pcm_padding(void)
{
    struct fixture f;
    setup(&f, false, 1, true);
    pause_after_write = false;
    mp_mutex_lock(&f.p.lock);
    f.ao.stream_silence = true;
    f.p.ka_pad_silence = true;
    mp_mutex_unlock(&f.p.lock);
    static const uint8_t audio[6] = {1, 2, 3, 4, 5, 6};
    const int chunk = sizeof(f.chunk);
    memset(f.chunk, 0xaa, chunk); // what a read that did not pad would leave

    // Six bytes of audio pad to a whole chunk, written and credited whole.
    begin_write(&f, audio, sizeof(audio));
    mp_mutex_lock(&gate);
    assert(last_write_count == chunk);
    mp_mutex_unlock(&gate);
    take_write(chunk, NULL, 0); // and the device holds a chunk of padding
    mp_mutex_lock(&gate);
    assert(tracks[0].sink_size == chunk && !memcmp(tracks[0].sink, audio, sizeof(audio)));
    for (int i = sizeof(audio); i < chunk; i++)
        assert(!tracks[0].sink[i]);
    assert(last_write_count == chunk);
    tracks[0].head = 64; // some of it played
    mp_mutex_unlock(&gate);
    expect_delay_played(&f, chunk / 2, 64);
    mp_mutex_lock(&f.p.lock);
    assert(!f.p.ka_enabled && atomic_load(&f.p.ka_feeding) && f.p.ka_feed_units == 1);
    mp_mutex_unlock(&f.p.lock);
    assert(atomic_load(&f.p.write_outstanding));

    // The device stops taking the padding: the verdict stands the track
    // down, and nothing is recreated or charged.
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
    assert(warnings == 1 && strstr(last_warning, "standing the feed down"));
    atomic_store(&f.p.monitor_terminate, true);
    mp_cond_signal(&f.p.monitor_wakeup);
    assert(!pthread_join(f.p.monitor, NULL));
    f.p.monitor_created = false;
    finish_write(&f, 0);
    expect_stood_down(&f, 0);

    // Audio again: written to the paused track, which starts primed once
    // it is in, and the padding follows it.
    mp_mutex_lock(&gate);
    int target = entered + 1;
    memcpy(input, audio, sizeof(audio));
    input_bytes = sizeof(audio);
    mp_mutex_unlock(&gate);
    wait_counter(&entered, target);
    mp_mutex_lock(&gate);
    assert(tracks[0].state == AudioTrack.PLAYSTATE_PAUSED && last_write_count == chunk);
    mp_mutex_unlock(&gate);
    take_write(chunk, NULL, 0);
    mp_mutex_lock(&gate);
    assert(tracks[0].state == AudioTrack.PLAYSTATE_PLAYING);
    tracks[0].head = 64; // some of it played
    mp_mutex_unlock(&gate);
    assert(!atomic_load(&f.p.play_deferred));
    expect_delay_played(&f, chunk / 2, 64);
    mp_mutex_lock(&f.p.lock);
    assert(!f.p.ka_stood_down && atomic_load(&f.p.ka_feeding) && f.p.ka_feed_units == 1);
    mp_mutex_unlock(&f.p.lock);

    // The track dies under the padding: stood down again, budget intact.
    // The next audio finds it dead and recovers it the usual way.
    mp_mutex_lock(&gate);
    interrupted = pauses + 1;
    mp_mutex_unlock(&gate);
    finish_write(&f, AudioManager.ERROR_DEAD_OBJECT);
    wait_counter(&pauses, interrupted);
    expect_stood_down(&f, 0);
    mp_mutex_lock(&gate);
    target = entered + 1;
    memcpy(input, audio, sizeof(audio));
    input_bytes = sizeof(audio);
    mp_mutex_unlock(&gate);
    wait_counter(&entered, target);
    finish_write(&f, AudioManager.ERROR_DEAD_OBJECT);
    assert(track_count == 2 && !failures && !atomic_load(&f.p.failed));
    assert(atomic_load(&f.p.recovery_attempts) == 1);
    // The replacement starts primed with the next audio, as a track stood
    // down does; the padding is not written to it meanwhile.
    assert(atomic_load(&f.p.play_deferred));
    mp_mutex_lock(&gate);
    assert(tracks[1].state == AudioTrack.PLAYSTATE_PAUSED);
    target = entered + 1;
    memcpy(input, audio, sizeof(audio));
    input_bytes = sizeof(audio);
    mp_mutex_unlock(&gate);
    wait_counter(&entered, target);
    take_write(chunk, NULL, 0); // the audio is in, the track plays, the padding follows
    mp_mutex_lock(&gate);
    assert(tracks[1].state == AudioTrack.PLAYSTATE_PLAYING);
    assert(tracks[1].sink_size == chunk && !memcmp(tracks[1].sink, audio, sizeof(audio)));
    mp_mutex_unlock(&gate);
    assert(!atomic_load(&f.p.play_deferred));
    expect_delay(&f, chunk / 2);
    assert(warnings == 2 && strstr(last_warning, "recreating the AudioTrack"));
    park_fed_worker(&f);
    teardown(&f);
    puts("PASS: a padded PCM track is written whole, stands down when the device "
         "stops taking the padding or the track dies, and restarts with audio");
}

// The worker has just entered a wait that goes half the way from `queued`
// frames (at 48 kHz) to `mark`, and no less than a poll.
static void expect_wait_half_to(uint32_t queued, int64_t mark)
{
    const int64_t drained_in = queued * MP_TIME_S_TO_NS(1) / 48000 - mark;
    mp_mutex_lock(&gate);
    const int64_t wait = last_wait_ns;
    mp_mutex_unlock(&gate);
    assert(wait >= KA_POLL_NS && llabs(wait - MPMAX(drained_in / 2, KA_POLL_NS)) <= 1000);
}

// Audio is paced too: with KA_QUEUE_NS queued and the head moving, the
// worker waits for the queue to drain before it reads more, so that no more
// than that plays out at a seek or a pause. A head that stands still gets
// the track filled as before.
static void check_stream_silence_queue_cap(void)
{
    struct fixture f;
    setup_stream_silence(&f, true, 5, silence_ac3_20, silence_ac3_51, 1536);
    uint8_t wire[32];
    const int len = raw_burst(wire, IEC61937_AC3, layouts[0].frame_51, 10);
    const uint32_t cap = 48000 * 500 / 1000;
    begin_write(&f, wire, len);

    // The queue reads 600 ms (the accounting is the test's to set) and the
    // core has a burst: the worker waits instead of reading it. The head at
    // 0 has just been seen, so it counts as moving.
    mp_mutex_lock(&f.p.lock);
    f.p.written_frames = cap + 4800;
    mp_mutex_unlock(&f.p.lock);
    mp_mutex_lock(&gate);
    memcpy(input, wire, len);
    input_bytes = len;
    response = 10;
    permitted = entered;
    int writes = entered, target = idle + 2;
    pthread_cond_broadcast(&changed);
    mp_mutex_unlock(&gate);
    wait_counter(&idle, target);
    mp_mutex_lock(&gate);
    assert(entered == writes && input_bytes == len); // not read
    mp_mutex_unlock(&gate);
    const uint32_t written = cap + 4800 + len / 2;
    expect_wait_half_to(written, KA_QUEUE_NS);
    expect_delay(&f, written);
    mp_mutex_lock(&f.p.lock);
    assert(!atomic_load(&f.p.ka_feeding));

    // Just over the mark (10 ms, under a 32 ms burst), the wait is half a
    // burst, not half the way: one wake per burst near the mark.
    f.p.written_frames = cap + 480;
    mp_mutex_unlock(&f.p.lock);
    expect_idle();
    mp_mutex_lock(&gate);
    assert(entered == writes && input_bytes == len); // still not read
    assert(last_wait_ns == MP_TIME_S_TO_NS(1) * 1536 / 48000 / 2);
    mp_mutex_unlock(&gate);
    mp_mutex_lock(&f.p.lock);
    f.p.written_frames = cap + 4800;
    mp_mutex_unlock(&f.p.lock);
    expect_idle();

    // 200 ms play: the queue is under the mark, and the burst is read and
    // written. With the core out of audio behind it and 400 ms queued, the
    // silence then paces as usual.
    mp_mutex_lock(&gate);
    tracks[0].head = 9600;
    target = entered + 1;
    mp_mutex_unlock(&gate);
    wait_counter(&entered, target);
    take_write_then_wait(10);
    expect_pacing_wait(written + len / 2 - 9600);
    mp_mutex_lock(&f.p.lock);
    assert(!atomic_load(&f.p.ka_feeding)); // the first unit waits for the queue
    mp_mutex_unlock(&f.p.lock);

    // Well over the mark again (the smoothed head drifts on while the raw
    // one stands), and the head has stood still for KA_HEAD_STILL_NS: the
    // burst is read and written regardless.
    mp_mutex_lock(&f.p.lock);
    f.p.written_frames = 9600 + cap + 24000;
    mp_mutex_unlock(&f.p.lock);
    mp_mutex_lock(&gate);
    memcpy(input, wire, len);
    input_bytes = len;
    target = idle + 2;
    writes = entered;
    mp_mutex_unlock(&gate);
    wait_counter(&idle, target);
    mp_mutex_lock(&gate);
    assert(entered == writes && input_bytes == len); // still held back
    target = entered + 1;
    mp_mutex_unlock(&gate);
    atomic_fetch_add(&now_ns, KA_HEAD_STILL_NS);
    wait_counter(&entered, target);
    mp_mutex_lock(&gate);
    assert(!input_bytes);
    mp_mutex_unlock(&gate);
    assert(!warnings && !failures);
    park_fed_worker(&f);
    teardown(&f);
    puts("PASS: audio is paced to 500 ms ahead of a moving head and fed to a "
         "still one");
}

// A track that gets no silence (its format has none) polls for the core's
// audio every KA_POLL_NS while it plays, as the worker does without the
// option. A track stood down backs off from KA_POLL_NS to KA_IDLE_NS.
static void check_stream_silence_polls(void)
{
    struct fixture f;
    setup(&f, true, 5, true);
    mp_mutex_lock(&f.p.lock);
    f.ao.stream_silence = true; // ka_enabled stays off: no silent frame
    mp_mutex_unlock(&f.p.lock);
    mp_mutex_lock(&gate);
    int target = idle + 3;
    mp_mutex_unlock(&gate);
    start(&f.ao);
    wait_counter(&idle, target);
    mp_mutex_lock(&gate);
    assert(tracks[0].state == AudioTrack.PLAYSTATE_PLAYING);
    assert(entered == 0 && last_wait_ns == KA_POLL_NS);
    mp_mutex_unlock(&gate);

    // Stood down: paused, armed for a deferred start, and the core without
    // audio. The waits double up to KA_IDLE_NS and stay there: nothing
    // wakes the worker for the core's next audio, so that cap is how late
    // the next audio can be written.
    mp_mutex_lock(&f.p.lock);
    f.p.ka_enabled = true;
    f.p.ka_stood_down = true;
    mp_mutex_unlock(&f.p.lock);
    atomic_store(&f.p.play_deferred, true);
    mp_mutex_lock(&gate);
    tracks[0].state = AudioTrack.PLAYSTATE_PAUSED;
    mp_mutex_unlock(&gate);
    int64_t last = KA_POLL_NS;
    int at_idle = 0;
    for (int n = 0; n < 40 && at_idle < 3; n++) {
        mp_mutex_lock(&gate);
        target = idle + 1;
        mp_mutex_unlock(&gate);
        wait_counter(&idle, target);
        mp_mutex_lock(&gate);
        const int64_t wait = last_wait_ns;
        mp_mutex_unlock(&gate);
        assert(wait == last || wait == MPMIN(last * 2, KA_IDLE_NS));
        assert(wait >= KA_POLL_NS && wait <= KA_IDLE_NS);
        if (wait == KA_IDLE_NS)
            at_idle++;
        last = wait;
    }
    assert(at_idle == 3 && last == MP_TIME_MS_TO_NS(50));
    mp_mutex_lock(&gate);
    assert(entered == 0);
    mp_mutex_unlock(&gate);
    assert(!warnings && !failures);
    teardown(&f);
    puts("PASS: a track without silence polls every 5 ms while it plays; a "
         "track stood down backs off to 50 ms");
}

// the rate the frames are encoded at; the IEC 61937 carrier gets zero words
// at any rate. Reads are sized to the format's largest burst where silence
// is on, and left alone where it is not.
static void check_stream_silence_init_rate(void)
{
    struct fixture f;
    setup(&f, true, 5, true);
    mp_mutex_lock(&f.p.lock);
    f.ao.stream_silence = true;
    f.ao.format = AF_FORMAT_S_AC3;
    f.p.samplerate = 48000;
    f.p.cfg_silence_limit = 60;
    AudioTrack_initKeepAlive(&f.ao);
    assert(f.p.ka_enabled && !f.p.ka_pad_silence);
    assert(f.p.ka_unit_51 == silence_ac3_51 && f.p.ka_unit_20 == silence_ac3_20);
    assert(f.p.ka_unit_frames == 1536 && f.p.chunksize == 1536 * 4);
    assert(!warnings);

    f.p.samplerate = 44100;
    f.p.chunksize = sizeof(f.chunk);
    AudioTrack_initKeepAlive(&f.ao);
    assert(!f.p.ka_enabled && f.p.chunksize == (int)sizeof(f.chunk));
    assert(warnings == 1 && strstr(last_warning, "48 kHz"));
    assert(strstr(last_warning, "44100 Hz"));

    f.p.raw_passthrough = false; // the carrier is rate-agnostic
    AudioTrack_initKeepAlive(&f.ao);
    assert(f.p.ka_enabled && f.p.ka_unit_frames == 1536 * 2);
    assert(f.p.chunksize == 1536 * 4 && warnings == 1);

    f.ao.format = 1; // a PCM track: the core pads its reads, no feed here
    f.p.chunksize = sizeof(f.chunk);
    AudioTrack_initKeepAlive(&f.ao);
    assert(!f.p.ka_enabled && f.p.ka_pad_silence && f.p.chunksize == (int)sizeof(f.chunk));
    f.ao.format = AF_FORMAT_S_AC3;

    f.ao.stream_silence = false; // and nothing changes with the option off
    f.p.chunksize = sizeof(f.chunk);
    AudioTrack_initKeepAlive(&f.ao);
    assert(!f.p.ka_enabled && !f.p.ka_pad_silence);
    assert(f.p.chunksize == (int)sizeof(f.chunk) && warnings == 1);
    f.p.raw_passthrough = true;
    mp_mutex_unlock(&f.p.lock);
    teardown(&f);
    puts("PASS: raw silent frames are enabled at 48 kHz only; the carrier at any rate");
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

// #1804's failure on a raw TrueHD track: the route took it and never plays
// it. A new track would stall the same way, so instead of the recreate every
// other track gets, the monitor demotes TrueHD: it flags every later TrueHD
// stream, stands the AO down, releases the writer and asks for a reload,
// after which init() refuses TrueHD and the core decodes it.
static void check_truehd_stall_demotes(void)
{
    struct fixture f;
    setup(&f, true, 14, true);
    f.ao.format = AF_FORMAT_S_TRUEHD; // the worker idles on the paused track
    atomic_store(&f.p.play_requested, true);
    atomic_store(&f.p.write_outstanding, true); // a write the device never took
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
    assert(reloads == 1 && atomic_load(thd_raw_failed()) && atomic_load(&f.p.failed));
    assert(!atomic_load(&f.p.recreate_requested) && !atomic_load(&f.p.recovery_attempts));
    assert(!failures && track_count == 1);
    atomic_store(&f.p.monitor_terminate, true);
    mp_cond_signal(&f.p.monitor_wakeup);
    assert(!pthread_join(f.p.monitor, NULL));
    f.p.monitor_created = false;
    atomic_store(&f.p.play_requested, false);
    teardown(&f);
    puts("PASS: a stalled raw TrueHD track demotes TrueHD to decoding instead of recreating");
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
    check_recreate_retries_while_device_away();
    check_recreate_gives_up_after_grace();
    check_recreate_retry_restarts_stall_clock();
    check_raw_dtshd_reopen_retry();
    check_recreate_stop_start_during_retry();
    check_recreate_request_meets_completed_write();
    check_recreate_stop_start_during_attempt();
    check_stream_silence_deferred_start();
    for (int n = 0; n < MP_ARRAY_SIZE(layouts); n++)
        check_stream_silence_layout(&layouts[n]);
    check_stream_silence_burst_grid();
    check_stream_silence_pacing();
    check_stream_silence_limit();
    check_stream_silence_stall_stands_down();
    check_stream_silence_write_error_stands_down();
    check_stream_silence_stand_down_meets_audio(1);
    check_stream_silence_stand_down_meets_audio(2);
    check_stream_silence_resume_answer_waits_for_the_pause();
    check_stream_silence_pcm_padding();
    check_stream_silence_queue_cap();
    check_stream_silence_polls();
    check_stream_silence_init_rate();
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
    check_truehd_stall_demotes(); // last: the demotion is process-wide
    return 0;
}
