// vo_mediacodec's process-lifetime Choreographer vsync sampler, extracted
// verbatim by test_mediacodec_vsync.sh, driven by a model of Android 11's
// AChoreographer as android-11.0.0_r1 ships it
// (libs/nativedisplay/AChoreographer.cpp, libs/gui/DisplayEventDispatcher.cpp),
// the behavior a Fire TV on Fire OS 8 shows.
//
// That release reports one display mode switch to a refresh-rate callback
// twice: DisplayManager's refresh rate through handleRefreshRateUpdates, and
// SurfaceFlinger's config-changed event through dispatchConfigChanged; later
// R releases stopped the second. Each fires when its period differs from the
// last one either reported, and DisplayManager's comes from a float refresh
// rate. The second is delivered from inside a post the first one's callback
// makes: a frame callback due now, posted on the Choreographer's own thread,
// runs scheduleVsync at once, and scheduleVsync drains the display events
// already queued before it asks for the vsync. The sampler used to make that
// post under its lock, and the callback it re-entered waited on the lock its
// own thread held (plezy#2465).
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- The platform surface the sampler compiles against.

typedef struct AChoreographer AChoreographer;
typedef struct ALooper ALooper;
typedef struct AChoreographerFrameCallbackData {
    int64_t frame_time;
    int64_t presentation[2];
    size_t count;
} AChoreographerFrameCallbackData;
typedef void (*AChoreographer_frameCallback)(long frame_time, void *data);
typedef void (*AChoreographer_frameCallback64)(int64_t frame_time, void *data);
typedef void (*AChoreographer_vsyncCallback)(
    const AChoreographerFrameCallbackData *data, void *arg);
#define ALOOPER_POLL_ERROR (-4)

#include "mediacodec_vsync_types.inc"

typedef pthread_mutex_t mp_static_mutex;
#define MP_STATIC_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
typedef int mp_thread;
#define MP_THREAD_VOID void *
#define MP_THREAD_RETURN() return NULL
#define MP_VERBOSE(obj, ...) ((void)(obj))
#define MP_WARN(obj, ...) ((void)(obj))
#define VO_EVENT_WIN_STATE 1

struct priv {
    bool vsync_attached;
    int64_t vsync_sample;
};

struct vo {
    void *priv;
    unsigned win_state_events;
    unsigned wakeups;
};

static void vo_event(struct vo *vo, int event)
{
    if (event & VO_EVENT_WIN_STATE)
        vo->win_state_events++;
}

static void vo_wakeup(struct vo *vo)
{
    vo->wakeups++;
}

static int64_t now_ns;

static int64_t monotonic_now_ns(void)
{
    return now_ns;
}

// The only lock the sampler takes. A normal mutex taken again by the thread
// that holds it never returns; report that instead of hanging.
static _Thread_local int sampler_lock_depth;

static void mp_mutex_lock(mp_static_mutex *lock)
{
    if (sampler_lock_depth) {
        fprintf(stderr, "FAIL: vsync_sampler.lock taken again by the thread "
                        "that holds it; the sampler thread would wait on "
                        "itself forever\n");
        exit(1);
    }
    pthread_mutex_lock(lock);
    sampler_lock_depth++;
}

static void mp_mutex_unlock(mp_static_mutex *lock)
{
    sampler_lock_depth--;
    pthread_mutex_unlock(lock);
}

// --- Android 11's AChoreographer, as far as the sampler can observe it.

struct frame_callback {
    AChoreographer_frameCallback64 callback64;
    AChoreographer_vsyncCallback timeline;
    int64_t due;
};

static struct {
    bool own_thread;        // the caller runs on the Choreographer's Looper
    bool waiting_for_vsync; // DisplayEventDispatcher::mWaitingForVsync
    int64_t latest_period;  // Choreographer::mLatestVsyncPeriod
    int64_t signalled;      // gChoreographers.mLastKnownVsync (DisplayManager)
    int64_t queued[4];      // config-changed events on the receiver socket
    int nqueued;
    struct frame_callback pending[8];
    int npending;
    mp_refresh_rate_callback refresh;
    bool refresh_fired;     // RefreshRateCallback::firstCallbackFired
    unsigned drained;       // config changes delivered from inside a post
    bool timelines;         // API 33: frame timelines are available
} ch;

static void enter_choreographer(const char *entry)
{
    if (sampler_lock_depth) {
        fprintf(stderr, "FAIL: %s called with vsync_sampler.lock held\n", entry);
        exit(1);
    }
}

// Choreographer::dispatchConfigChanged (android-11.0.0_r1).
static void dispatch_config_changed(int64_t period)
{
    int64_t last = ch.latest_period;
    bool fired = ch.refresh_fired;
    ch.refresh_fired = true;
    if (ch.refresh && (!fired || (period > 0 && period != last)))
        ch.refresh(period, NULL);
    ch.latest_period = period;
}

