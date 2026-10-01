// Behavioral regression for patch 0034, extracted by
// test_audio_reload_held_frame.sh from player/audio.c: reload_audio_output()
// and the ao filter's process function, ao_process().
//
// What must hold: when the audio output reloads (audio-exclusive,
// audio-channels, audio-device ... are UPDATE_AUDIO) while the output chain
// has already handed the old AO a frame that is still waiting in the ao
// filter's input, the next AO still opens. That needs a data request to reach
// the output chain: the chain only reports a new format, and so only asks for
// a new AO, while it is asked for data. Before 0034 the frame stayed on the
// pin, ao_process() only peeked at it, no request left the ao filter, and
// audio stayed silent for good (plezy#2530).
//
// The pin fakes follow filters/filter.c: a request only propagates from a pin
// that holds no frame, mp_pin_out_read() requests first and then takes the
// frame, and mp_filter_reset() drops frames and requests held on the filter's
// pins (reset_pin()). A filter's process function only runs once something
// wakes it.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MP_NOPTS_VALUE (-0x1p+63)
#define MP_ERR(...) ((void)0)
#define MP_VERBOSE(...) ((void)0)

enum mp_frame_type { MP_FRAME_NONE = 0, MP_FRAME_AUDIO, MP_FRAME_EOF };
struct mp_frame { enum mp_frame_type type; void *data; };
#define MP_NO_FRAME ((struct mp_frame){0})
#define MP_EOF_FRAME ((struct mp_frame){.type = MP_FRAME_EOF})

struct mp_log;
struct mp_aframe { double pts; int samples; };
struct mp_async_queue;
struct mp_decoder_wrapper;
struct track { struct mp_decoder_wrapper *dec; };

struct mp_pin { struct mp_frame data; bool data_requested; };
struct mp_filter {
    void *priv;
    struct mp_pin *ppins[1]; // the filter's internal side of its input
    struct mp_pin *pins[1];  // the queue filter's input, for ao_process()
    int wakeups;
};
struct mp_output_chain { int resets; };
struct ao { int unused; };

#include "reload_held_frame_types.inc"

struct MPOpts { int unused; };
struct MPContext {
    struct MPOpts *opts;
    struct ao *ao;
    struct ao_chain *ao_chain;
    enum playback_status audio_status, video_status;
    double play_dir, audio_speed, delay;
    int64_t shown_aframes;
};

static int unrefs;
static int queue_writes;

// --- fakes: filters/filter.c and friends, as far as the extracted code reaches.

static struct mp_output_chain chain;

static bool mp_pin_out_request_data(struct mp_pin *p)
{
    if (p->data.type)
        return true;
    p->data_requested = true;
    return false;
}

static struct mp_frame mp_pin_out_read(struct mp_pin *p)
{
    if (!mp_pin_out_request_data(p))
        return MP_NO_FRAME;
    struct mp_frame res = p->data;
    p->data = MP_NO_FRAME;
    p->data_requested = false;
    return res;
}

static void mp_pin_out_unread(struct mp_pin *p, struct mp_frame frame)
{
    p->data = frame;
}

static bool mp_pin_can_transfer_data(struct mp_pin *dst, struct mp_pin *src)
{
    (void)dst;
    return mp_pin_out_request_data(src);
}

static bool mp_pin_in_write(struct mp_pin *p, struct mp_frame frame)
{
    (void)p;
    (void)frame;
    queue_writes++;
    return true;
}

static void mp_frame_unref(struct mp_frame *frame)
{
    if (frame->type)
        unrefs++;
    *frame = MP_NO_FRAME;
}

static void mp_filter_reset(struct mp_filter *f)
{
    for (int n = 0; n < 1; n++) {
        mp_frame_unref(&f->ppins[n]->data);
        f->ppins[n]->data_requested = false;
    }
}

static void mp_filter_wakeup(struct mp_filter *f) { f->wakeups++; }
static void mp_filter_internal_mark_progress(struct mp_filter *f) { (void)f; }
static bool mp_async_queue_is_full(struct mp_async_queue *q) { (void)q; return true; }
static double get_play_end_pts(struct MPContext *mpctx) { (void)mpctx; return MP_NOPTS_VALUE; }
static double mp_aframe_get_pts(struct mp_aframe *a) { return a->pts; }
static void mp_aframe_clip_timestamps(struct mp_aframe *a, double s, double e) { (void)a; (void)s; (void)e; }
static int mp_aframe_get_size(struct mp_aframe *a) { return a->samples; }
static double mp_aframe_get_rate(struct mp_aframe *a) { (void)a; return 48000; }
static double mp_aframe_end_pts(struct mp_aframe *a) { return a->pts; }
static void update_throttle(struct MPContext *mpctx) { (void)mpctx; }
static bool ao_is_playing(struct ao *ao) { (void)ao; return false; }
static void mp_wakeup_core(struct MPContext *mpctx) { (void)mpctx; }

