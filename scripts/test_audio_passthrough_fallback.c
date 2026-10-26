// Behavioral regression for patch 0040, extracted by
// test_audio_passthrough_fallback.sh from player/audio.c: handle_ao_events(),
// fall_back_to_pcm() and reload_audio_output().
//
// What must hold: an output that cannot play its passthrough stream reports
// AO_EVENT_PASSTHROUGH_FAILED. The player closes it, decodes the track it fed
// to PCM, and decodes again from the playback position, because the failed
// output took audio it never played. The fallback belongs to that track: a
// later plain reload tries passthrough again, and a failure reported by an
// output that outlived its chain (gapless file change, track switch) only
// closes it. Before 0040 the outputs latched the format instead, and
// ao_avfoundation's latch outlived every track and player in the process.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MP_ERR(...) ((void)0)
#define MP_VERBOSE(...) ((void)0)

struct mp_log;
struct mp_async_queue;
struct mp_pin;
struct mp_decoder_wrapper { bool spdif; int reinits; bool fail_reinit; };
struct track { struct mp_decoder_wrapper *dec; };
struct mp_filter { int resets, wakeups; };
struct mp_output_chain { int resets; };
struct ao { int events; int resets; bool closed; };

#include "passthrough_fallback_types.inc"

struct MPContext {
    struct ao *ao;
    struct ao_chain *ao_chain;
    enum playback_status audio_status;
};

// --- fakes: audio/out/ao.c, player/audio.c and player/misc.c, as far as the
// extracted code reaches.

static int ao_query_and_reset_events(struct ao *ao, int events)
{
    int set = ao->events & events;
    ao->events &= ~events;
    return set;
}

static void ao_reset(struct ao *ao) { ao->resets++; }

// player/audio.c: forgets the chain's AO and closes the player's.
static void uninit_audio_out(struct MPContext *mpctx)
{
    if (mpctx->ao_chain) {
        mpctx->ao_chain->ao_queue = NULL;
        mpctx->ao_chain->queue_filter = NULL;
        mpctx->ao_chain->ao = NULL;
    }
    if (mpctx->ao)
        mpctx->ao->closed = true;
    mpctx->ao = NULL;
}

static int reinit_audio_filters(struct MPContext *mpctx) { (void)mpctx; return 1; }
static void reset_audio_state(struct MPContext *mpctx) { mpctx->audio_status = STATUS_SYNCING; }
static void mp_output_chain_reset_harder(struct mp_output_chain *c) { c->resets++; }
static void mp_filter_reset(struct mp_filter *f) { f->resets++; }
static void mp_filter_wakeup(struct mp_filter *f) { f->wakeups++; }
static void mp_wakeup_core(struct MPContext *mpctx) { (void)mpctx; }
static void mp_decoder_wrapper_set_spdif_flag(struct mp_decoder_wrapper *d, bool f) { d->spdif = f; }
static bool mp_decoder_wrapper_reinit(struct mp_decoder_wrapper *d)
{
    d->reinits++;
    return !d->fail_reinit;
}

static int track_errors;
static struct track *errored_track;
// player/misc.c: deselecting the track frees its chain.
static void error_on_track(struct MPContext *mpctx, struct track *t)
{
    track_errors++;
    errored_track = t;
    mpctx->ao_chain = NULL;
}

static int refresh_seeks;
static enum seek_precision refresh_precision;
static void issue_refresh_seek(struct MPContext *mpctx, enum seek_precision min_prec)
{
    (void)mpctx;
    refresh_seeks++;
    refresh_precision = min_prec;
}

void reload_audio_output(struct MPContext *mpctx);

#include "passthrough_fallback.inc"

// --- harness

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

struct rig {
    struct MPContext mpctx;
    struct ao ao;
    struct ao_chain ao_c;
    struct track track;
    struct mp_decoder_wrapper dec;
    struct mp_filter ao_filter;
    struct mp_output_chain chain;
};

// A passthrough track playing through r->ao.
static void rig_init(struct rig *r)
{
    memset(r, 0, sizeof(*r));
    track_errors = refresh_seeks = 0;
    errored_track = NULL;
    refresh_precision = MPSEEK_DEFAULT;
    r->dec.spdif = true;
    r->track.dec = &r->dec;
    r->ao_c.mpctx = &r->mpctx;
    r->ao_c.spdif_passthrough = true;
    r->ao_c.filter = &r->chain;
    r->ao_c.ao_filter = &r->ao_filter;
    r->ao_c.track = &r->track;
    r->ao_c.ao = &r->ao;
    r->mpctx.ao = &r->ao;
    r->mpctx.ao_chain = &r->ao_c;
    r->mpctx.audio_status = STATUS_PLAYING;
}

static bool decodes_to_pcm(struct rig *r)
{
    return !r->dec.spdif && !r->ao_c.spdif_passthrough && r->ao_c.spdif_failed;
}

