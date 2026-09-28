// Behavioral regressions for the core's frame-queue gate and frame durations
// under a VO that times each frame from its own pts (patch 0131), extracted by
// test_player_queue_frames.sh from player/video.c. The decoder is a scripted
// output pin and the VO a caps word.
//
// What must hold: vo_mediacodec gets a decoded frame without waiting for the
// next one to be decoded (on an Amlogic box that wait made about one frame in
// eighteen late, plezy#2485), while the decoder is still asked for that next
// frame as early as before; and a frame queued alone takes the stream's
// measured spacing as its duration, not the container rate, which is half the
// frame rate of a decoder that splits fields into frames. Everything else the
// core does -- other VOs, the first frame after a seek, display-sync, EOF --
// is unchanged.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MP_NOPTS_VALUE (-0x1p+63)
#define VO_MAX_REQ_FRAMES 10
#define MP_ARRAY_SIZE(s) (sizeof(s) / sizeof((s)[0]))
#define MPMAX(a, b) ((a) > (b) ? (a) : (b))
#define MPCLAMP(a, min, max) (((a) < (min)) ? (min) : (((a) > (max)) ? (max) : (a)))
#define mp_assert(x) do { if (!(x)) { fprintf(stderr, "FAIL: assert %s\n", #x); exit(1); } } while (0)
#define MP_STATS(...) ((void)0)
#define MP_ERR(...) ((void)0)
#define MP_WARN(...) ((void)0)

#include "queue_frames_types.inc"

// --- fakes: the fields and calls the extracted functions reach.

struct mp_image { double pts; };

enum mp_frame_type { MP_FRAME_NONE = 0, MP_FRAME_VIDEO, MP_FRAME_EOF };
struct mp_frame { enum mp_frame_type type; void *data; };

struct mp_pin { int unused; };
struct mp_filter { struct mp_pin *pins[2]; };
struct mp_output_chain { struct mp_filter *f; bool got_output_eof; double container_fps; };
struct vo_chain { struct mp_output_chain *filter; bool is_sparse, is_coverart; };

struct vo_driver { int caps; };
struct mp_vo_opts { int video_sync; };
struct vo { const struct vo_driver *driver; struct mp_vo_opts *opts; int req_frames; };

struct MPOpts { bool untimed, video_latency_hacks; };

struct MPContext {
    struct vo *video_out;
    struct MPOpts *opts;
    struct vo_chain *vo_chain;
    double video_pts;
    struct mp_image *next_frames[VO_MAX_REQ_FRAMES + 1];
    int num_next_frames;
    struct frame_info past_frames[16];
    int num_past_frames;
    bool display_sync_active;
    enum playback_status video_status;
    bool hrseek_active, hrseek_backstep, hrseek_lastframe;
    double hrseek_pts, playback_pts;
    struct mp_image *saved_frame;
    int max_frames, play_dir;
};

// The decoder: frames it has finished, handed out in order, and how often
// the core asked for one.
static struct {
    struct mp_image frames[8];
    int ready, taken, reads;
} decoder;

static struct mp_frame mp_pin_out_read(struct mp_pin *p)
{
    decoder.reads++;
    if (decoder.taken == decoder.ready)
        return (struct mp_frame){0};
    return (struct mp_frame){ MP_FRAME_VIDEO, &decoder.frames[decoder.taken++] };
}

static void mp_pin_out_unread(struct mp_pin *p, struct mp_frame frame) { decoder.taken--; }
static int vo_get_num_req_frames(struct vo *vo) { return vo->req_frames; }
static bool vo_has_frame(struct vo *vo) { return false; }
static double get_play_end_pts(struct MPContext *mpctx) { return MP_NOPTS_VALUE; }
static void handle_new_frame(struct MPContext *mpctx) {}
static void mp_image_setrefp(struct mp_image **dst, struct mp_image *src) { *dst = src; }
static void mp_image_unrefp(struct mp_image **img) { *img = NULL; }
static void talloc_free(void *p) {}
static void mp_frame_unref(struct mp_frame *frame) {}

#include "queue_frames.inc"

// --- harness

static void check(bool ok, const char *scenario)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", scenario);
        exit(1);
    }
}

static bool near(double a, double b) { return fabs(a - b) < 1e-9; }

static struct MPContext mpctx;
static struct MPOpts opts;
static struct vo vo;
static struct vo_driver driver;
static struct mp_vo_opts vo_opts;
static struct vo_chain vo_chain;
static struct mp_output_chain chain;
static struct mp_filter filter;
static struct mp_pin pins[2];

