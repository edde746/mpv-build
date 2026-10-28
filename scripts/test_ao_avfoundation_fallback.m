// Behavioral test for ao_avfoundation's compressed (AVPlayer) sink when the
// stream never starts
// (patches/mpv/pool/0040-ao-decode-the-playing-track-to-pcm-when-passthrough-fails.patch,
// patches/mpv/pool/0041-avfoundation-say-when-avplayer-never-asked-for-the-stream.patch),
// and of the clock anchor it keeps while the playhead is parked
// (patches/mpv/pool/0042-avfoundation-anchor-the-passthrough-clock-to-the-playhead.patch).
// It drives the driver the way mpv does, against a real AVPlayer, in a process
// whose main thread never services the main dispatch queue. AVPlayer attaches
// an item there, so it never asks the resource loader for the stream and the
// startup supervisor has to give up. mp_time_ns() is a clock the test moves,
// so the supervisor's two 2 s deadlines pass at once. Run
// scripts/test_ao_avfoundation_fallback.sh, which builds it against the
// patched mpv source.

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/out/ao_avfoundation.m"

// The parts of mpv the driver calls.
static _Atomic int reloads, fallbacks, reads, streaming_stops, anchors;
static _Atomic int64_t clock_ns, end_time_ns;
int64_t mp_time_ns(void) { return atomic_load(&clock_ns); }
void ao_request_reload(struct ao *ao) { reloads++; }
void ao_request_pcm_fallback(struct ao *ao) { fallbacks++; }
void ao_stop_streaming(struct ao *ao) { streaming_stops++; }
void ao_set_end_time_ns(struct ao *ao, int64_t out_time_ns)
{
    anchors++;
    atomic_store(&end_time_ns, out_time_ns);
}
bool af_fmt_is_spdif(int format) { return format >= AF_FORMAT_S_AAC && format < AF_FORMAT_COUNT; }
int af_fmt_to_bytes(int format) { return 2; }
void *(ta_alloc_size)(void *ta_parent, size_t size) { return calloc(1, size); }
void *ta_dbg_set_loc(void *ptr, const char *name) { return ptr; }
void ta_free(void *ptr) { free(ptr); }
const m_option_type_t m_option_type_bool = {0}, m_option_type_double = {0};
// Only the PCM path calls these, and this test opens compressed outputs only.
int af_fmt_from_planar(int format) { abort(); }
bool af_fmt_is_planar(int format) { abort(); }
void ca_fill_asbd(struct ao *ao, AudioStreamBasicDescription *asbd) { abort(); }
AudioChannelLayout *ca_get_acl(struct ao *ao, size_t *out_layout_size) { abort(); }
void ca_get_device_list(struct ao *ao, struct ao_device_list *list) { abort(); }
CFStringRef cfstr_from_cstr(const char *str) { abort(); }
char *cfstr_get_cstr(const CFStringRef cfstr) { abort(); }

// Messages at MSGL_INFO and above, kept for the checks and echoed when severe.
static struct { int lev; char text[200]; } lines[1024];
static int line_count;
static pthread_mutex_t lines_lock = PTHREAD_MUTEX_INITIALIZER;

void mp_msg(struct mp_log *log, int lev, const char *format, ...)
{
    if (lev > MSGL_INFO)
        return;
    char text[200];
    va_list va;
    va_start(va, format);
    vsnprintf(text, sizeof(text), format, va);
    va_end(va);
    if (lev <= MSGL_WARN)
        printf("log: %s", text);
    pthread_mutex_lock(&lines_lock);
    if (line_count < MP_ARRAY_SIZE(lines)) {
        lines[line_count].lev = lev;
        snprintf(lines[line_count].text, sizeof(lines[0].text), "%s", text);
        line_count++;
    }
    pthread_mutex_unlock(&lines_lock);
}

// Lines logged at exactly lev that contain needle; NULL counts every line at
// lev or above.
static int logged(int lev, const char *needle)
{
    int n = 0;
    pthread_mutex_lock(&lines_lock);
    for (int i = 0; i < line_count; i++) {
        n += needle ? lines[i].lev == lev && strstr(lines[i].text, needle)
                    : lines[i].lev <= lev;
    }
    pthread_mutex_unlock(&lines_lock);
    return n;
}

