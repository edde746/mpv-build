// Behavioral regression for the frame vo.c keeps for redraws at the end of a
// file (patch 0139), extracted by test_player_release_held_frame.sh from
// player/video.c and video/out/vo.c. The decoder is a reference count that
// every frame it made shares, like FFmpeg's MediaCodec context, which deletes
// the codec with its last output buffer.
//
// What must hold: under vo_mediacodec, tearing down the video chain leaves no
// frame holding the decoder, so it is deleted before the next file's decoder
// opens. A MediaTek MT5895 runs one HEVC or Dolby Vision decoder at a time;
// with the old one alive the next fails, and the file loses HDR on a GL VO
// (plezy#2755). The decoder is deleted on the VO thread with the VO's lock
// released. A VO without the cap keeps its frame for redraws, as upstream.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define VO_MAX_REQ_FRAMES 10
#define MPV_EVENT_VIDEO_RECONFIG 17

#include "release_held_frame_types.inc"

// --- fakes: the fields and calls the extracted functions reach.

static int failures;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL: " __VA_ARGS__);                             \
            fputc('\n', stderr);                                               \
            failures++;                                                        \
        }                                                                      \
    } while (0)

// The decoder context: alive while the decoder filter or any frame holds it.
struct decoder {
    int refs;
    bool deleted;
    bool deleted_on_vo_thread;
    bool deleted_under_vo_lock;
};

struct mp_image { struct decoder *dec; };

struct vo_frame {
    int num_vsyncs;
    bool display_synced;
    struct mp_image *current;
};

struct mp_dispatch_queue { bool running; int runs; };

struct vo_internal {
    struct mp_dispatch_queue *dispatch;
    int lock;
    bool hasframe, hasframe_rendered, send_reset;
    int64_t drop_count, delayed_count;
    uint64_t current_frame_id;
    struct vo_frame *current_frame, *frame_queued;
};

struct vo_driver { int caps; };
struct vo { struct vo_internal *in; const struct vo_driver *driver; };

struct vo_chain { struct vo *vo; struct decoder *dec; };

struct MPContext {
    struct vo_chain *vo_chain;
    enum playback_status video_status;
    int reconfig_events;
};

static void mp_mutex_lock(int *lock) { (*lock)++; }
static void mp_mutex_unlock(int *lock) { (*lock)--; }
static void reset_vsync_timings(struct vo *vo) {}
static void wakeup_locked(struct vo *vo) {}

// The VO thread runs the dispatched call before the core continues.
static void mp_dispatch_run(struct mp_dispatch_queue *q, void (*fn)(void *), void *arg)
{
    q->running = true;
    q->runs++;
    fn(arg);
    q->running = false;
}

static struct vo *the_vo;

static void decoder_unref(struct decoder *dec)
{
    if (--dec->refs > 0)
        return;
    dec->deleted = true;
    dec->deleted_on_vo_thread = the_vo->in->dispatch->running;
    dec->deleted_under_vo_lock = the_vo->in->lock != 0;
}

static void talloc_free(struct vo_frame *frame)
{
    if (!frame)
        return;
    if (frame->current) {
        decoder_unref(frame->current->dec);
        free(frame->current);
    }
    free(frame);
}

static void mp_notify(struct MPContext *mpctx, int event, void *arg)
{
    if (event == MPV_EVENT_VIDEO_RECONFIG)
        mpctx->reconfig_events++;
}

void vo_seek_reset(struct vo *vo);

// What reset_video_state() does to the VO (vo_chain_reset_state).
static void reset_video_state(struct MPContext *mpctx)
{
    vo_seek_reset(mpctx->vo_chain->vo);
}

// Destroying the decoder filter drops its own reference to the context.
static void vo_chain_uninit(struct vo_chain *vo_c)
{
    decoder_unref(vo_c->dec);
    free(vo_c);
}

#include "release_held_frame.inc"

// --- rig: a file playing, its last frame on screen and the next one queued.

struct rig {
    struct mp_dispatch_queue dispatch;
    struct vo_internal in;
    struct vo_driver driver;
    struct vo vo;
    struct decoder dec;
    struct MPContext mpctx;
};