// A playing stream on vo_mediacodec (its req_frames is 1), one frame shown
// since the last reset, nothing decoded ahead; `caps` picks the VO.
static void reset(int caps)
{
    memset(&mpctx, 0, sizeof(mpctx));
    memset(&decoder, 0, sizeof(decoder));
    driver = (struct vo_driver){ .caps = caps };
    vo_opts = (struct mp_vo_opts){ .video_sync = VS_DEFAULT };
    vo = (struct vo){ .driver = &driver, .opts = &vo_opts, .req_frames = 1 };
    filter.pins[0] = &pins[0];
    filter.pins[1] = &pins[1];
    chain = (struct mp_output_chain){ .f = &filter, .container_fps = 25 };
    vo_chain = (struct vo_chain){ .filter = &chain };
    mpctx.video_out = &vo;
    mpctx.opts = &opts;
    mpctx.vo_chain = &vo_chain;
    mpctx.video_status = STATUS_PLAYING;
    mpctx.play_dir = 1;
    mpctx.max_frames = -1;
    mpctx.hrseek_pts = MP_NOPTS_VALUE;
    mpctx.playback_pts = MP_NOPTS_VALUE;
    mpctx.video_pts = 1.0;
    mpctx.num_past_frames = 1;
    mpctx.past_frames[0] = (struct frame_info){ .pts = 1.0, .duration = 0.04,
                                                .approx_duration = 0.04 };
}

static void decoded(double pts)
{
    decoder.frames[decoder.ready++] = (struct mp_image){ .pts = pts };
}

static void test_own_timing_queues_a_frame_while_decoding_one_ahead(void)
{
    reset(VO_CAP_FRAMEDROP | VO_CAP_OWN_TIMING);
    decoded(1.04);
    bool eof = false;
    check(video_output_image(&mpctx, &eof) == VD_NEW_FRAME && mpctx.num_next_frames == 1,
          "a decoded frame goes to a self-timed VO without the next one");

    // The core comes back while the VO slot is still taken: the frame stays
    // queueable and the decoder is asked for the next one on each pass.
    int reads = decoder.reads;
    check(video_output_image(&mpctx, &eof) == VD_NEW_FRAME && decoder.reads == reads + 1,
          "the decoder is still asked for the next frame while one is queueable");

    decoded(1.08);
    check(video_output_image(&mpctx, &eof) == VD_NEW_FRAME && mpctx.num_next_frames == 2,
          "the next frame is taken as soon as the decoder has it");
    reads = decoder.reads;
    check(video_output_image(&mpctx, &eof) == VD_NEW_FRAME && decoder.reads == reads,
          "with two frames in hand the core stops asking, as before");
}

static void test_other_vos_still_wait_for_the_next_frame(void)
{
    reset(VO_CAP_FRAMEDROP);
    decoded(1.04);
    bool eof = false;
    check(video_output_image(&mpctx, &eof) == VD_PROGRESS && mpctx.num_next_frames == 1,
          "a VO that does not time itself still gets a frame with its successor");
    decoded(1.08);
    check(video_output_image(&mpctx, &eof) == VD_NEW_FRAME && mpctx.num_next_frames == 2,
          "and gets it once the successor is decoded");
}

static void test_lookahead_kept_where_nothing_is_measured_or_display_sync_runs(void)
{
    bool eof = false;

    reset(VO_CAP_FRAMEDROP | VO_CAP_OWN_TIMING);
    mpctx.video_pts = MP_NOPTS_VALUE;
    mpctx.num_past_frames = 0;
    decoded(0.0);
    check(video_output_image(&mpctx, &eof) == VD_PROGRESS,
          "the first frame after a reset waits for the second, as before");

    reset(VO_CAP_FRAMEDROP | VO_CAP_OWN_TIMING);
    mpctx.display_sync_active = true;
    decoded(1.04);
    check(video_output_image(&mpctx, &eof) == VD_PROGRESS,
          "active display-sync keeps the lookahead");

    reset(VO_CAP_FRAMEDROP | VO_CAP_OWN_TIMING);
    vo_opts.video_sync = VS_DISP_RESAMPLE;
    decoded(1.04);
    check(video_output_image(&mpctx, &eof) == VD_PROGRESS,
          "a display-sync mode keeps the lookahead before it activates");
}

static void test_eof_drains_single_frames_for_every_vo(void)
{
    reset(VO_CAP_FRAMEDROP);
    decoded(1.04);
    chain.got_output_eof = true;
    bool eof = false;
    check(video_output_image(&mpctx, &eof) == VD_PROGRESS,
          "the last frame waits for the decoder to report its end");
    check(video_output_image(&mpctx, &eof) == VD_NEW_FRAME && eof,
          "at EOF the last frame goes out alone, as before");
}

