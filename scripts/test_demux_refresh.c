// Behavioral regression for patch 0208, extracted by test_demux_refresh.sh
// from demux/demux.c: add_packet_locked(), which queues every packet the
// demuxer thread reads, and clear_queue(), which gives a queue its initial
// state. The struct demux_queue and struct demux_stream layouts come from the
// same file; struct demux_packet from demux/packet.h.
//
// What must hold: after a refresh seek, a stream that was already selected
// drops what it reads back up to and including its old position and queues
// everything past it. demux.c refreshes in two places, and both are replayed
// here as demux.c sets them up: the in-cache seek that resumes reading at the
// end of the cached data (a seek whose new range joined an existing one keeps
// the queue and its last_dts/last_pos), and initiate_refresh_seek() for a
// track switch (clears the queue and restores last_dts/last_pos from the last
// packet the decoder got). Before 0208 the packet that ended the search was
// dropped even when it lay past the old position. Video and audio read their
// old last packet back, so nothing new was lost for them; a subtitle stream
// usually has no packet between the seek target and its old position, so it
// lost its next line for good (plezy#2717). A subtitle queue with nothing in
// the joined range has no old position at all; with Matroska's missing DTS
// its first line was dropped as NOPTS <= NOPTS (plezy#2810).
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "demux/packet.h"

#define MP_NOPTS_VALUE (-0x1p+63)
#define MP_PTS_MIN(a, b) ((a) == MP_NOPTS_VALUE ? (b) : (b) == MP_NOPTS_VALUE ? (a) : (a) < (b) ? (a) : (b))
#define mp_assert assert
#define DEMUX_EVENT_DURATION (1 << 1)

static int warnings;
#define MP_WARN(ctx, ...) ((void)(ctx), warnings++)
// Evaluates its arguments, so the locals add_packet_locked() computes only for
// its trace line stay used under -Werror.
static void trace_sink(const char *fmt, ...) { (void)fmt; }
#define MP_TRACE(ctx, ...) ((void)(ctx), trace_sink(__VA_ARGS__))

enum stream_type { STREAM_VIDEO, STREAM_AUDIO, STREAM_SUB };

struct demux_cached_range;
struct demux_packet_pool;
struct demux_cache;
struct index_entry;

struct demux_opts { bool disk_cache; };
struct demuxer {
    double start_time;
    double duration;
    struct demux_opts *opts;
};

struct sh_stream {
    enum stream_type type;
    int index;
    struct demux_packet *attached_picture;
    struct demux_stream *ds;
};

// The fields add_packet_locked() reaches; the real struct is mostly locking,
// cache and thread state the refresh decision does not read.
struct demux_internal {
    struct demuxer *d_thread;
    struct demuxer *d_user;
    struct demux_packet_pool *packet_pool;
    struct demux_cache *cache;
    bool after_seek;
    bool after_seek_to_start;
    bool seeking;
    bool eof;
    double demux_ts;
    double highest_av_pts;
    double duration;
    uint64_t total_bytes;
    int events;
};

#include "demux_refresh_types.inc"

// --- fakes: the demuxer around the extracted code.

static int dropped;

static bool demux_cancel_test(struct demuxer *demuxer) { (void)demuxer; return false; }
static void talloc_free(void *p) { free(p); }

static void demux_packet_pool_push(struct demux_packet_pool *pool, struct demux_packet *dp)
{
    (void)pool;
    dropped++;
    free(dp);
}

static void demux_packet_pool_prepend(struct demux_packet_pool *pool,
                                      struct demux_packet *head, struct demux_packet *tail)
{
    (void)pool; (void)tail;
    while (head) {
        struct demux_packet *next = head->next;
        free(head);
        head = next;
    }
}

size_t demux_packet_estimate_total_size(struct demux_packet *dp) { return dp->len; }
void demux_packet_unref_contents(struct demux_packet *dp) { (void)dp; abort(); }
static int64_t demux_cache_write(struct demux_cache *cache, struct demux_packet *dp)
{
    (void)cache; (void)dp;
    abort();
}