// ad_spdif's output: one IEC 61937 burst per (E-)AC-3 syncframe, Pa Pb Pc Pd
// and the frame as 16-bit little-endian words, zero-padded to the burst
// period, repeated for as long as the driver reads.
static uint8_t carrier[24576];
static int carrier_size, carrier_pos;

int ao_read_data(struct ao *ao, void **data, int samples, int64_t out_time_ns,
                 bool *eof, bool pad_silence, bool blocking)
{
    reads++;
    uint8_t *dst = data[0];
    for (int i = 0; i < samples * ao->sstride; i++) {
        dst[i] = carrier[carrier_pos];
        carrier_pos = (carrier_pos + 1) % carrier_size;
    }
    *eof = false;
    return samples;
}

static void load_carrier(int format)
{
    // 384-byte (96 kb/s) 48 kHz 2/0 syncframes of 1536 samples. Only the
    // headers carry anything; AVPlayer never asks for the silence after them.
    // E-AC-3: independent substream 0, frmsiz 191, fscod 0, 6 blocks,
    // acmod 2, bsid 16, dialnorm 31.
    static const uint8_t eac3[] = {0x0b, 0x77, 0x00, 0xbf, 0x34, 0x87, 0xc0};
    // AC-3: crc1, fscod 0 with frmsizecod 12, bsid 8, acmod 2.
    static const uint8_t ac3[] = {0x0b, 0x77, 0x00, 0x00, 0x0c, 0x40, 0x40};
    bool is_eac3 = format == AF_FORMAT_S_EAC3;
    uint8_t frame[384] = {0};
    memcpy(frame, is_eac3 ? eac3 : ac3, sizeof(eac3));
    // Pd counts bytes for E-AC-3 and bits for AC-3.
    int pd = is_eac3 ? sizeof(frame) : sizeof(frame) * 8;
    uint8_t header[IEC61937_HEADER_SIZE] = {
        0x72, 0xf8, 0x1f, 0x4e, is_eac3 ? IEC61937_EAC3 : IEC61937_AC3, 0, pd & 0xff, pd >> 8,
    };
    memset(carrier, 0, sizeof(carrier));
    memcpy(carrier, header, sizeof(header));
    for (int i = 0; i < sizeof(frame); i += 2) {
        carrier[sizeof(header) + i] = frame[i + 1];
        carrier[sizeof(header) + i + 1] = frame[i];
    }
    // A burst period is 4 bytes per frame of 1536 samples, at 4x for E-AC-3.
    carrier_size = is_eac3 ? 24576 : 6144;
    carrier_pos = 0;
}

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define GRACE AVP_START_GRACE_NS
#define UNASKED "AVPlayer has made no resource requests"
#define SSTRIDE 4

static void set_clock(int64_t ns) { atomic_store(&clock_ns, ns); }

// A compressed output as ad_spdif sets it up: 16-bit stereo IEC 61937 at
// 192 kHz for E-AC-3, 48 kHz for AC-3. Returns NULL when init() refuses.
static struct ao *open_ao(int format)
{
    struct ao *ao = calloc(1, sizeof(*ao));
    struct priv *p = calloc(1, sizeof(*p));
    // As ao_alloc() does, before any init().
    *p = *(const struct priv *)audio_out_avfoundation.priv_defaults;
    ao->priv = p;
    ao->format = format;
    ao->samplerate = format == AF_FORMAT_S_EAC3 ? 192000 : 48000;
    ao->channels.num = 2;
    atomic_store(&ao->gain, 1.0f);
    load_carrier(format);
    if (audio_out_avfoundation.init(ao) < 0) {
        free(p);
        free(ao);
        return NULL;
    }
    // ao_init() sets sstride only after the driver's init().
    ao->sstride = SSTRIDE;
    return ao;
}

// What ao_uninit() does after uninit(): talloc_free(ao) with its children.
static void free_ao(struct ao *ao)
{
    struct priv *p = ao->priv;
    free(p->stash);
    free(p->es_buf);
    free(p);
    free(ao);
}

// The driver's state, read on its queue; with feed, after what the feed
// timer does on each of its 50 ms ticks.
static struct priv on_queue(struct ao *ao, bool feed)
{
    struct priv *p = ao->priv;
    __block struct priv seen;
    dispatch_sync(p->queue, ^{
        if (feed)
            feed_avp(ao);
        memcpy(&seen, p, sizeof(seen));
    });
    return seen;
}