static struct vo_frame *new_frame(struct decoder *dec)
{
    struct vo_frame *frame = calloc(1, sizeof(*frame));
    struct mp_image *image = calloc(1, sizeof(*image));
    if (!frame || !image)
        abort();
    frame->current = image;
    frame->current->dec = dec;
    frame->num_vsyncs = 1;
    frame->display_synced = true;
    dec->refs++;
    return frame;
}

static void rig_init(struct rig *r, int caps)
{
    *r = (struct rig){0};
    r->driver.caps = caps;
    r->in.dispatch = &r->dispatch;
    r->vo = (struct vo){ .in = &r->in, .driver = &r->driver };
    the_vo = &r->vo;
    r->dec.refs = 1; // the decoder filter's
    r->in.current_frame = new_frame(&r->dec);
    r->in.frame_queued = new_frame(&r->dec);
    r->in.hasframe = r->in.hasframe_rendered = true;
    struct vo_chain *vo_c = calloc(1, sizeof(*vo_c));
    if (!vo_c)
        abort();
    *vo_c = (struct vo_chain){ .vo = &r->vo, .dec = &r->dec };
    r->mpctx = (struct MPContext){ .vo_chain = vo_c, .video_status = STATUS_PLAYING };
}

static void rig_free(struct rig *r)
{
    talloc_free(r->in.current_frame);
    talloc_free(r->in.frame_queued);
}

int main(void)
{
    struct rig r;

    // vo_mediacodec at the end of a file: nothing may hold the decoder once
    // the chain is down, or the next file's decoder opens against it.
    rig_init(&r, VO_CAP_FRAMEDROP | VO_CAP_OWN_TIMING | VO_CAP_CLIENT_FALLBACK |
                 VO_CAP_FRAMES_HOLD_DECODER);
    uninit_video_chain(&r.mpctx);
    CHECK(r.dec.deleted, "mediacodec: the decoder outlives its video chain (%d refs left)", r.dec.refs);
    CHECK(!r.in.current_frame && !r.in.frame_queued, "mediacodec: the VO still holds a frame");
    CHECK(r.dec.deleted_on_vo_thread, "mediacodec: the decoder was deleted off the VO thread");
    CHECK(!r.dec.deleted_under_vo_lock, "mediacodec: the decoder was deleted under the VO lock");
    CHECK(!r.in.hasframe && !r.in.hasframe_rendered, "mediacodec: the VO still reports a frame");
    CHECK(r.in.lock == 0, "mediacodec: the VO lock is left held (%d)", r.in.lock);
    CHECK(!r.mpctx.vo_chain && r.mpctx.video_status == STATUS_EOF && r.mpctx.reconfig_events == 1,
          "mediacodec: the chain teardown itself changed");
    rig_free(&r);

    // A GL VO redraws the frame it showed until the next file's first one:
    // it keeps that frame, and the decoder with it, as upstream does.
    rig_init(&r, VO_CAP_ROTATE90 | VO_CAP_FILM_GRAIN | VO_CAP_VFLIP);
    uninit_video_chain(&r.mpctx);
    CHECK(r.in.current_frame && r.in.current_frame->current,
          "gpu: the frame kept for redraws was released");
    CHECK(!r.dec.deleted && r.dec.refs == 1, "gpu: decoder refs %d, want the redraw frame's 1", r.dec.refs);
    CHECK(!r.in.frame_queued, "gpu: the frame queued for the ended file survived");
    CHECK(r.dispatch.runs == 0, "gpu: the core waited on the VO thread %d time(s)", r.dispatch.runs);
    rig_free(&r);

    // No video chain (audio-only file or video deselected): nothing to do.
    rig_init(&r, VO_CAP_FRAMES_HOLD_DECODER);
    struct vo_chain *vo_c = r.mpctx.vo_chain;
    r.mpctx.vo_chain = NULL;
    uninit_video_chain(&r.mpctx);
    CHECK(r.dispatch.runs == 0 && r.in.current_frame && r.mpctx.reconfig_events == 0,
          "no chain: the teardown reached a VO it does not own");
    free(vo_c);
    rig_free(&r);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("player held-frame release harness: ok\n");
    return 0;
}
