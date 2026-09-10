// Exercises the production OSD init/uninit/rebind functions with a parked
// presentation worker. EGL/Android are substituted, never the lifetime code.
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OSD_SLOTS 3
#define MP_ERR(...) ((void)0)
#define MP_WARN(...) ((void)0)
typedef pthread_t mp_thread;
typedef pthread_mutex_t mp_mutex;
typedef pthread_cond_t mp_cond;
#define mp_mutex_init(m) pthread_mutex_init(m, NULL)
#define mp_mutex_lock pthread_mutex_lock
#define mp_mutex_unlock pthread_mutex_unlock
#define mp_mutex_destroy pthread_mutex_destroy
#define mp_cond_init(c) pthread_cond_init(c, NULL)
#define mp_cond_wait pthread_cond_wait
#define mp_cond_broadcast pthread_cond_broadcast
#define mp_cond_destroy pthread_cond_destroy

typedef void JNIEnv;
typedef void *jobject;
typedef struct window { int refs; bool entered, release; } ANativeWindow;
struct mediacodec_geometry { int width; };
struct mediacodec_opts { char *video_rect; int osd_vsync_delay; int64_t osd_surface; };
struct m_config_cache { struct mediacodec_opts *opts; bool dirty; };
struct osd_shared {
    bool terminate, paused;
    uint64_t epoch;
    struct mediacodec_geometry geometry;
    int vsync_delay, present_slot;
};
struct osd_slot { void *sbs; };
struct priv {
    int64_t osd_surface;
    ANativeWindow *osd_window;
    bool osd_threads_created, osd_missing_logged;
    mp_thread osd_render_thread, osd_present_thread;
    mp_mutex osd_lock;
    mp_cond osd_wakeup;
    struct osd_shared osd;
    struct osd_slot osd_slots[OSD_SLOTS];
    uint64_t prepared_frame_id, prepared_seq, prepared_epoch;
    uint32_t stats_frames;
    struct m_config_cache *opts_cache;
};
struct vo { struct priv *priv; void *log; bool want_redraw; };
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool joining_present, rebound, fail_present_once;
static int freed;
static mp_thread present_thread;

static JNIEnv *mp_jni_get_env(void *log) { (void)log; return (JNIEnv *)1; }
static ANativeWindow *ANativeWindow_fromSurface(JNIEnv *env, jobject object)
{
    (void)env;
    ANativeWindow *w = object;
    assert(w->refs == 0);
    w->refs++;
    return w;
}
static void ANativeWindow_release(ANativeWindow *w) { assert(w->refs == 1); w->refs--; }
static void talloc_free(void *p) { if (p) { freed++; free(p); } }
static void osd_log_stats(struct vo *vo, const char *label) { (void)vo; (void)label; }
static bool m_config_cache_update(struct m_config_cache *c)
{ bool dirty = c->dirty; c->dirty = false; return dirty; }
static bool mediacodec_geometry_parse(const char *s, struct mediacodec_geometry *g)
{ g->width = atoi(s); return true; }
static bool mediacodec_geometry_equal(struct mediacodec_geometry a, struct mediacodec_geometry b)
{ return a.width == b.width; }
static void osd_invalidate_locked(struct priv *p) { p->osd.epoch++; }

static void *osd_render_thread(void *arg)
{
    struct priv *p = ((struct vo *)arg)->priv;
    mp_mutex_lock(&p->osd_lock);
    while (!p->osd.terminate) mp_cond_wait(&p->osd_wakeup, &p->osd_lock);
    mp_mutex_unlock(&p->osd_lock);
    return NULL;
}
static void *osd_present_thread(void *arg)
{
    struct priv *p = ((struct vo *)arg)->priv;
    ANativeWindow *w = p->osd_window;
    pthread_mutex_lock(&gate);
    w->entered = true;
    pthread_cond_broadcast(&changed);
    // Represents an EGL call: it does not observe the OSD termination flag.
    while (!w->release) pthread_cond_wait(&changed, &gate);
    assert(w->refs == 1);
    pthread_mutex_unlock(&gate);
    return NULL;
}
static int mp_thread_create(mp_thread *t, void *(*fn)(void *), void *arg)
{
    if (fn == osd_present_thread && fail_present_once) {
        fail_present_once = false;
        return EAGAIN;
    }
    int result = pthread_create(t, NULL, fn, arg);
    if (!result && fn == osd_present_thread) present_thread = *t;
    return result;
}
static void mp_thread_join(mp_thread t)
{
    if (pthread_equal(t, present_thread)) {
        pthread_mutex_lock(&gate);
        joining_present = true;
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&gate);
    }
    assert(pthread_join(t, NULL) == 0);
}

#include "mediacodec_rebind.inc"

static void wait_for(bool *value)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    while (!*value) assert(pthread_cond_timedwait(&changed, &gate, &deadline) == 0);
}
static void *rebind(void *arg)
{
    update_opts(arg);
    pthread_mutex_lock(&gate);
    rebound = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    return NULL;
}
int main(void)
{
    ANativeWindow a = {0}, b = {0};
    struct mediacodec_opts opts = {.video_rect = "1920", .osd_vsync_delay = 2};
    struct m_config_cache cache = {.opts = &opts};
    struct priv p = {.opts_cache = &cache, .osd_surface = (intptr_t)&a,
                     .osd = {.paused = true, .geometry = {.width = 1920}, .vsync_delay = 2}};
    struct vo vo = {.priv = &p};
    osd_init(&vo);
    pthread_mutex_lock(&gate);
    wait_for(&a.entered);
    pthread_mutex_unlock(&gate);
    p.osd_slots[0].sbs = malloc(16);
    uint64_t epoch = p.osd.epoch;
    opts.osd_surface = (intptr_t)&b;
    cache.dirty = true;
    pthread_t handoff;
    assert(pthread_create(&handoff, NULL, rebind, &vo) == 0);
    pthread_mutex_lock(&gate);
    wait_for(&joining_present);
    assert(!rebound && a.refs == 1 && b.refs == 0 && freed == 0);
    a.release = true;
    pthread_cond_broadcast(&changed);
    wait_for(&rebound);
    wait_for(&b.entered);
    pthread_mutex_unlock(&gate);
    assert(pthread_join(handoff, NULL) == 0);
    assert(a.refs == 0 && b.refs == 1 && freed == 1 && p.osd_slots[0].sbs == NULL);
    assert(!p.osd.terminate && p.osd.paused && p.osd.epoch > epoch);
    assert(p.osd.geometry.width == 1920 && p.osd.vsync_delay == 2 && vo.want_redraw);
    pthread_mutex_lock(&gate);
    b.release = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    opts.osd_surface = 0;
    cache.dirty = true;
    update_opts(&vo);
    assert(!p.osd_threads_created && !p.osd_window && b.refs == 0);
    osd_uninit(&vo);
    assert(freed == 1);

    // Failed presenter startup must not leave terminate=true for its successor.
    fail_present_once = true;
    opts.osd_surface = (intptr_t)&a;
    cache.dirty = true;
    update_opts(&vo);
    assert(!p.osd_threads_created && !p.osd_window && a.refs == 0);
    opts.osd_surface = (intptr_t)&b;
    cache.dirty = true;
    update_opts(&vo);
    assert(p.osd_threads_created && !p.osd.terminate && b.refs == 1);
    osd_uninit(&vo);
    assert(b.refs == 0);
    puts("PASS: OSD rebind waits for producer retirement, releases once, and restarts without stale state");
    return 0;
}
