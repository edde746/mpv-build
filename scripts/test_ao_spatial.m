// Unit test for the "spatial" audio output
// (patches/mpv/pool/0039-ao-spatial-render-truehd-atmos-and-dts-x.patch).
// It drives the driver the way mpv's push thread does, against a real
// synchronizer and a fake renderer. Run scripts/test_ao_spatial.sh, which
// builds it against the patched mpv source.

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/out/ao_spatial.m"

#import <QuartzCore/QuartzCore.h>

// The parts of mpv the driver calls.
static atomic_int wakeups;
static int reloads;
void mp_msg(struct mp_log *log, int lev, const char *format, ...) {}
void ao_wakeup(struct ao *ao) { atomic_fetch_add(&wakeups, 1); }
void ao_request_reload(struct ao *ao) { reloads++; }
bool af_fmt_is_spdif(int format) { return format >= AF_FORMAT_S_AAC && format < AF_FORMAT_COUNT; }
const char *af_fmt_to_str(int format) { return "spdif"; }

struct fake_sink {
    struct mpv_global player;
    bool refuse, ready, failed, defer_ready;
    int codec;
    AVSampleBufferRenderSynchronizer *synchronizer;
    int begins, closes, ends, volumes;
    float volume;
    int64_t begin_time;
    NSMutableData *received;
};

static struct fake_sink *opening_sink;

void *mp_spatial_create(int codec, void *synchronizer)
{
    struct fake_sink *s = opening_sink;
    if (!s) return NULL;
    s->codec = codec;
    s->synchronizer = synchronizer;
    return s->refuse ? NULL : s;
}
void mp_spatial_begin(void *ctx, int64_t time)
{
    struct fake_sink *s = ctx;
    s->begins++;
    s->begin_time = time;
    [s->received setLength:0];
}
void mp_spatial_write(void *ctx, const void *data, size_t size)
{
    [((struct fake_sink *)ctx)->received appendBytes:data length:size];
}
bool mp_spatial_ready(void *ctx) { return ((struct fake_sink *)ctx)->ready; }
bool mp_spatial_failed(void *ctx) { return ((struct fake_sink *)ctx)->failed; }
void mp_spatial_destroy(void *ctx) { ((struct fake_sink *)ctx)->closes++; }
void mp_spatial_volume(void *ctx, float volume)
{
    struct fake_sink *s = ctx;
    s->volumes++;
    s->volume = volume;
}
// The feeder reports readiness asynchronously after processing the final input.
void mp_spatial_end(void *ctx)
{
    struct fake_sink *s = ctx;
    s->ends++;
    if (!s->defer_ready)
        s->ready = true;
}

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static struct fake_sink *new_sink(void)
{
    struct fake_sink *s = calloc(1, sizeof(*s));
    for (int i = 0; i < 2; i++)
        atomic_init(&s->player.spatial_format_latch[i], false);
    s->received = [[NSMutableData alloc] init];
    return s;
}

// TrueHD passthrough as ad_spdif sets it up: 192 kHz, 8 channels of 16 bits.
static struct ao *new_ao(struct fake_sink *s, int format)
{
    struct ao *ao = calloc(1, sizeof(*ao));
    struct priv *p = calloc(1, sizeof(*p));
    opening_sink = s;
    static struct mpv_global no_sink_player;
    ao->global = s ? &s->player : &no_sink_player;
    ao->priv = p;
    ao->format = format;
    ao->samplerate = 192000;
    ao->sstride = 16;
    // As ao_alloc() does, before any init().
    atomic_store(&ao->gain, 1.0f);
    audio_out_spatial.set_gain(ao, 1.0f);
    return ao;
}

static struct mp_pcm_state state_of(struct ao *ao)
{
    struct mp_pcm_state state = {0};
    get_state(ao, &state);
    return state;
}

// One MAT burst lasts 20 ms: 3840 samples at 192 kHz.
static void write_burst(struct ao *ao, uint8_t fill)
{
    uint8_t burst[3840 * 16];
    memset(burst, fill, sizeof(burst));
    void *planes[] = {burst};
    audio_write(ao, planes, 3840);
}

