// Unit test for the "spatial" audio output
// (patches/mpv/pool/0039-ao-spatial-render-truehd-atmos-and-dts-x.patch).
// It drives the driver the way mpv's push thread does, against a real
// synchronizer and a fake sink. Run scripts/test_ao_spatial.sh, which builds
// it against the patched mpv source.

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/out/ao_spatial.m"

// The parts of mpv the driver calls.
static int reloads;
void mp_msg(struct mp_log *log, int lev, const char *format, ...) {}
void ao_request_reload(struct ao *ao) { reloads++; }
bool af_fmt_is_spdif(int format) { return format >= AF_FORMAT_S_AAC && format < AF_FORMAT_COUNT; }
const char *af_fmt_to_str(int format) { return "spdif"; }
int af_fmt_to_bytes(int format) { return 2; }

struct fake_sink {
    struct mpv_global player;
    bool refuse;
    int codec;
    size_t buffer_limit;
    AVSampleBufferRenderSynchronizer *synchronizer;
    int begins, ends, closes, volumes, failure_reads;
    float volume;
    int64_t begin_time;
    // Bytes the sink takes before it is full.
    size_t space;
    NSMutableData *received;
    bool ready, final;
    int failure;
    int64_t covered;
    // Enqueued audio a route change flushed from the renderer.
    int64_t missing;
};

static struct fake_sink *opening_sink;

void *mp_spatial_create(int codec, void *synchronizer, size_t buffer_limit)
{
    struct fake_sink *s = opening_sink;
    if (!s) return NULL;
    s->codec = codec;
    s->synchronizer = synchronizer;
    s->buffer_limit = buffer_limit;
    return s->refuse ? NULL : s;
}
void mp_spatial_begin(void *ctx, int64_t time)
{
    struct fake_sink *s = ctx;
    s->begins++;
    s->begin_time = time;
    s->ready = s->final = false;
    s->failure = MP_SPATIAL_OK;
    s->covered = INT64_MIN;
    s->missing = 0;
    [s->received setLength:0];
}
// Like the real sink, a failed timeline takes nothing until the next begin.
size_t mp_spatial_write(void *ctx, const void *data, size_t size)
{
    struct fake_sink *s = ctx;
    size_t taken = s->failure ? 0 : MPMIN(size, s->space);
    s->space -= taken;
    [s->received appendBytes:data length:taken];
    return taken;
}
void mp_spatial_end(void *ctx) { ((struct fake_sink *)ctx)->ends++; }
void mp_spatial_destroy(void *ctx) { ((struct fake_sink *)ctx)->closes++; }
void mp_spatial_volume(void *ctx, float volume)
{
    struct fake_sink *s = ctx;
    s->volumes++;
    s->volume = volume;
}
int mp_spatial_poll(void *ctx, bool *ready, int64_t *covered, int64_t *end, bool *final)
{
    struct fake_sink *s = ctx;
    *ready = s->ready;
    *covered = s->covered;
    *end = s->covered == INT64_MIN ? INT64_MIN : s->covered + s->missing;
    *final = s->final;
    return s->failure;
}
char *mp_spatial_failure(void *ctx)
{
    struct fake_sink *s = ctx;
    s->failure_reads++;
    return s->failure ? strdup("fake failure") : NULL;
}

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define MS INT64_C(1000000)
#define BURST_SAMPLES 3840
#define SSTRIDE 16

static struct fake_sink *new_sink(void)
{
    struct fake_sink *s = calloc(1, sizeof(*s));
    for (int i = 0; i < 2; i++)
        atomic_init(&s->player.spatial_format_latch[i], false);
    s->received = [[NSMutableData alloc] init];
    s->space = SIZE_MAX;
    return s;
}

// TrueHD passthrough as ad_spdif sets it up: 192 kHz, 8 channels of 16 bits.
// ao_init() sets sstride only after the driver's init().
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
    ao->channels.num = 8;
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

