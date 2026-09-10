// Exercise the production request dispatcher with immutable subtitle snapshots.
// Rendering, clocks and locks are synchronous host seams; selection, slot
// ownership, queueing and invalidation are extracted verbatim from patch 0001.
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mediacodec_osd_core.inc"
#include "mediacodec_osd_types.inc"

#define MP_VERBOSE(vo, ...) ((void)(vo), (void)fprintf(stderr, __VA_ARGS__))

struct osd_shared {
    struct osd_prefetch prefetch;
    struct osd_spec spec;
    struct osd_result_queue results;
    struct osd_release releases[OSD_RELEASE_HISTORY];
    int release_next, present_slot;
    uint64_t release_seq, epoch;
    bool terminate, present_failed, requested;
    struct osd_request request;
    unsigned stale, coalesced;
};
struct sub_bitmap_list { int change_id; };
struct osd_slot { struct sub_bitmap_list *sbs; };
struct osd_render_state {
    bool force_full;
    int64_t posted_change_id;
    struct osd_request last;
};
struct priv {
    struct osd_shared osd;
    struct osd_slot osd_slots[OSD_SLOTS];
    int osd_lock, osd_wakeup;
    double osd_pts;
    bool long_sign;
    // Host-only state: one renderer survives all requests, like the worker.
    struct osd_render_state renderer;
    void (*after_render)(struct priv *p);
};
struct vo { struct priv *priv; };
struct mediacodec_geometry { int unused; };

static void mp_mutex_lock(int *lock) { (void)lock; }
static void mp_mutex_unlock(int *lock) { (void)lock; }
static void mp_cond_broadcast(int *cond) { (void)cond; }
static int64_t mp_time_ns(void) { return 0; }

// Observable snapshots for an ended sign, a blank gap, and a later dialogue.
// The render seam is a cue timeline, not an echo of the request timestamp.
static struct sub_bitmap_list blank = {.change_id = 1};
static struct sub_bitmap_list sign = {.change_id = 2};
static struct sub_bitmap_list dialogue = {.change_id = 3};
static struct sub_bitmap_list sign_and_dialogue = {.change_id = 4};
static bool osd_render_pass(struct vo *vo, struct osd_render_state *rs,
                            double pts, struct mediacodec_geometry geometry,
                            int slot)
{
    (void)geometry;
    struct priv *p = vo->priv;
    struct sub_bitmap_list *image = &blank;
    double sign_end = p->long_sign ? 11.0 : 10.04;
    if (pts >= 10.0 && pts < sign_end)
        image = &sign;
    if (pts >= 10.08 && pts < 11.0)
        image = image == &sign ? &sign_and_dialogue : &dialogue;
    bool changed = rs->force_full || image->change_id != rs->posted_change_id;
    if (changed)
        p->osd_slots[slot].sbs = image;
    if (p->after_render) {
        void (*hook)(struct priv *) = p->after_render;
        p->after_render = NULL;
        hook(p);
    }
    return changed;
}

#include "mediacodec_osd_request.inc"

static void check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static struct priv warmed(void)
{
    struct priv p = {
        .osd = {.epoch = 7, .present_slot = 0},
        .osd_slots = {{&blank}, {&sign}},
    };
    osd_prefetch_store(&p.osd.prefetch, 10.0, 1, 7, 1, 0.01);
    return p;
}

static void service_pending(struct priv *p)
{
    // This seam does not simulate worker backpressure: never invoke the
    // production service outside the worker's capacity precondition.
    check(p->osd.requested && p->osd.results.count < OSD_RESULT_CAPACITY,
          "service requires a pending request and reserved queue capacity");
    struct osd_request req = p->osd.request;
    p->osd.requested = false;
    struct vo vo = {.priv = p};
    osd_service_request(&vo, &p->renderer, req,
                        (struct mediacodec_geometry){0});
}

static void request(struct priv *p, double pts, uint64_t seq, bool full)
{
    osd_file_request(p, (struct osd_request){
        .pts = pts, .delta = 1001.0 / 24000.0, .seq = seq,
        .epoch = p->osd.epoch, .full = full,
    });
    service_pending(p);
}