// DisplayEventDispatcher::scheduleVsync: processPendingEvents first.
static void schedule_vsync(void)
{
    if (ch.waiting_for_vsync)
        return;
    while (ch.nqueued) {
        int64_t period = ch.queued[0];
        memmove(ch.queued, ch.queued + 1, --ch.nqueued * sizeof(ch.queued[0]));
        ch.drained++;
        dispatch_config_changed(period);
    }
    ch.waiting_for_vsync = true;
}

// Choreographer::postFrameCallbackDelayed. A callback due now schedules the
// vsync right there on the Choreographer's own thread; from another thread,
// or with a delay, it goes through the Looper (MSG_SCHEDULE_VSYNC,
// MSG_SCHEDULE_CALLBACKS), which deliver_vsync stands in for.
static void choreographer_post(const char *entry, AChoreographer_frameCallback64 callback64,
                 AChoreographer_vsyncCallback timeline, int64_t delay_ns)
{
    enter_choreographer(entry);
    if (ch.npending == (int)(sizeof(ch.pending) / sizeof(ch.pending[0]))) {
        fprintf(stderr, "FAIL: frame callbacks pile up without bound\n");
        exit(1);
    }
    ch.pending[ch.npending++] = (struct frame_callback){
        .callback64 = callback64,
        .timeline = timeline,
        .due = now_ns + delay_ns,
    };
    if (!delay_ns && ch.own_thread)
        schedule_vsync();
}

static AChoreographer *get_instance(void)
{
    enter_choreographer("AChoreographer_getInstance");
    return (AChoreographer *)&ch;
}

static void post_frame_callback64(AChoreographer *c,
                                  AChoreographer_frameCallback64 callback,
                                  void *data)
{
    choreographer_post("AChoreographer_postFrameCallback64", callback, NULL, 0);
}

static void post_frame_callback_delayed64(AChoreographer *c,
                                          AChoreographer_frameCallback64 callback,
                                          void *data, uint32_t delay_ms)
{
    choreographer_post("AChoreographer_postFrameCallbackDelayed64", callback, NULL,
         delay_ms * INT64_C(1000000));
}

static void register_refresh_rate_callback(AChoreographer *c,
                                           mp_refresh_rate_callback callback,
                                           void *data)
{
    enter_choreographer("AChoreographer_registerRefreshRateCallback");
    ch.refresh = callback;
}

// API 33: postFrameCallbackDelayed with a vsync callback, due now.
static void post_timeline_callback(AChoreographer *c,
                                AChoreographer_vsyncCallback callback,
                                void *data)
{
    choreographer_post("AChoreographer_postVsyncCallback", NULL, callback, 0);
}

static int64_t timeline_frame_time(const AChoreographerFrameCallbackData *data)
{
    return data->frame_time;
}

static size_t timeline_count(const AChoreographerFrameCallbackData *data)
{
    return data->count;
}

static int64_t timeline_presentation(const AChoreographerFrameCallbackData *data,
                                     size_t index)
{
    return data->presentation[index];
}

static void *fake_dlopen(const char *name, int flags)
{
    return strcmp(name, "libandroid.so") ? NULL : &ch;
}

static void *fake_dlsym(void *lib, const char *name)
{
    static const struct {
        const char *name;
        void *fn;
        bool timelines;
    } table[] = {
        {"AChoreographer_getInstance", (void *)get_instance, false},
        {"AChoreographer_postFrameCallback64", (void *)post_frame_callback64, false},
        {"AChoreographer_postFrameCallbackDelayed64",
         (void *)post_frame_callback_delayed64, false},
        {"AChoreographer_registerRefreshRateCallback",
         (void *)register_refresh_rate_callback, false},
        {"AChoreographer_postVsyncCallback", (void *)post_timeline_callback, true},
        {"AChoreographerFrameCallbackData_getFrameTimeNanos",
         (void *)timeline_frame_time, true},
        {"AChoreographerFrameCallbackData_getFrameTimelinesLength",
         (void *)timeline_count, true},
        {"AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos",
         (void *)timeline_presentation, true},
    };
    for (size_t n = 0; n < sizeof(table) / sizeof(table[0]); n++) {
        if (!strcmp(table[n].name, name))
            return !table[n].timelines || ch.timelines ? table[n].fn : NULL;
    }
    return NULL; // the long-typed API 24 entry points
}

static int fake_dlclose(void *lib)
{
    return 0;
}

#define dlopen fake_dlopen
#define dlsym fake_dlsym
#define dlclose fake_dlclose
#define RTLD_NOW 0
#define RTLD_LOCAL 0