int main(void)
{
    @autoreleasepool {
        // ao_alloc() sets the gain before init(); nothing is open to tell.
        struct fake_sink *early = new_sink();
        struct ao *unopened = new_ao(early, AF_FORMAT_S_TRUEHD);
        audio_out_spatial.set_gain(unopened, 1.0f);
        CHECK(early->volumes == 0, "a gain set before init() reaches no renderer");

        // Without a sink, or for PCM, the output refuses, so mpv uses the next one.
        struct ao *none = new_ao(NULL, AF_FORMAT_S_TRUEHD);
        CHECK(init(none) < 0, "init without a sink must fail");

        struct fake_sink *s = new_sink();
        struct ao *pcm = new_ao(s, AF_FORMAT_S16);
        CHECK(init(pcm) < 0, "init for PCM must fail");
        CHECK(s->codec == 0, "open must not be called for PCM");

        s->refuse = true;
        struct ao *refused = new_ao(s, AF_FORMAT_S_TRUEHD);
        CHECK(init(refused) < 0, "init must fail when the renderer refuses");
        CHECK(s->codec == 1, "open gets the codec");
        s->refuse = false;

        struct ao *dts = new_ao(s, AF_FORMAT_S_DTSHD);
        CHECK(init(dts) == 0 && s->codec == 2, "DTS-HD opens as DTS-HD");
        uninit(dts);

        // TrueHD: opened with the synchronizer the clock runs on, at rate 0.
        struct ao *ao = new_ao(s, AF_FORMAT_S_TRUEHD);
        struct priv *p = ao->priv;
        s->begins = 0;
        s->volumes = 0;
        CHECK(init(ao) == 0, "init for TrueHD succeeds");
        CHECK(s->synchronizer == p->synchronizer && s->synchronizer != nil, "open gets the synchronizer");
        CHECK(s->begins == 1, "init begins a timeline");
        CHECK(ao->device_buffer == 192000 * 3, "mpv may queue 3 s");
        CHECK(p->synchronizer.rate == 0, "the clock waits at rate 0");
        // mpv cannot scale a bitstream, so its volume and mute go to the renderer
        // as mpv sets them (ao_set_gain() calls the driver), paused or not.
        CHECK(s->volumes == 1 && s->volume == 1.0f, "open hands the renderer mpv's volume");
        set_pause(ao, true);
        audio_out_spatial.set_gain(ao, 0.0f);
        CHECK(s->volumes == 2 && s->volume == 0.0f, "a change reaches the renderer at once, paused too, mute as 0");
        state_of(ao);
        CHECK(s->volumes == 2, "reading the state sends nothing more");
        set_pause(ao, false);
        audio_out_spatial.set_gain(ao, 1.0f);

        // Bursts reach the renderer unchanged, before and after start().
        write_burst(ao, 0x5a);
        start(ao);
        for (int n = 0; n < 9; n++)
            write_burst(ao, (uint8_t)n);
        CHECK(s->received.length == 10 * 3840 * 16, "every byte reaches the renderer (%lu)", (unsigned long)s->received.length);
        CHECK(((const uint8_t *)s->received.bytes)[0] == 0x5a, "bytes arrive unchanged");

        // While the renderer is not ready, the clock holds and everything counts as queued.
        struct mp_pcm_state st = state_of(ao);
        CHECK(st.playing, "reports playing while the clock waits, or mpv sees an underrun");
        CHECK(fabs(st.delay - 0.2) < 0.001, "delay is all written audio while waiting (%f)", st.delay);
        CHECK(st.free_samples == 192000 * 3 - 38400, "free space is the rest (%d)", st.free_samples);
        int before = atomic_load(&wakeups);
        usleep(60 * 1000);
        CHECK(fabs(state_of(ao).delay - 0.2) < 0.001, "the clock does not move while waiting");
        CHECK(atomic_load(&wakeups) > before, "the ao thread is nudged while waiting");

        // Once ready, the clock runs from the timeline's start. Sleeps on a
        // busy machine overshoot, so the delay is checked against the time
        // that actually passed.
        s->ready = true;
        st = state_of(ao);
        CFTimeInterval started = CACurrentMediaTime();
        CHECK(p->synchronizer.rate == 1, "the clock starts when the renderer is ready");
        usleep(50 * 1000);
        double delay = state_of(ao).delay;
        double expected = MPMAX(0.2 - (CACurrentMediaTime() - started), 0);
        CHECK(fabs(delay - expected) < 0.02, "delay falls in real time (%f, expected %f)", delay, expected);
        usleep(15 * 1000); // lets a timer tick already in flight land
        before = atomic_load(&wakeups);
        usleep(40 * 1000);
        CHECK(atomic_load(&wakeups) == before, "no nudging once the clock runs");

        // Pause stops the clock, resume restarts it.
        set_pause(ao, true);
        CHECK(p->synchronizer.rate == 0, "pause stops the clock");
        delay = state_of(ao).delay;
        usleep(50 * 1000);
        CHECK(fabs(state_of(ao).delay - delay) < 0.002, "the clock holds while paused");
        set_pause(ao, false);
        CHECK(p->synchronizer.rate == 1, "resume restarts the clock");

        // Playing out everything ends the stream: mpv reads that as EOF.
        usleep(150 * 1000);
        st = state_of(ao);
        CHECK(!st.playing && st.delay == 0, "all audio played ends playback (%f)", st.delay);

        // Audio after an underrun would land in the past on the old timeline,
        // so it starts a new one, which waits for the renderer again.
        int begins = s->begins;
        s->ready = false;
        write_burst(ao, 2);
        CHECK(s->begins == begins + 1, "a write after an underrun begins a new timeline");
        CHECK(!p->clock_running && p->synchronizer.rate == 0, "the new timeline waits for the renderer");
        // mpv may not have seen the underrun, and then calls no start().
        CHECK(p->playing && atomic_load(&p->waiting), "playback stays active across the new timeline");
        CHECK(fabs(state_of(ao).delay - 0.02) < 0.001, "only the new audio is queued");

        // A seek begins a new timeline at the current time and waits for the renderer again.
        begins = s->begins;
        reset(ao);
        CHECK(s->begins == begins + 1 && s->received.length == 0, "reset begins a new timeline");
        CHECK(p->synchronizer.rate == 0 && !p->clock_running, "reset stops the clock");
        CHECK(s->begin_time == ns_from_time(CMTimebaseGetTime(p->synchronizer.timebase)), "the timeline starts at the clock's time");
        for (int i = 0; i < 3; i++)
            write_burst(ao, 1);
        start(ao);
        CHECK(fabs(state_of(ao).delay - 0.06) < 0.001, "delay restarts from the new timeline");

        // At the end the renderer lets go of what it holds back, so audio shorter
        // than its startup buffer still plays.
        s->defer_ready = true;
        end_of_stream(ao);
        CHECK(s->ends == 1, "the renderer hears the end");
        state_of(ao);
        CHECK(!p->clock_running && atomic_load(&p->waiting), "EOF waits for the feeder to finish");
        CHECK(state_of(ao).free_samples == 0, "an unfinished feeder cannot admit the next file");
        // The production Swift feeder test covers readiness with no decoded
        // audio, including a tail that ends before a TrueHD major sync.
        s->ready = true;
        state_of(ao);
        CHECK(p->clock_running && p->synchronizer.rate == 1, "the clock starts once the renderer has flushed");
        CHECK(state_of(ao).free_samples == 0, "the next file waits until this one has played out");
        set_pause(ao, true);
        state_of(ao);
        CHECK(!atomic_load(&p->waiting), "no nudging while paused at the end, as nothing plays out");
        set_pause(ao, false);
        CHECK(state_of(ao).delay > 0 && atomic_load(&p->waiting), "nudging resumes with playback");
        int nudged = atomic_load(&wakeups);
        usleep(100 * 1000);
        CHECK(atomic_load(&wakeups) > nudged, "the ao thread is nudged until the ended timeline has played out");
        state_of(ao);
        CHECK(!atomic_load(&p->waiting), "and no longer once it has");
        begins = s->begins;
        write_burst(ao, 3);
        CHECK(s->begins == begins + 1 && !p->ended, "the next file begins a new timeline");
        CHECK(p->playing, "and keeps playing, as mpv sends no start() between gapless files");

        // A long run stays representable: 14 h at 192 kHz overflows written * 1e9.
        reset(ao);
        CMTime now = CMTimebaseGetTime(p->synchronizer.timebase);
        p->timeline = CMTimeSubtract(now, CMTimeMake(14 * 3600, 1));
        p->written = INT64_C(192000) * 14 * 3600 + 19200; // and 0.1 s still to play
        CHECK(fabs(state_of(ao).delay - 0.1) < 0.001, "a 14 h timeline keeps its delay (%f)", state_of(ao).delay);

        // A renderer failure asks mpv to reopen the output, once.
        s->failed = true;
        state_of(ao);
        state_of(ao);
        CHECK(reloads == 1, "a failure requests one reload (%d)", reloads);

        uninit(ao);
        CHECK(s->closes == 2, "uninit closes the sink (%d)", s->closes);
        before = atomic_load(&wakeups);
        usleep(30 * 1000);
        CHECK(atomic_load(&wakeups) == before, "no wakeups after uninit");

        // A second player neither inherits the failure nor clears it when
        // it probes PCM. The first player's new AO must still refuse TrueHD.
        struct fake_sink *independent = new_sink();
        struct ao *independent_ao = new_ao(independent, AF_FORMAT_S_TRUEHD);
        CHECK(init(independent_ao) == 0, "another player can still open TrueHD");
        CHECK(init(new_ao(independent, AF_FORMAT_FLOAT)) < 0,
              "another player's PCM probe leaves the first latch alone");
        uninit(independent_ao);

        // The reopen refuses the failed format without asking the renderer, so
        // mpv falls back to decoding it.
        s->failed = false;
        s->codec = 0;
        struct ao *reopened = new_ao(s, AF_FORMAT_S_TRUEHD);
        CHECK(init(reopened) < 0, "the reopen after a failure refuses TrueHD");
        CHECK(s->codec == 0, "and does not offer it to the renderer");
        struct ao *other = new_ao(s, AF_FORMAT_S_DTSHD);
        CHECK(init(other) == 0, "DTS-HD is not latched by a TrueHD failure");
        uninit(other);
        struct ao *ac3 = new_ao(s, AF_FORMAT_S_AC3);
        CHECK(init(ac3) < 0, "AC-3 goes to the next output");
        s->codec = 0;
        CHECK(init(new_ao(s, AF_FORMAT_S_TRUEHD)) < 0, "another passthrough format leaves the latch");
        CHECK(s->codec == 0, "and the renderer still is not asked");

        // Once the decoded track opens a PCM output, the next track tries the
        // renderer again.
        CHECK(init(new_ao(s, AF_FORMAT_FLOAT)) < 0, "PCM goes to the next output");
        struct ao *retried = new_ao(s, AF_FORMAT_S_TRUEHD);
        CHECK(init(retried) == 0 && s->codec == 1,
              "a PCM initialization clears the latch");
        uninit(retried);
    }
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