static void free_index(struct demux_queue *queue) { (void)queue; }
static void record_packet(struct demux_internal *in, struct demux_packet *dp) { (void)in; (void)dp; }
static uint64_t get_forward_buffered_bytes(struct demux_stream *ds) { (void)ds; return 0; }
static const char *stream_type_name(enum stream_type type) { (void)type; return "stream"; }
static void adjust_seek_range_on_packet(struct demux_stream *ds, struct demux_packet *dp)
{
    (void)ds; (void)dp;
}
static void prune_old_packets(struct demux_internal *in) { (void)in; }
static void back_demux_see_packets(struct demux_stream *ds) { (void)ds; }
static void wakeup_ds(struct demux_stream *ds) { (void)ds; }

#include "demux_refresh.inc"

// --- harness

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

struct rig {
    struct demux_opts opts;
    struct demuxer demuxer;
    struct demux_internal in;
    struct sh_stream sh;
    struct demux_stream ds;
    struct demux_queue queue;
};

static void rig_init(struct rig *r, enum stream_type type)
{
    memset(r, 0, sizeof(*r));
    r->demuxer.opts = &r->opts;
    r->in.d_thread = r->in.d_user = &r->demuxer;
    r->in.demux_ts = r->in.highest_av_pts = MP_NOPTS_VALUE;
    r->sh.type = type;
    r->sh.ds = &r->ds;
    r->ds.in = &r->in;
    r->ds.sh = &r->sh;
    r->ds.type = type;
    r->ds.queue = &r->queue;
    r->ds.selected = true;
    r->ds.global_correct_dts = r->ds.global_correct_pos = true;
    r->ds.base_ts = MP_NOPTS_VALUE;
    r->queue.ds = &r->ds;
    clear_queue(&r->queue);
    dropped = 0;
    warnings = 0;
}

static void rig_free(struct rig *r)
{
    clear_queue(&r->queue);
}

// A packet as the demuxer thread hands it over. A NOPTS dts leaves the queue
// with correct_pos only, as for a demuxer that does not set DTS.
static void feed(struct rig *r, double dts, int64_t pos)
{
    struct demux_packet *dp = calloc(1, sizeof(*dp));
    assert(dp);
    dp->pts = dp->dts = dts;
    dp->pos = pos;
    dp->len = 1;
    dp->stream = r->sh.index;
    dp->keyframe = true;
    add_packet_locked(&r->sh, dp);
}

// What the seek in demux.c that resumes at the end of the cached data does
// once the joined range is current: the queue keeps its packets and its
// last_dts/last_pos, and every selected stream refreshes.
static void refresh_after_join(struct rig *r)
{
    r->ds.refreshing = r->ds.selected;
}

// What initiate_refresh_seek() does to a stream that was already selected
// when another track is switched on: the queue is cleared and the refresh
// searches for the last packet the decoder got.
static void refresh_after_track_switch(struct rig *r, double last_ret_dts, int64_t last_ret_pos)
{
    bool correct_dts = r->queue.correct_dts;
    bool correct_pos = r->queue.correct_pos;
    clear_queue(&r->queue);
    r->ds.refreshing = true;
    r->queue.correct_dts = correct_dts;
    r->queue.correct_pos = correct_pos;
    r->queue.last_pos = last_ret_pos;
    r->queue.last_dts = last_ret_dts;
}

// The queue as a list of positions, which every packet here sets uniquely.
static bool queue_is(struct rig *r, const int64_t *want, int n)
{
    int i = 0;
    for (struct demux_packet *dp = r->queue.head; dp; dp = dp->next, i++) {
        if (i >= n || dp->pos != want[i])
            return false;
    }
    return i == n;
}