static struct priv tick(struct ao *ao) { return on_queue(ao, true); }

// Feed until the stream holds its startup lead. The playhead never moves, so
// the feed stops there and the startup deadline no longer moves.
static struct priv prime(struct ao *ao)
{
    struct priv s = tick(ao);
    for (int n = 0; n < 20 && s.es_pts < AVP_START_LEAD_NS(&s); n++)
        s = tick(ao);
    return s;
}

static void *run(void *unused)
{
    @autoreleasepool {
        const int64_t t0 = MP_TIME_S_TO_NS(1000);
        set_clock(t0);
        // No AVAssetResourceLoadingRequest can be made outside AVFoundation.
        // The loader takes a missing one as a request without data, which it
        // finishes at once.
        AVAssetResourceLoadingRequest *no_request = nil;

        // E-AC-3 opens on the AVPlayer path and primes its startup lead.
        struct ao *ao = open_ao(AF_FORMAT_S_EAC3);
        CHECK(ao, "E-AC-3 opens");
        if (!ao)
            return NULL;
        struct priv *p = ao->priv;
        CHECK(p->spdif && p->player, "on the AVPlayer path");
        audio_out_avfoundation.start(ao);
        struct priv s = prime(ao);
        CHECK(s.es_pts >= AVP_START_LEAD_NS(&s), "the stream primes its startup lead (%.3f s)", s.es_pts / 1e9);
        CHECK(s.avp_rate_applied && reads > 0, "and the rate is applied");
        CHECK(!s.avp_loader_asked, "AVPlayer never asks for the stream while nothing services the main queue");
        CHECK(logged(MSGL_WARN, NULL) == 0, "priming warns about nothing");

        // 0042: a tick that reads nothing still anchors the clock, against
        // the playhead AVPlayer has not moved: the clock stands at the start
        // and the pulled stream is all still to play.
        int reads_primed = reads, anchors_primed = anchors;
        set_clock(t0 + GRACE / 2);
        s = tick(ao);
        int64_t written = CMTimeGetNanoseconds(CMTimeMake(s.pulled_samples, ao->samplerate)) - s.es_dropped_ns;
        CHECK(reads == reads_primed && anchors == anchors_primed + 1, "a tick that reads nothing anchors the clock");
        CHECK(written > 0 && end_time_ns == t0 + GRACE / 2 + written,
              "to the end of the pulled stream against the parked playhead (%.3f s of %.3f s ahead)",
              (end_time_ns - (t0 + GRACE / 2)) / 1e9, written / 1e9);

        // The first deadline: a warning, a seek to the start, and (0041) why.
        set_clock(t0 + GRACE - 1);
        tick(ao);
        CHECK(logged(MSGL_WARN, "playback did not engage") == 0, "no warning before the first deadline");
        set_clock(t0 + GRACE);
        s = tick(ao);
        CHECK(logged(MSGL_WARN, "playback did not engage") == 1 && s.avp_start_seeked,
              "the first deadline warns and seeks to the start");
        CHECK(logged(MSGL_WARN, UNASKED) == 1, "and says AVPlayer made no resource requests");
        CHECK(fallbacks == 0 && reloads == 0, "without falling back yet");

        // The second deadline: one PCM fallback for this track, no reload.
        set_clock(t0 + 2 * GRACE - 1);
        tick(ao);
        CHECK(fallbacks == 0 && logged(MSGL_ERR, NULL) == 0, "nothing fails before the second deadline");
        set_clock(t0 + 2 * GRACE);
        s = tick(ao);
        CHECK(logged(MSGL_ERR, "did not start; falling back to PCM") == 1, "the second deadline gives up");
        CHECK(logged(MSGL_ERR, UNASKED) == 1, "saying again that AVPlayer made no resource requests");
        CHECK(fallbacks == 1 && reloads == 0,
              "and requests one PCM fallback (%d), not an output reload (%d)", fallbacks, reloads);
        CHECK(s.spdif_reload_requested && p->player.rate == 0, "the player stops");

        // Nothing after the request feeds or asks again: ticks, a pause and
        // resume, a seek (reset, then start).
        int reads_then = reads, anchors_then = anchors;
        for (int n = 1; n <= 3; n++) {
            set_clock(t0 + (2 + n) * GRACE);
            tick(ao);
        }
        audio_out_avfoundation.set_pause(ao, true);
        audio_out_avfoundation.set_pause(ao, false);
        set_clock(t0 + 6 * GRACE);
        tick(ao);
        audio_out_avfoundation.reset(ao);
        audio_out_avfoundation.start(ao);
        for (int n = 7; n <= 9; n++) {
            set_clock(t0 + n * GRACE);
            tick(ao);
        }
        CHECK(fallbacks == 1 && reloads == 0,
              "ticks, pause, reset and start request nothing more (%d fallbacks, %d reloads)", fallbacks, reloads);
        CHECK(reads == reads_then && anchors == anchors_then, "nor read any more audio or anchor the clock");
        CHECK(logged(MSGL_ERR, "did not start") == 1, "nor give up again");

        // uninit() after the fallback stops everything that could call back.
        dispatch_source_t timer = p->avp_timer;
        dispatch_retain(timer);
        AVPLoader *loader = [p->loader retain];
        AVAssetResourceLoader *resource_loader = [p->asset.resourceLoader retain];
        int severe = logged(MSGL_WARN, NULL);
        audio_out_avfoundation.uninit(ao);
        CHECK(dispatch_source_testcancel(timer), "uninit cancels the feed timer");
        CHECK(![loader resourceLoader:resource_loader shouldWaitForLoadingOfRequestedResource:no_request],
              "a loader call after uninit is refused");
        CHECK(fallbacks == 1 && reloads == 0 && streaming_stops == 0 && logged(MSGL_WARN, NULL) == severe,
              "uninit is quiet and requests nothing");
        dispatch_release(timer);
        [loader release];
        [resource_loader release];
        free_ao(ao);

        // The fallback belonged to that track: a fresh output for the same
        // format opens, streams and gets its own startup supervision.
        int64_t t1 = t0 + 10 * GRACE;
        set_clock(t1);
        struct ao *again = open_ao(AF_FORMAT_S_EAC3);
        CHECK(again, "a fresh E-AC-3 output opens after the fallback");
        if (again) {
            audio_out_avfoundation.start(again);
            s = prime(again);
            CHECK(s.es_pts >= AVP_START_LEAD_NS(&s) && s.avp_rate_applied && !s.spdif_reload_requested,
                  "and primes its startup lead (%.3f s)", s.es_pts / 1e9);
            // AVPlayer's first loader call, as it comes once the main queue
            // runs: a failure after it does not blame the host.
            struct priv *q = again->priv;
            dispatch_sync(q->queue, ^{
                [q->loader resourceLoader:q->asset.resourceLoader shouldWaitForLoadingOfRequestedResource:no_request];
            });
            int unasked = logged(MSGL_WARN, UNASKED) + logged(MSGL_ERR, UNASKED);
            set_clock(t1 + GRACE);
            tick(again);
            set_clock(t1 + 2 * GRACE);
            s = tick(again);
            CHECK(s.avp_loader_asked && fallbacks == 2 && reloads == 0,
                  "its own failure requests its own fallback (%d)", fallbacks);
            CHECK(logged(MSGL_WARN, UNASKED) + logged(MSGL_ERR, UNASKED) == unasked,
                  "without the no-resource-requests line once the loader was asked");
            audio_out_avfoundation.uninit(again);
            free_ao(again);
        }

        // So does AC-3.
        struct ao *ac3 = open_ao(AF_FORMAT_S_AC3);
        CHECK(ac3, "a fresh AC-3 output opens after the fallback");
        if (ac3) {
            audio_out_avfoundation.start(ac3);
            s = prime(ac3);
            CHECK(s.es_pts >= AVP_START_LEAD_NS(&s) && s.avp_rate_applied && !s.spdif_reload_requested,
                  "and primes its startup lead (%.3f s)", s.es_pts / 1e9);
            audio_out_avfoundation.uninit(ac3);
            free_ao(ac3);
        }
        CHECK(fallbacks == 2 && reloads == 0, "opening outputs requests nothing");
        CHECK(logged(MSGL_FATAL, NULL) == 0, "no output was refused");
    }
    return NULL;
}

int main(void)
{
    // The main thread only waits for the test: it never runs the main run
    // loop or services the main dispatch queue, like a libmpv host that does
    // neither.
    pthread_t thread;
    pthread_create(&thread, NULL, run, NULL);
    pthread_join(thread, NULL);
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