static bool still_passthrough(struct rig *r)
{
    return r->dec.spdif && r->ao_c.spdif_passthrough && !r->ao_c.spdif_failed;
}

int main(void)
{
    struct rig r;

    // The output fails under the track it plays: AVPlayer never started, or
    // the spatial renderer refused the stream.
    rig_init(&r);
    r.ao.events = AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    CHECK(r.ao.closed && !r.mpctx.ao, "failure: the failed output stays open");
    CHECK(decodes_to_pcm(&r) && r.dec.reinits == 1,
          "failure: the track is not decoded to PCM (spdif %d, reinits %d)",
          r.dec.spdif, r.dec.reinits);
    CHECK(r.ao_filter.wakeups > 0 && r.chain.resets > 0,
          "failure: the chain is not reset and woken for the next output");
    CHECK(refresh_seeks == 1 && refresh_precision == MPSEEK_EXACT,
          "failure: %d refresh seek(s) at precision %d; the audio the failed output "
          "took is skipped (silent with video)", refresh_seeks, refresh_precision);
    CHECK(track_errors == 0, "failure: a successful fallback errored the track");

    // A later plain reload of the PCM output (a route or device change) tries
    // passthrough again: the fallback belongs to this track, not the player.
    struct ao pcm = {.events = AO_EVENT_RELOAD};
    r.mpctx.ao = &pcm;
    r.ao_c.ao = &pcm;
    handle_ao_events(&r.mpctx);
    CHECK(pcm.closed && still_passthrough(&r) && r.dec.reinits == 2,
          "later reload: passthrough is not tried again (spdif %d, failed %d)",
          r.dec.spdif, r.ao_c.spdif_failed);
    CHECK(refresh_seeks == 1, "later reload: a plain reload issued a refresh seek");

    // Both events pending at once: one teardown, and the failure wins.
    rig_init(&r);
    r.ao.events = AO_EVENT_RELOAD | AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    CHECK(r.ao.resets == 1 && r.ao.closed, "both: the output was reset %d times", r.ao.resets);
    CHECK(decodes_to_pcm(&r) && r.dec.reinits == 1 && refresh_seeks == 1,
          "both: the plain reload hid the failure (spdif %d, reinits %d, seeks %d)",
          r.dec.spdif, r.dec.reinits, refresh_seeks);

    // A plain reload of a working passthrough output leaves the decoder alone.
    rig_init(&r);
    r.ao.events = AO_EVENT_RELOAD;
    handle_ao_events(&r.mpctx);
    CHECK(r.ao.closed && still_passthrough(&r) && r.dec.reinits == 0 && refresh_seeks == 0,
          "plain reload: the passthrough track was touched (spdif %d, reinits %d, seeks %d)",
          r.dec.spdif, r.dec.reinits, refresh_seeks);

    // No event: the output stays, as on every playloop iteration.
    rig_init(&r);
    handle_ao_events(&r.mpctx);
    CHECK(!r.ao.closed && r.ao.resets == 0 && r.mpctx.ao == &r.ao,
          "no event: the output was closed");

    // The output outlived the chain it failed for: a track switch or gapless
    // file change kept it open, and the new chain is not attached to it yet.
    // The new track tries passthrough with a fresh output.
    rig_init(&r);
    r.ao_c.ao = NULL;
    r.ao.events = AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    CHECK(r.ao.closed && !r.mpctx.ao, "outlived chain: the failed output stays open");
    CHECK(still_passthrough(&r) && r.dec.reinits == 0 && refresh_seeks == 0,
          "outlived chain: the next track lost passthrough for the old one's failure");

    // No chain at all (the last file ended; the output was kept for the next).
    rig_init(&r);
    r.mpctx.ao_chain = NULL;
    r.ao.events = AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    CHECK(r.ao.closed && !r.mpctx.ao && r.dec.reinits == 0 && refresh_seeks == 0,
          "no chain: the failed output is not just closed");

    // A lavfi-complex chain has no track and no decoder to switch.
    rig_init(&r);
    r.ao_c.track = NULL;
    r.ao.events = AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    CHECK(r.ao.closed && r.dec.reinits == 0 && refresh_seeks == 0 && track_errors == 0,
          "no track: a chain without a decoder was asked to fall back");

    // The decoder cannot be reinitialized: the track errors out, which frees
    // its chain, and nothing runs on that chain afterwards.
    rig_init(&r);
    r.dec.fail_reinit = true;
    r.ao.events = AO_EVENT_PASSTHROUGH_FAILED;
    handle_ao_events(&r.mpctx);
    CHECK(track_errors == 1 && errored_track == &r.track && !r.mpctx.ao_chain,
          "reinit failure: the track was not errored (%d)", track_errors);
    CHECK(refresh_seeks == 0, "reinit failure: a refresh seek ran for a freed chain");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("audio passthrough fallback harness: ok\n");
    return 0;
}
