// Compile the core's ao_set_paused (audio/out/buffer.c) for a pull AO with a
// hardware pause, and check when the resume restarts the core's audio clock:
// once the driver's set_pause(false) has returned, since ao_audiotrack holds
// it until a passthrough track's playhead moves again (Plezy #2302).
// test_audiotrack_timing.sh slices the function out of the patched tree.
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define MP_TIME_MS_TO_NS(v) ((int64_t)((v) * 1000000LL))

typedef pthread_mutex_t mp_mutex;
static void mp_mutex_lock(mp_mutex *m) { assert(!pthread_mutex_lock(m)); }
static void mp_mutex_unlock(mp_mutex *m) { assert(!pthread_mutex_unlock(m)); }
static int64_t now_ns;
static int64_t mp_time_ns(void) { return now_ns; }

// The members ao_set_paused touches, with buffer.c's names.
struct mp_pcm_state { int unused; };
struct buffer_state {
    mp_mutex lock;
    bool playing, paused, streaming, hw_paused, recover_pause;
    int64_t end_time_ns, queued_time_ns;
    struct mp_pcm_state prepause_state;
};
struct ao;
struct ao_driver {
    bool (*write)(struct ao *ao, void **data, int samples); // NULL: a pull AO
    bool (*set_pause)(struct ao *ao, bool paused);
    void (*reset)(struct ao *ao);
    void (*start)(struct ao *ao);
};
struct ao {
    const struct ao_driver *driver;
    struct buffer_state *buffer_state;
    bool stream_silence, untimed;
};

static bool ao_is_playing(struct ao *ao) { return ao->buffer_state->playing; }
static void ao_drain(struct ao *ao) { (void)ao; assert(!"no drain on a pause without eof"); }
static void get_dev_state(struct ao *ao, struct mp_pcm_state *state)
{
    (void)ao; (void)state;
    assert(!"a pull AO has no device state to save");
}
static void ao_wakeup(struct ao *ao) { (void)ao; }

#include "buffer_pause.inc"

// The device takes this long to play again after the resume is asked for.
#define RESTART_NS MP_TIME_MS_TO_NS(60)
static int64_t resumed_at;
static int pauses, resumes;

static bool set_pause(struct ao *ao, bool paused)
{
    (void)ao;
    if (paused) {
        pauses++;
    } else {
        now_ns += RESTART_NS;
        resumed_at = now_ns;
        resumes++;
    }
    return true;
}
static void reset(struct ao *ao) { (void)ao; assert(!"a hardware pause does not reset"); }
static void start(struct ao *ao) { (void)ao; assert(!"a hardware resume does not start"); }

int main(void)
{
    const struct ao_driver driver = {.set_pause = set_pause, .reset = reset, .start = start};
    struct buffer_state state = {.playing = true, .streaming = true};
    assert(!pthread_mutex_init(&state.lock, NULL));
    struct ao ao = {.driver = &driver, .buffer_state = &state};

    // 300 ms of audio are in the device when the pause comes.
    now_ns = MP_TIME_MS_TO_NS(1000);
    state.end_time_ns = now_ns + MP_TIME_MS_TO_NS(300);
    ao_set_paused(&ao, true, false);
    assert(pauses == 1 && state.hw_paused && state.paused);
    assert(state.queued_time_ns == MP_TIME_MS_TO_NS(300));

    now_ns += MP_TIME_MS_TO_NS(5000);
    ao_set_paused(&ao, false, false);
    assert(resumes == 1 && !state.hw_paused && !state.paused);
    // The 300 ms play out from when the device plays, not from the call.
    assert(state.end_time_ns == resumed_at + MP_TIME_MS_TO_NS(300));

    assert(!pthread_mutex_destroy(&state.lock));
    puts("PASS: a hardware resume restarts the core's audio clock when the driver returns");
    return 0;
}