// The sampler thread is run by the test, on its own schedule.
static MP_THREAD_VOID (*sampler_thread)(void *);
static void (*looper_script)(void);

static int mp_thread_create(mp_thread *thread, MP_THREAD_VOID (*fn)(void *),
                            void *arg)
{
    sampler_thread = fn;
    return 0;
}

static void mp_thread_detach(mp_thread thread)
{
}

static void mp_thread_set_name(const char *name)
{
}

static ALooper *ALooper_prepare(int opts)
{
    return (ALooper *)&ch;
}

// The Looper's whole life: the script's messages, then the loop ends.
static int ALooper_pollOnce(int timeout, int *fd, int *events, void **data)
{
    looper_script();
    return ALOOPER_POLL_ERROR;
}

#include "mediacodec_vsync.inc"

// --- Looper messages, on the Choreographer's own thread.

// Choreographer::handleRefreshRateUpdates (android-11.0.0_r1): what
// DisplayManager signalled through scheduleLatestConfigRequest.
static void handle_refresh_rate_updates(void)
{
    int64_t pending = ch.signalled, last = ch.latest_period;
    if (pending > 0)
        ch.latest_period = pending;
    bool fired = ch.refresh_fired;
    ch.refresh_fired = true;
    if (ch.refresh && (!fired || (pending > 0 && pending != last)))
        ch.refresh(pending, NULL);
}

// A vsync: every due frame callback, in posting order.
static void deliver_vsync(int64_t frame_time, const AChoreographerFrameCallbackData *timelines)
{
    ch.waiting_for_vsync = false;
    struct frame_callback due[8];
    int ndue = 0;
    for (int n = 0; n < ch.npending;) {
        if (ch.pending[n].due <= now_ns) {
            due[ndue++] = ch.pending[n];
            memmove(ch.pending + n, ch.pending + n + 1,
                    (--ch.npending - n) * sizeof(ch.pending[0]));
        } else {
            n++;
        }
    }
    for (int n = 0; n < ndue; n++) {
        if (due[n].timeline)
            due[n].timeline(timelines, NULL);
        else
            due[n].callback64(frame_time, NULL);
    }
}

// --- Checks.

static int failures;

static void check(bool ok, const char *what, int line)
{
    if (!ok) {
        fprintf(stderr, "FAIL (line %d): %s\n", line, what);
        failures++;
    }
}

#define CHECK(cond, what) check((cond), (what), __LINE__)

static int posted_now(void)
{
    int n = 0;
    for (int i = 0; i < ch.npending; i++)
        n += ch.pending[i].due <= now_ns;
    return n;
}

static void reset(bool timelines)
{
    memset(&ch, 0, sizeof(ch));
    ch.timelines = timelines;
    memset(&vsync_sampler, 0, sizeof(vsync_sampler));
    pthread_mutex_init(&vsync_sampler.lock, NULL);
    sampler_thread = NULL;
    now_ns = INT64_C(1000000000000);
}

// Starts the sampler thread for an attached vo and runs its Looper.
static void run_sampler_thread(void (*script)(void))
{
    looper_script = script;
    ch.own_thread = true;
    sampler_thread(NULL);
    ch.own_thread = false;
}

// DisplayManager's and SurfaceFlinger's periods for one mode.
#define P60_DISPLAY_MANAGER INT64_C(16666665)
#define P24_DISPLAY_MANAGER INT64_C(41666663)
#define P24_SURFACEFLINGER INT64_C(41666666)

static struct vo *script_vo;

// The switch that hung plezy#2465: the attached vo is playing at 60 Hz,
// DisplayManager signals the 24 Hz mode while SurfaceFlinger's
// config-changed event for it is already queued on the receiver.
static void script_mode_switch(void)
{
    struct priv *p = script_vo->priv;

    // Registration's own update: the current mode.
    ch.signalled = P60_DISPLAY_MANAGER;
    handle_refresh_rate_updates();
    CHECK(vsync_sampler.vsync_period == P60_DISPLAY_MANAGER,
          "the first refresh rate is taken");
    now_ns += 16 * INT64_C(1000000);
    deliver_vsync(now_ns, NULL);
    CHECK(p->vsync_sample == now_ns, "a vsync is sampled for the attached vo");
    CHECK(vsync_sampler.posted == 1 && ch.npending == 1,
          "one 500 ms resample stays armed");

    unsigned events = script_vo->win_state_events;
    ch.signalled = P24_DISPLAY_MANAGER;
    ch.queued[ch.nqueued++] = P24_SURFACEFLINGER;
    handle_refresh_rate_updates();

    CHECK(ch.drained == 1,
          "the config-changed event is delivered from inside the urgent post");
    CHECK(vsync_sampler.vsync_period == P24_SURFACEFLINGER,
          "the last period reported is the one kept");
    CHECK(script_vo->win_state_events == events + 2,
          "the vo is told about each new period");
    CHECK(p->vsync_sample == 0, "the stale sample is dropped");
    CHECK(vsync_sampler.posted == ch.npending,
          "every reservation reached the Choreographer");

    now_ns += 42 * INT64_C(1000000);
    int64_t frame_time = now_ns;
    deliver_vsync(frame_time, NULL);
    CHECK(p->vsync_sample == frame_time, "the new grid is sampled at once");
    CHECK(vsync_sampler.posted == 1 && ch.npending == 1 && !posted_now(),
          "back to one 500 ms resample");
}