static int64_t now_of(struct ao *ao)
{
    return ns_from_time([((struct priv *)ao->priv)->synchronizer currentTime]);
}

// The synchronizer applies rate changes asynchronously: give the timebase a
// moment to reach the rate the driver asked for, then report what it has.
static float rate_of(struct ao *ao)
{
    struct priv *p = ao->priv;
    float asked = p->running ? 1 : 0;
    for (int n = 0; n < 200 && CMTimebaseGetRate(p->synchronizer.timebase) != asked; n++)
        usleep(1000);
    return CMTimebaseGetRate(p->synchronizer.timebase);
}

// One MAT burst lasts 20 ms.
static void write_burst(struct ao *ao, uint8_t fill)
{
    uint8_t burst[BURST_SAMPLES * SSTRIDE];
    memset(burst, fill, sizeof(burst));
    void *planes[] = {burst};
    CHECK(audio_write(ao, planes, BURST_SAMPLES), "a burst within free_samples is accepted");
}

int main(void)
{
    @autoreleasepool {
        // ao_alloc() sets the gain before init(); nothing is open to tell.
        struct fake_sink *early = new_sink();
        new_ao(early, AF_FORMAT_S_TRUEHD);
        CHECK(early->volumes == 0, "a gain set before init() reaches no sink");

        // Without a sink, or for PCM, the output refuses, so mpv uses the next one.
        struct ao *none = new_ao(NULL, AF_FORMAT_S_TRUEHD);
        CHECK(init(none) < 0, "init without a sink must fail");

        struct fake_sink *s = new_sink();
        struct ao *pcm = new_ao(s, AF_FORMAT_S16);
        CHECK(init(pcm) < 0, "init for PCM must fail");
        CHECK(s->codec == 0, "open must not be called for PCM");

        s->refuse = true;
        struct ao *refused = new_ao(s, AF_FORMAT_S_TRUEHD);
        CHECK(init(refused) < 0, "init must fail when the sink refuses");
        CHECK(s->codec == 1, "open gets the codec");
        s->refuse = false;

        struct ao *dts = new_ao(s, AF_FORMAT_S_DTSHD);
        CHECK(init(dts) == 0 && s->codec == 2, "DTS-HD opens as DTS-HD");
        uninit(dts);

        // TrueHD: opened with the synchronizer the clock runs on, at rate 0.
        struct ao *ao = new_ao(s, AF_FORMAT_S_TRUEHD);
        struct priv *p = ao->priv;
        ao->stream_silence = true;
        s->begins = 0;
        s->volumes = 0;
        CHECK(init(ao) == 0, "init for TrueHD succeeds");
        ao->sstride = SSTRIDE;
        CHECK(s->synchronizer == p->synchronizer && s->synchronizer != nil, "open gets the synchronizer");
        CHECK(s->buffer_limit == 192000 * SSTRIDE, "the sink buffers a second (%zu)", s->buffer_limit);
        CHECK(rate_of(ao) == 0, "the clock waits at rate 0");
        CHECK(s->begins == 1 && s->begin_time == now_of(ao), "init begins a timeline at the clock's time");
        CHECK(ao->device_buffer == 192000 / 2, "half a second waits in the output (%d)", ao->device_buffer);
        // Pause-as-silence: with audio-stream-silence mpv would never pause
        // this output but write zeros through the pause. Clearing the flag
        // before buffer.c reads it routes a pause to set_pause() and a seek
        // to reset(), as for any output.
        CHECK(!ao->stream_silence, "the output opts out of streamed silence");

        // mpv cannot scale a bitstream, so its volume and mute go to the sink
        // as mpv sets them (ao_set_gain() calls the driver), paused or not.
        CHECK(s->volumes == 1 && s->volume == 1.0f, "open hands the sink mpv's volume");
        set_pause(ao, true);
        audio_out_spatial.set_gain(ao, 0.0f);
        CHECK(s->volumes == 2 && s->volume == 0.0f, "a change reaches the sink at once, paused too, mute as 0");
        state_of(ao);
        CHECK(s->volumes == 2, "reading the state sends nothing more");
        set_pause(ao, false);
        audio_out_spatial.set_gain(ao, 1.0f);

        // Bursts reach the sink unchanged. What it cannot take yet waits in
        // the output, which admits only what it can hold.
        s->space = 2 * BURST_SAMPLES * SSTRIDE;
        write_burst(ao, 0x5a);
        write_burst(ao, 1);
        write_burst(ao, 2);
        CHECK(s->received.length == 2 * BURST_SAMPLES * SSTRIDE, "the sink takes what fits (%lu)",
              (unsigned long)s->received.length);
        CHECK(((const uint8_t *)s->received.bytes)[0] == 0x5a, "bytes arrive unchanged");
        struct mp_pcm_state st = state_of(ao);
        CHECK(st.free_samples == 96000 - 3840, "the rest waits and counts against free space (%d)", st.free_samples);
        s->space = SIZE_MAX;
        st = state_of(ao);
        CHECK(s->received.length == 3 * BURST_SAMPLES * SSTRIDE && st.free_samples == 96000,
              "the output offers it again when the state is read");
        CHECK(((const uint8_t *)s->received.bytes)[2 * BURST_SAMPLES * SSTRIDE] == 2, "in order");

        // Until the sink is ready, the clock holds and everything counts as queued.
        start(ao);
        for (int n = 0; n < 30; n++)
            write_burst(ao, 3);
        s->covered = s->begin_time + 100 * MS;
        st = state_of(ao);
        CHECK(rate_of(ao) == 0, "the clock waits for the sink's readiness");
        CHECK(st.playing, "reports playing while the clock waits, or mpv sees an underrun");
        CHECK(fabs(st.delay - 0.66) < 0.001, "delay is all written audio while waiting (%f)", st.delay);
        usleep(30 * 1000);
        CHECK(now_of(ao) == s->begin_time, "the clock does not move while waiting");

        // Ready but short of the lead: still waiting.
        s->ready = true;
        state_of(ao);
        CHECK(rate_of(ao) == 0, "the first start needs the resume lead too");
        s->covered = s->begin_time + 600 * MS;
        st = state_of(ao);
        CHECK(rate_of(ao) == 1, "ready with the resume lead starts the clock");
        CHECK(fabs(st.delay - 0.66) < 0.005, "the delay still counts everything written (%f)", st.delay);

        // The clock stops short of the end of what the renderer holds, and
        // resumes once enough is queued again, on the same timeline.
        usleep(20 * 1000);
        s->covered = now_of(ao) + 200 * MS;
        state_of(ao);
        CHECK(rate_of(ao) == 0, "the clock holds short of the end of the converted audio");
        int64_t held = now_of(ao);
        usleep(30 * 1000);
        CHECK(now_of(ao) == held, "a hold keeps the time");
        s->covered = held + 400 * MS;
        state_of(ao);
        CHECK(rate_of(ao) == 0, "it does not resume below the resume lead");
        int begins = s->begins;
        s->covered = held + 600 * MS;
        state_of(ao);
        CHECK(rate_of(ao) == 1 && s->begins == begins, "it resumes with the lead, without a new timeline");

        // A full sink means the renderer holds all it can, or conversion is
        // behind; the driver cannot tell which. Either way the clock never
        // runs past the converted audio. State reads come every 125 ms, as
        // mpv's ao thread makes them while it streams.
        s->covered = now_of(ao) + 100 * MS;
        state_of(ao);
        CHECK(rate_of(ao) == 0, "held");
        s->space = 0;
        write_burst(ao, 4);
        int changes = 0;
        bool overran = false;
        for (int n = 0; n < 6; n++) {
            float before = rate_of(ao);
            state_of(ao);
            changes += rate_of(ao) != before;
            overran |= now_of(ao) > s->covered;
            usleep(125 * 1000);
        }
        CHECK(changes == 0 && !overran && rate_of(ao) == 0,
              "a full sink below the stop lead keeps the clock held (%d rate changes)", changes);
        s->covered = now_of(ao) + 300 * MS;
        state_of(ao);
        CHECK(rate_of(ao) == 1, "a full sink resumes the clock with the stop lead");
        for (int n = 0; n < 4; n++) {
            usleep(125 * 1000);
            state_of(ao);
            overran |= now_of(ao) > s->covered;
        }
        CHECK(!overran && rate_of(ao) == 0, "and holds it again before the converted audio runs out");
        s->space = SIZE_MAX;
        s->covered = now_of(ao) + 2000 * MS;
        state_of(ao);
        CHECK(rate_of(ao) == 1 && !p->full, "the waiting burst goes through");

        // A pause stops the clock at once; the next state read resumes it.
        set_pause(ao, true);
        CHECK(rate_of(ao) == 0, "pause stops the clock");
        set_pause(ao, false);
        CHECK(rate_of(ao) == 0, "unpausing waits for the state read");
        state_of(ao);
        CHECK(rate_of(ao) == 1, "the state read resumes it");

        // Silence-only input, as streamed silence would be, converts to
        // nothing. The clock plays out what the renderer holds and idles
        // there, without failing, until audio arrives again.
        int64_t idle_start = now_of(ao);
        s->covered = idle_start + 600 * MS;
        for (int n = 0; n < 100; n++) {
            write_burst(ao, 0);
            state_of(ao);
            usleep(10 * 1000);
        }
        CHECK(rate_of(ao) == 0, "silence-only input holds the clock");
        int64_t idle = now_of(ao);
        CHECK(idle - idle_start > 300 * MS && idle < s->covered,
              "after playing out what was held, short of its end (%lld ms)", (long long)((idle - idle_start) / MS));
        CHECK(reloads == 0 && !p->reload_requested, "silence never falls back to PCM");
        s->covered = idle + 600 * MS;
        state_of(ao);
        CHECK(rate_of(ao) == 1, "audio after the silence resumes the clock");

        // A gap in the passthrough stream: the queued audio is gone, and what
        // the sink had not taken yet starts a new timeline at once.
        s->space = 0;
        write_burst(ao, 5);
        s->space = SIZE_MAX;
        s->failure = MP_SPATIAL_DISCONTINUITY;
        begins = s->begins;
        int64_t gap = now_of(ao);
        st = state_of(ao);
        CHECK(s->begins == begins + 1 && s->begin_time >= gap, "a discontinuity begins a new timeline at the clock's time");
        CHECK(s->received.length == BURST_SAMPLES * SSTRIDE && ((const uint8_t *)s->received.bytes)[0] == 5,
              "the untaken burst is the new timeline's first audio");
        CHECK(rate_of(ao) == 0 && p->written == BURST_SAMPLES, "the clock waits for the new timeline (%lld)",
              (long long)p->written);
        CHECK(fabs(st.delay - 0.02) < 0.005 && reloads == 0, "only that burst is queued (%f)", st.delay);

        // End of stream reaches the sink after the last waiting byte.
        s->space = 0;
        write_burst(ao, 6);
        end_of_stream(ao);
        CHECK(s->ends == 0, "the end waits for the bytes before it");
        s->space = SIZE_MAX;
        st = state_of(ao);
        CHECK(s->ends == 1, "the end follows them");
        CHECK(st.playing && st.free_samples == 0, "the next file waits for this one to play out");
        state_of(ao);
        CHECK(s->ends == 1, "the end is sent once");
        // A route change flushed part of the finished stream: its tail waits
        // until the renderer holds it again, and the stream is not drained.
        s->ready = s->final = true;
        s->covered = now_of(ao) + 100 * MS;
        s->missing = 300 * MS;
        st = state_of(ao);
        CHECK(rate_of(ao) == 0 && st.playing, "a flushed tail waits for the renderer");
        // A finished stream plays out to its end, below the stop lead too.
        s->missing = 0;
        st = state_of(ao);
        CHECK(rate_of(ao) == 1 && st.playing, "the final tail plays");
        usleep(130 * 1000);
        st = state_of(ao);
        CHECK(!st.playing && st.delay == 0 && st.free_samples > 0, "a drained stream releases the next file");
        CHECK(rate_of(ao) == 0, "a drained stream stops the clock");
        begins = s->begins;
        write_burst(ao, 7);
        CHECK(s->begins == begins + 1 && !p->ended && p->playing && rate_of(ao) == 0,
              "a gapless next file begins a timeline and keeps playing");

        // A stream that ends with nothing to play drains at once.
        reset(ao);
        CHECK(s->received.length == 0 && !p->playing && rate_of(ao) == 0, "a seek discards the old carrier");
        for (int i = 0; i < 3; i++)
            write_burst(ao, 0);
        start(ao);
        end_of_stream(ao);
        CHECK(state_of(ao).playing, "the end waits for the sink");
        s->ready = s->final = true;
        s->covered = s->begin_time;
        st = state_of(ao);
        CHECK(!st.playing && st.delay == 0, "an empty end needs no audio clock");

        // 14 h of carrier stays representable.
        reset(ao);
        p->timeline = now_of(ao) - INT64_C(14) * 3600 * 1000000000;
        p->written = INT64_C(192000) * 14 * 3600 + 19200;
        CHECK(fabs(state_of(ao).delay - 0.1) < 0.001, "14 h carrier accounting remains representable");

        // A sink failure asks mpv to reopen the output, once, and stops the clock.
        start(ao);
        s->ready = true;
        s->covered = now_of(ao) + 2000 * MS;
        state_of(ao);
        CHECK(rate_of(ao) == 1, "running before the failure");
        s->failure = MP_SPATIAL_FAILED;
        state_of(ao);
        state_of(ao);
        CHECK(reloads == 1 && s->failure_reads == 1, "a failure is logged and requests one reload (%d)", reloads);
        CHECK(rate_of(ao) == 0, "and stops the clock");

        uninit(ao);
        CHECK(s->closes == 2, "uninit closes the sink (%d)", s->closes);

        // A second player neither inherits the failure nor clears it when
        // it probes PCM. The first player's new AO must still refuse TrueHD.
        struct fake_sink *independent = new_sink();
        struct ao *independent_ao = new_ao(independent, AF_FORMAT_S_TRUEHD);
        CHECK(init(independent_ao) == 0, "another player can still open TrueHD");
        CHECK(init(new_ao(independent, AF_FORMAT_FLOAT)) < 0,
              "another player's PCM probe leaves the first latch alone");
        uninit(independent_ao);

        // The reopen refuses the failed format without asking the sink, so
        // mpv falls back to decoding it.
        s->codec = 0;
        struct ao *reopened = new_ao(s, AF_FORMAT_S_TRUEHD);
        CHECK(init(reopened) < 0, "the reopen after a failure refuses TrueHD");
        CHECK(s->codec == 0, "and does not offer it to the sink");
        struct ao *other = new_ao(s, AF_FORMAT_S_DTSHD);
        CHECK(init(other) == 0, "DTS-HD is not latched by a TrueHD failure");
        uninit(other);
        struct ao *ac3 = new_ao(s, AF_FORMAT_S_AC3);
        CHECK(init(ac3) < 0, "AC-3 goes to the next output");
        s->codec = 0;
        CHECK(init(new_ao(s, AF_FORMAT_S_TRUEHD)) < 0, "another passthrough format leaves the latch");
        CHECK(s->codec == 0, "and the sink still is not asked");

        // Once the decoded track opens a PCM output, the next track tries the
        // sink again.
        CHECK(init(new_ao(s, AF_FORMAT_FLOAT)) < 0, "PCM goes to the next output");
        struct ao *retried = new_ao(s, AF_FORMAT_S_TRUEHD);
        CHECK(init(retried) == 0 && s->codec == 1,
              "a PCM initialization clears the latch");
        uninit(retried);
    }
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