static struct sub_bitmap_list *head_image(struct priv *p)
{
    struct osd_result *r = osd_result_head(&p->osd.results);
    check(r != NULL, "request produces an image, including blank clears");
    return p->osd_slots[r->slot].sbs;
}

// Host flip seam: supply timestamps, retain the bounded history, and invoke
// the production publication helper. No MediaCodec or presenter thread runs.
static struct osd_release publish(struct priv *p, uint64_t seq)
{
    struct osd_release release = {
        .seq = seq, .timestamp = 1000000000 + (int64_t)seq * 40000000,
        .vsync = 1010000000 + (int64_t)seq * 40000000, .period = 16666667,
    };
    p->osd.releases[p->osd.release_next] = release;
    osd_result_publish_release(&p->osd.results, release);
    p->osd.release_next = (p->osd.release_next + 1) % OSD_RELEASE_HISTORY;
    p->osd.release_seq = seq;
    return release;
}

static struct osd_result take(struct priv *p, struct sub_bitmap_list *image,
                               struct osd_release release)
{
    struct osd_result *head = osd_result_head(&p->osd.results);
    check(head && (!head->seq || head->release.seq == head->seq),
          "consume only a pose with its own published release");
    struct osd_result r = osd_result_pop(&p->osd.results);
    p->osd.present_slot = r.slot;
    check(p->osd_slots[r.slot].sbs == image,
          "consume the expected pixel state in FIFO order");
    check(r.seq == release.seq && r.release.seq == release.seq &&
          r.release.timestamp == release.timestamp &&
          r.release.vsync == release.vsync && r.release.period == release.period,
          "each consumed pose retains its exact video release");
    return r;
}

static void test_warmed_images_require_timestamp_validation(void)
{
    struct priv p = warmed();
    request(&p, 10.06, 42, false);
    check(head_image(&p) == &blank,
          "an expired prefetched sign must not replace the blank gap");

    p = warmed();
    request(&p, 10.125, 42, false);
    check(head_image(&p) == &dialogue,
          "a newer cue must not be replaced by the old prefetched sign");

    p = warmed();
    p.long_sign = true;
    request(&p, 10.125, 42, false);
    check(head_image(&p) == &sign_and_dialogue,
          "a static prefetched sign must not hide an overlapping dialogue cue");

    p = warmed();
    request(&p, 9.999, 42, false);
    check(head_image(&p) == &blank, "the sign must not appear before its start");
    check(p.osd.prefetch.valid, "the future image stays available for GPU warming");

    p = warmed();
    p.osd_slots[2].sbs = &dialogue;
    osd_spec_store(&p.osd.spec, 10.125, 7, 2);
    request(&p, 10.125, 42, false);
    check(head_image(&p) == &dialogue,
          "a matching next-frame image wins over an older event image");

    p = warmed();
    p.osd_slots[2].sbs = &sign;
    osd_spec_store(&p.osd.spec, 10.125, 7, 2);
    p.osd_pts = 10.125;
    osd_invalidate_locked(&p);
    service_pending(&p);
    check(head_image(&p) == &dialogue,
          "a seek invalidates both prepared images");

    p = warmed();
    p.osd_slots[2].sbs = &sign;
    osd_spec_store(&p.osd.spec, 10.125, 7, 2);
    request(&p, 10.125, 42, true);
    check(head_image(&p) == &dialogue,
          "a full repaint cannot reuse either prepared image");
}