static void test_mode_switch_reentry(void)
{
    reset(false);
    struct priv p = {0};
    struct vo vo = {.priv = &p};
    script_vo = &vo;
    init_vsync_sampler(&vo);
    CHECK(sampler_thread != NULL, "the sampler thread is started");
    CHECK(p.vsync_attached, "the vo is attached");
    run_sampler_thread(script_mode_switch);
    uninit_vsync_sampler(&vo);
}

static void script_nothing(void)
{
}

// The thread's first post is due now and made on its own thread, so a mode
// switch already queued is delivered from inside it, before the Looper runs.
static void test_startup_with_a_queued_switch(void)
{
    reset(false);
    struct priv p = {0};
    struct vo vo = {.priv = &p};
    init_vsync_sampler(&vo);
    ch.queued[ch.nqueued++] = P24_SURFACEFLINGER;
    run_sampler_thread(script_nothing);
    CHECK(ch.drained == 1, "the queued switch is delivered at startup");
    CHECK(vsync_sampler.vsync_period == P24_SURFACEFLINGER,
          "its period is taken");
    CHECK(vsync_sampler.posted == ch.npending,
          "every reservation reached the Choreographer");
    uninit_vsync_sampler(&vo);
}

static void script_timelines(void)
{
    struct priv *p = script_vo->priv;
    now_ns += 16 * INT64_C(1000000);
    deliver_vsync(now_ns, NULL);
    CHECK(vsync_sampler.posted == 1 && posted_now() == 1,
          "a vsync sample asks for the next vsync's timelines");
    now_ns += 16 * INT64_C(1000000);
    AChoreographerFrameCallbackData timelines = {
        .frame_time = now_ns,
        .presentation = {now_ns + 30000000, now_ns + 45708000},
        .count = 2,
    };
    deliver_vsync(now_ns, &timelines);
    CHECK(p->vsync_sample == now_ns, "the timelines' frame time is the sample");
    CHECK(vsync_sampler.vsync_spacing == 15708000,
          "the timelines' spacing is taken");
    CHECK(vsync_sampler.posted == 1 && ch.npending == 1 && !posted_now(),
          "then one 500 ms resample");

    // A new grid on the VO thread asks for a fresh sample at once.
    ch.own_thread = false;
    request_vsync_sample(script_vo);
    ch.own_thread = true;
    CHECK(vsync_sampler.posted == 2 && posted_now() == 1,
          "the VO's request joins the resample");
}

static void test_timelines_and_requests(void)
{
    reset(true);
    struct priv p = {0};
    struct vo vo = {.priv = &p};
    script_vo = &vo;
    init_vsync_sampler(&vo);
    run_sampler_thread(script_timelines);
    uninit_vsync_sampler(&vo);
}

static void script_detach(void)
{
    struct vo *vo = script_vo;
    uninit_vsync_sampler(vo);
    now_ns += 16 * INT64_C(1000000);
    deliver_vsync(now_ns, NULL);
    CHECK(((struct priv *)vo->priv)->vsync_sample == 0,
          "a detached vo is never sampled");
    CHECK(vsync_sampler.posted == 0 && ch.npending == 0,
          "nothing is posted without a client");
    ch.signalled = P24_DISPLAY_MANAGER;
    handle_refresh_rate_updates();
    CHECK(vsync_sampler.vsync_period == P24_DISPLAY_MANAGER &&
          ch.npending == 0 && vo->win_state_events == 0,
          "a period change without a client is only recorded");
}

static void test_detach(void)
{
    reset(false);
    struct priv p = {0};
    struct vo vo = {.priv = &p};
    script_vo = &vo;
    init_vsync_sampler(&vo);
    run_sampler_thread(script_detach);
}

int main(void)
{
    test_mode_switch_reentry();
    test_startup_with_a_queued_switch();
    test_timelines_and_requests();
    test_detach();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("mediacodec vsync sampler: all checks passed\n");
    return 0;
}