// The core's write_video: record the frame about to be queued, then derive
// its duration.
static void queue_next(void)
{
    memmove(&mpctx.past_frames[1], &mpctx.past_frames[0],
            sizeof(mpctx.past_frames[0]) * (size_t)mpctx.num_past_frames);
    mpctx.num_past_frames++;
    mpctx.past_frames[0] = (struct frame_info){ .pts = mpctx.next_frames[0]->pts,
                                                .num_vsyncs = -1 };
    calculate_frame_duration(&mpctx);
}

// A decoder splitting 1080i50 fields into 50 frames a second, from a 25 fps
// container: ten frames measured 20 ms apart. With `guessed`, the last one
// was queued alone on the container's 40 ms, the guess its successor settles.
static void field_rate_history(int caps, bool guessed)
{
    reset(caps);
    mpctx.num_past_frames = 10;
    for (int n = 0; n < 10; n++) {
        mpctx.past_frames[n] = (struct frame_info){ .pts = 1.0 - 0.02 * n, .duration = 0.02,
                                                    .approx_duration = 0.02 };
    }
    if (guessed) {
        mpctx.past_frames[0].duration = 0.04;
        mpctx.past_frames[0].approx_duration = 0.04;
    }
}

static void test_a_frame_queued_alone_takes_the_measured_spacing(void)
{
    struct mp_image next = { .pts = 1.02 };

    field_rate_history(VO_CAP_FRAMEDROP | VO_CAP_OWN_TIMING, true);
    mpctx.next_frames[0] = &next;
    mpctx.num_next_frames = 1;
    queue_next();
    check(near(mpctx.past_frames[0].duration, 0.02) &&
          near(mpctx.past_frames[0].approx_duration, 0.02),
          "a frame queued alone lasts the measured 20 ms, not the container's 40 ms");
    check(near(mpctx.past_frames[1].duration, 0.02),
          "the previous frame's guessed duration is settled by this frame's pts");

    // MediaTek repeats the timestamp across its first field pair.
    field_rate_history(VO_CAP_FRAMEDROP | VO_CAP_OWN_TIMING, false);
    next.pts = 1.0;
    mpctx.next_frames[0] = &next;
    mpctx.num_next_frames = 1;
    queue_next();
    check(near(mpctx.past_frames[0].duration, 0.02),
          "a repeated timestamp keeps the previous frame's estimate");

    // A broadcast recording's timestamps jump 5 s: the gap is the previous
    // frame's to hold, not a 5 s frame after it.
    field_rate_history(VO_CAP_FRAMEDROP | VO_CAP_OWN_TIMING, false);
    next.pts = 6.0;
    mpctx.next_frames[0] = &next;
    mpctx.num_next_frames = 1;
    queue_next();
    check(near(mpctx.past_frames[0].duration, 0.02) &&
          near(mpctx.past_frames[1].duration, 5.0),
          "a timestamp jump stays with the previous frame, not the one after it");

    // A stream dropping to half its rate is a new cadence, not a jump.
    field_rate_history(VO_CAP_FRAMEDROP | VO_CAP_OWN_TIMING, false);
    next.pts = 1.04;
    mpctx.next_frames[0] = &next;
    mpctx.num_next_frames = 1;
    queue_next();
    check(near(mpctx.past_frames[0].duration, 0.04),
          "a halved frame rate is taken as the new spacing");
}

static void test_durations_are_unchanged_elsewhere(void)
{
    struct mp_image a = { .pts = 1.02 }, b = { .pts = 1.04 };

    field_rate_history(VO_CAP_FRAMEDROP | VO_CAP_OWN_TIMING, false);
    mpctx.next_frames[0] = &a;
    mpctx.next_frames[1] = &b;
    mpctx.num_next_frames = 2;
    queue_next();
    check(near(mpctx.past_frames[0].duration, 0.02),
          "with the next frame in hand the duration is still read from it");

    field_rate_history(VO_CAP_FRAMEDROP, true);
    mpctx.next_frames[0] = &a;
    mpctx.num_next_frames = 1;
    queue_next();
    check(near(mpctx.past_frames[0].duration, 0.04) &&
          near(mpctx.past_frames[1].duration, 0.04),
          "another VO's last frame keeps the container duration and history");
}

int main(void)
{
    test_own_timing_queues_a_frame_while_decoding_one_ahead();
    test_other_vos_still_wait_for_the_next_frame();
    test_lookahead_kept_where_nothing_is_measured_or_display_sync_runs();
    test_eof_drains_single_frames_for_every_vo();
    test_a_frame_queued_alone_takes_the_measured_spacing();
    test_durations_are_unchanged_elsewhere();
    puts("PASS: a self-timed VO gets each frame as soon as it is decoded, the "
         "decoder still runs a frame ahead, and a lone frame lasts the measured "
         "spacing");
    return 0;
}