static void test_three_pixel_states_survive_a_busy_presenter(void)
{
    struct priv p = warmed();
    struct osd_release a_release = publish(&p, 101); // flip before enqueue
    request(&p, 10.02, 101, false);
    struct osd_result a = take(&p, &sign, a_release);

    request(&p, 10.06, 102, false); // B: the sign must clear
    request(&p, 10.125, 103, false); // C: a different cue appears
    check(p.osd_slots[a.slot].sbs == &sign,
          "rendering B and C cannot overwrite the presenter's held A");
    check(head_image(&p) == &blank,
          "C must not replace B while the presenter is holding A");
    check(osd_result_head(&p.osd.results)->release.seq == 0,
          "an unflipped pose must not inherit an unrelated earlier release");
    struct osd_release b_release = publish(&p, 102); // flip after enqueue
    struct osd_release c_release = publish(&p, 103);

    // Both pending poses must retain their releases after flip history wraps.
    for (uint64_t seq = 104; seq <= 103 + OSD_RELEASE_HISTORY; seq++)
        publish(&p, seq);
    check(osd_find_release(&p, 102).seq == 0 && osd_find_release(&p, 103).seq == 0,
          "the scenario has evicted both pending releases from history");
    take(&p, &blank, b_release);
    take(&p, &dialogue, c_release);
    check(!osd_result_head(&p.osd.results), "all three changed states were consumed");

    request(&p, 10.125, 200, false);
    check(!osd_result_head(&p.osd.results),
          "an unchanged cue needs no duplicate presentation after the queue drains");
}

static void test_cancelled_clear_is_not_suppressed_by_render_history(void)
{
    struct priv p = warmed();
    struct osd_release release = publish(&p, 101);
    request(&p, 10.02, 101, false);
    take(&p, &sign, release);
    request(&p, 10.06, 102, false); // accepted blank, not yet visible
    check(head_image(&p) == &blank, "a clear is waiting behind the visible sign");

    // A forced same-PTS repaint must not inherit the pending pinned request.
    struct osd_request old = {
        .pts = 10.06, .seq = 103, .epoch = p.osd.epoch,
    };
    osd_file_request(&p, old);
    osd_file_request(&p, (struct osd_request){
        .pts = old.pts, .epoch = old.epoch,
    });
    check(p.osd.request.seq == old.seq && p.osd.epoch == old.epoch &&
          head_image(&p) == &blank,
          "an ordinary same-PTS redraw preserves the pending frame's timeline");
    p.osd_pts = 10.06;
    osd_invalidate_locked(&p);
    check(!osd_result_head(&p.osd.results), "invalidation discards accepted old poses");
    check(p.osd.request.seq == 0 && p.osd.request.epoch != old.epoch,
          "forced repaint cancels the old timeline even at the pending frame's PTS");

    // The renderer still remembers the accepted blank. The repaint must clear
    // the visible sign anyway, without another stale request forcing it first.
    service_pending(&p);
    take(&p, &blank, (struct osd_release){0});
}

static void test_render_finishing_after_cancellation_can_still_clear(void)
{
    struct priv p = warmed();
    struct osd_release release = publish(&p, 101);
    request(&p, 10.02, 101, false);
    take(&p, &sign, release);
    p.osd_pts = 10.06;
    p.after_render = osd_invalidate_locked;
    request(&p, 10.06, 102, false);
    check(!osd_result_head(&p.osd.results),
          "render completion after an epoch barrier must be rejected");
    service_pending(&p);
    take(&p, &blank, (struct osd_release){0});
}

static void test_expired_release_does_not_authorize_or_suppress_a_clear(void)
{
    struct priv p = warmed();
    struct osd_release release = publish(&p, 101);
    request(&p, 10.02, 101, false);
    take(&p, &sign, release);
    for (uint64_t seq = 102; seq <= 102 + OSD_RELEASE_HISTORY; seq++)
        publish(&p, seq);
    request(&p, 10.06, 102, false);
    check(!osd_result_head(&p.osd.results),
          "a late render without its retained release cannot be presented");
    request(&p, 10.06, 200, false);
    take(&p, &blank, publish(&p, 200));
}

int main(void)
{
    test_warmed_images_require_timestamp_validation();
    test_three_pixel_states_survive_a_busy_presenter();
    test_cancelled_clear_is_not_suppressed_by_render_history();
    test_render_finishing_after_cancellation_can_still_clear();
    test_expired_release_does_not_authorize_or_suppress_a_clear();
    printf("mediacodec osd requests: all content scenarios passed\n");
    return 0;
}