int main(void)
{
    struct rig r;

    // A seek joined a new range to the cached one and reading resumes 1 s
    // before the cached end (55.958 here). The subtitle stream's last queued
    // line is at 50 s, its next at 60 s: the first line it reads back is new.
    rig_init(&r, STREAM_SUB);
    for (int s = 10; s <= 50; s += 10)
        feed(&r, s, s * 10);
    refresh_after_join(&r);
    feed(&r, 60, 600);
    feed(&r, 70, 700);
    CHECK(queue_is(&r, (const int64_t[]){100, 200, 300, 400, 500, 600, 700}, 7),
          "join, subtitles: the line at 60 s, the first past the old position, was not "
          "queued (%d dropped); it never shows and never reaches the cache (plezy#2717)",
          dropped);
    CHECK(!r.ds.refreshing, "join, subtitles: the stream is still refreshing after a new packet");
    rig_free(&r);

    // The same join for video, which reads its old tail back: exactly the
    // packets it already had are dropped, so none is queued twice, and the
    // stream resumes with the packet after its old last one.
    rig_init(&r, STREAM_VIDEO);
    for (int i = 0; i <= 12; i++)
        feed(&r, 50.0 + i * 0.5, 5000 + i * 50);
    refresh_after_join(&r);
    for (int i = 10; i <= 14; i++)
        feed(&r, 50.0 + i * 0.5, 5000 + i * 50);
    {
        int64_t want[15];
        for (int i = 0; i <= 14; i++)
            want[i] = 5000 + i * 50;
        CHECK(queue_is(&r, want, 15) && dropped == 3,
              "join, video: re-read packets were not dropped exactly up to the old last "
              "one (%d dropped, expected 3)", dropped);
    }
    CHECK(!r.ds.refreshing, "join, video: the stream is still refreshing past its old position");
    rig_free(&r);

    // Audio switched on mid-playback: the subtitle stream is refreshed from
    // 1 s before the video position, and the decoder's last line was at 50 s.
    rig_init(&r, STREAM_SUB);
    for (int s = 10; s <= 50; s += 10)
        feed(&r, s, s * 10);
    refresh_after_track_switch(&r, 50, 500);
    feed(&r, 60, 600);
    feed(&r, 70, 700);
    CHECK(queue_is(&r, (const int64_t[]){600, 700}, 2),
          "track switch, subtitles: the line at 60 s was not queued after the refresh "
          "(%d dropped)", dropped);
    rig_free(&r);

    // A stream without DTS refreshes by file position: the same rules apply.
    rig_init(&r, STREAM_SUB);
    for (int s = 10; s <= 50; s += 10)
        feed(&r, MP_NOPTS_VALUE, s * 10);
    CHECK(!r.queue.correct_dts && r.queue.correct_pos,
          "pos only: the queue did not fall back to file positions");
    refresh_after_join(&r);
    feed(&r, MP_NOPTS_VALUE, 600);
    CHECK(queue_is(&r, (const int64_t[]){100, 200, 300, 400, 500, 600}, 6),
          "pos only, subtitles: the packet at pos 600, the first past the old position, "
          "was not queued (%d dropped)", dropped);
    rig_free(&r);

    rig_init(&r, STREAM_SUB);
    for (int s = 10; s <= 50; s += 10)
        feed(&r, MP_NOPTS_VALUE, s * 10);
    refresh_after_join(&r);
    feed(&r, MP_NOPTS_VALUE, 400);
    feed(&r, MP_NOPTS_VALUE, 500);
    feed(&r, MP_NOPTS_VALUE, 600);
    CHECK(queue_is(&r, (const int64_t[]){100, 200, 300, 400, 500, 600}, 6) && dropped == 2,
          "pos only, re-read tail: packets at or before the old position were not dropped "
          "exactly (%d dropped, expected 2)", dropped);
    rig_free(&r);

    // A seek joined ranges while the subtitle stream had nothing queued in
    // either, as for a signs track with no line in the opening minute. The
    // queue keeps the empty queue's correct_dts and has no old DTS or
    // position; Matroska packets carry no DTS. Every packet is new.
    rig_init(&r, STREAM_SUB);
    refresh_after_join(&r);
    feed(&r, MP_NOPTS_VALUE, 3550);
    feed(&r, MP_NOPTS_VALUE, 3600);
    CHECK(queue_is(&r, (const int64_t[]){3550, 3600}, 2) && dropped == 0,
          "join, empty subtitle queue: the first line read after the join was not queued "
          "(%d dropped); it never shows and never reaches the cache (plezy#2810)", dropped);
    CHECK(!r.ds.refreshing, "join, empty subtitle queue: the stream is still refreshing");
    rig_free(&r);

    CHECK(warnings == 0, "a refresh with valid DTS or pos warned %d time(s)", warnings);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("demux refresh harness: ok\n");
    return 0;
}
