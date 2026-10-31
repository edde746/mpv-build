// Exercise the production first-frame and A/V scheduling functions, not a
// second implementation of their arithmetic. The AO retains seconds of audio
// while vid=no tears down video; restoring it must not wait out that backlog.
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define MP_NOPTS_VALUE (-0x1p+63)
#define MP_STATS(...) ((void)0)
#define MP_WARN(...) ((void)0)
#define MP_TRACE(...) ((void)0)
#define mp_assert(x) do { if (!(x)) abort(); } while (0)
#define VS_NONE 1
#define VO_CAP_UNTIMED 1
#include "restart_types.inc"

struct mp_image { double pts; };
struct vo_chain { bool is_sparse; };
struct ao_chain { bool delaying_audio_start; };
struct demuxer { bool ts_resets_possible; };
struct ao { double buffered; bool untimed; };
struct mp_vo_opts { int video_sync; };
struct vo_driver { int caps; };
struct vo { struct mp_vo_opts *opts; struct vo_driver *driver; };
struct MPOpts {
    double audio_delay, default_max_pts_correction;
    int autosync;
    bool untimed;
};
struct MPContext {
    struct MPOpts *opts;
    struct vo *video_out;
    struct vo_chain *vo_chain;
    struct ao_chain *ao_chain;
    struct ao *ao;
    struct demuxer *demuxer;
    enum playback_status audio_status, video_status;
    int num_next_frames;
    struct mp_image *next_frames[1];
    double video_pts, video_speed, delay, time_frame, total_avsync_change;
    double written;
    bool display_sync_active;
};
static double written_audio_pts(struct MPContext *c) { return c->written; }
static double ao_get_delay(struct ao *ao) { return ao->buffered; }
static bool ao_untimed(struct ao *ao) { return ao->untimed; }
#include "restart.inc"

static int checks;
static void close_to(double got, double want, const char *what)
{
    checks++;
    if (fabs(got - want) > 1e-8) {
        fprintf(stderr, "FAIL: %s: got %.9f, wanted %.9f\n", what, got, want);
        exit(1);
    }
}

static void rejoin(double speed, double audio_delay, double first, double written,
                   double buffered, double expected_wait)
{
    struct MPOpts opts = { .audio_delay = audio_delay,
                          .default_max_pts_correction = -1 };
    struct mp_vo_opts vo_opts = {0};
    struct vo_driver driver = {0};
    struct vo vo = { &vo_opts, &driver };
    struct vo_chain vc = {0};
    struct ao_chain ac = {0};
    struct demuxer demuxer = {0};
    struct ao ao = { .buffered = buffered };
    struct mp_image image = { .pts = first };
    struct MPContext c = {
        .opts = &opts, .video_out = &vo, .vo_chain = &vc, .ao_chain = &ac,
        .ao = &ao, .demuxer = &demuxer,
        .audio_status = STATUS_PLAYING, .video_status = STATUS_SYNCING,
        .num_next_frames = 1, .next_frames = { &image },
        .video_pts = MP_NOPTS_VALUE, .video_speed = speed,
        // Audio written since the reset cannot account for the older AO queue.
        .delay = 0.512, .written = written,
    };
    handle_new_frame(&c);
    close_to(c.delay, written + audio_delay - first, "first frame rejoins written audio");
    update_avsync_before_frame(&c);
    close_to(c.time_frame, 0, "first syncing frame is still immediate");

    c.video_pts = first;
    c.video_status = STATUS_PLAYING;
    image.pts += 0.041;
    handle_new_frame(&c);
    update_avsync_before_frame(&c);
    close_to(c.time_frame, expected_wait + 0.041 / speed,
             "next frame follows audio phase, not queued backlog");
}

static void missing_and_startup_timestamps(void)
{
    struct MPOpts opts = {0};
    struct mp_image image = { .pts = 10 };
    struct MPContext c = {
        .opts = &opts, .audio_status = STATUS_SYNCING,
        .video_status = STATUS_SYNCING, .video_pts = MP_NOPTS_VALUE,
        .video_speed = 1, .num_next_frames = 1, .next_frames = { &image },
        .written = 13.5, .delay = 0.125,
    };
    // A full seek/load has not started audio yet: its normal startup owns sync.
    handle_new_frame(&c);
    close_to(c.delay, 0.125, "ordinary seek retains audio startup synchronization");

    c.audio_status = STATUS_PLAYING;
    c.written = MP_NOPTS_VALUE;
    handle_new_frame(&c);
    close_to(c.delay, 0.125, "unknown audio timestamp leaves delay alone");
    c.written = 13.5;
    image.pts = MP_NOPTS_VALUE;
    handle_new_frame(&c);
    close_to(c.delay, 0.125, "unknown video timestamp leaves delay alone");
}

int main(void)
{
    rejoin(1, 0, 10, 13.5, 3.5, 0);
    rejoin(2, 0, 10, 17, 3.5, 0);
    rejoin(0.5, 0, 10, 11.75, 3.5, 0);
    rejoin(1.5, 0.25, 10.25, 15.25, 3.5, 0);
    rejoin(1, -0.25, 9.75, 13.5, 3.5, 0);
    rejoin(1, 0, 10, 13.5, 4, 0.5);
    rejoin(1, 0, -2, 1.5, 3.5, 0);
    missing_and_startup_timestamps();
    printf("PASS: %d video restart clock checks\n", checks);
    return 0;
}