static void ao_reset(struct ao *ao) { (void)ao; }
// player/audio.c: frees the AO queue and filter and forgets the AO.
static void uninit_audio_out(struct MPContext *mpctx)
{
    if (mpctx->ao_chain) {
        mpctx->ao_chain->ao_queue = NULL;
        mpctx->ao_chain->queue_filter = NULL;
        mpctx->ao_chain->ao = NULL;
    }
    mpctx->ao = NULL;
}
static int reinit_audio_filters(struct MPContext *mpctx) { (void)mpctx; return 1; }
static void reset_audio_state(struct MPContext *mpctx) { mpctx->audio_status = STATUS_SYNCING; }
static void mp_output_chain_reset_harder(struct mp_output_chain *c) { c->resets++; }
static void mp_decoder_wrapper_set_spdif_flag(struct mp_decoder_wrapper *d, bool f) { (void)d; (void)f; }
static bool mp_decoder_wrapper_reinit(struct mp_decoder_wrapper *d) { (void)d; return true; }
static void error_on_track(struct MPContext *mpctx, struct track *t) { (void)mpctx; (void)t; }

static void ao_process(struct mp_filter *f);
void reload_audio_output(struct MPContext *mpctx);

#include "reload_held_frame.inc"

// --- harness

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

struct rig {
    struct MPOpts opts;
    struct ao ao;
    struct MPContext mpctx;
    struct ao_chain ao_c;
    struct mp_filter ao_filter, queue_filter;
    struct mp_pin in, queue_in;
    struct mp_aframe frame;
};

static void rig_init(struct rig *r)
{
    memset(r, 0, sizeof(*r));
    memset(&chain, 0, sizeof(chain));
    unrefs = queue_writes = 0;
    r->mpctx.opts = &r->opts;
    r->mpctx.ao = &r->ao;
    r->mpctx.ao_chain = &r->ao_c;
    r->mpctx.audio_status = STATUS_PLAYING;
    r->mpctx.video_status = STATUS_PLAYING;
    r->mpctx.play_dir = 1;
    r->mpctx.audio_speed = 1;
    r->ao_c.mpctx = &r->mpctx;
    r->ao_c.filter = &chain;
    r->ao_c.ao = &r->ao;
    r->ao_c.queue_filter = &r->queue_filter;
    r->ao_c.ao_filter = &r->ao_filter;
    r->ao_c.start_pts_known = true;
    r->queue_filter.pins[0] = &r->queue_in;
    r->ao_filter.priv = &r->ao_c;
    r->ao_filter.ppins[0] = &r->in;
    r->frame = (struct mp_aframe){.pts = 10.0, .samples = 1024};
}

// What the filter scheduler does after the reload: run the ao filter if
// something woke it.
static void run_woken(struct rig *r)
{
    if (r->ao_filter.wakeups)
        ao_process(&r->ao_filter);
}

// The output chain only converts a frame for the next AO, and so only notices
// that it needs one, while the ao filter's input asks it for data.
static bool chain_is_asked(struct rig *r)
{
    return r->in.data_requested && !r->in.data.type;
}

int main(void)
{
    struct rig r;

    // The chain handed the old AO a frame that is still waiting in the ao
    // filter's input (the AO queue was full, or audio-spdif had just rebuilt
    // the chain on top of the still-open AO). audio-exclusive then reloads
    // the AO.
    rig_init(&r);
    r.in.data = (struct mp_frame){MP_FRAME_AUDIO, &r.frame};
    reload_audio_output(&r.mpctx);
    CHECK(r.ao_filter.wakeups > 0,
          "held frame: the reload does not wake the ao filter; nothing runs it again");
    run_woken(&r);
    CHECK(chain_is_asked(&r),
          "held frame: the output chain is not asked for data after the reload, "
          "so no new AO is ever opened (plezy#2530)");
    CHECK(unrefs == 1, "held frame: the frame meant for the old AO was released %d times, "
          "expected once", unrefs);
    CHECK(queue_writes == 0, "held frame: ao_process() wrote to a queue that no longer exists");

    // The common case, which recovered before 0034 too: the ao filter was
    // waiting on the chain with nothing parked. It must still be asking after
    // the reload, and nothing is dropped.
    rig_init(&r);
    r.in.data_requested = true;
    reload_audio_output(&r.mpctx);
    run_woken(&r);
    CHECK(chain_is_asked(&r), "empty pin: the output chain is not asked for data after the reload");
    CHECK(unrefs == 0, "empty pin: the reload released %d frames that did not exist", unrefs);

    // No audio chain (audio deselected): the reload must not touch it.
    rig_init(&r);
    r.mpctx.ao_chain = NULL;
    reload_audio_output(&r.mpctx);
    CHECK(r.ao_filter.wakeups == 0 && unrefs == 0,
          "no chain: the reload reached an ao filter that is not in use");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("audio reload held-frame harness: ok\n");
    return 0;
}
