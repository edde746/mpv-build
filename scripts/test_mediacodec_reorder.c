// Behavioral regressions for MediaCodec presentation-order recovery
// (libavcodec/mediacodec_reorder.c), built by test_mediacodec_reorder.sh.
//
// A model decoder stands in for MediaCodec: it takes access units in decode
// order and outputs frames in display order by the HEVC bumping process (a
// frame leaves once more frames wait for output than the reorder bound
// allows), echoing each frame's queued timestamp the way MediaCodec does.
// Streams are x265-shaped: an IDR, then mini-GOPs of four B-frames in a
// pyramid behind their anchor, with open-GOP CRA anchors whose B-frames are
// RASL pictures. A container without composition offsets queues each access
// unit at its decode position; a well-formed one queues its display position.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mediacodec_reorder.h"

#define AV_NOPTS_VALUE ((int64_t)UINT64_C(0x8000000000000000))
#define FD 40000 // frame duration, microseconds
#define DEPTH 2  // reorder bound of the streams below

enum { TRAIL_N = 0, TRAIL_R = 1, RASL_N = 8, RASL_R = 9, IDR_W_RADL = 19, CRA = 21 };

typedef struct Frame {
    int display; // display position
    int type;    // HEVC NAL unit type
    int irap;    // decode position of the IRAP it belongs to
} Frame;

static Frame stream[20000];
static int stream_len;

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        failures++; \
    } \
} while (0)

// An IDR, then `gops` mini-GOPs; every `cra_every`-th anchor is a CRA whose
// four B-frames are RASL pictures. Decode order inside a mini-GOP with anchor
// at a+5: a+5, a+3 (reference B), a+1, a+2, a+4.
static void build_stream(int gops, int cra_every)
{
    static const int order[5]  = { 5, 3, 1, 2, 4 };
    int irap = 0;

    stream_len = 0;
    stream[stream_len++] = (Frame){ 0, IDR_W_RADL, 0 };
    for (int g = 0; g < gops; g++) {
        int a = g * 5, cra = cra_every && g % cra_every == cra_every - 1;
        for (int i = 0; i < 5; i++) {
            Frame f = { a + order[i], 0, irap };
            if (i == 0) {
                f.type = cra ? CRA : TRAIL_R;
                if (cra)
                    irap = f.irap = stream_len;
            } else if (cra) {
                f.type = order[i] == 3 ? RASL_R : RASL_N;
            } else {
                f.type = order[i] == 3 ? TRAIL_R : TRAIL_N;
            }
            stream[stream_len++] = f;
        }
    }
}

// Annex B access unit holding one NAL unit header of `type`.
static void access_unit(uint8_t au[5], int type)
{
    au[0] = 0; au[1] = 0; au[2] = 1;
    au[3] = (uint8_t)(type << 1);
    au[4] = 1; // nuh_temporal_id_plus1
}

typedef struct Sim {
    MediaCodecReorder r;
    int broken_mux;        // queue decode positions instead of display positions
    int drop_display;      // a frame the decoder decodes but never outputs, -1 none
    int output_rasl;       // a decoder that outputs RASL pictures it should skip
    int with_nal;          // hand the module the access unit bytes

    // decoder state
    int waiting[64], nwaiting;
    int skip_rasl_of;      // IRAP whose RASL pictures are skipped, -1 none
    int started;

    // what came out
    int out_display[20000];
    int64_t out_echo[20000], out_pts[20000];
    int nout;
} Sim;

static int64_t queued_pts(const Sim *s, int decode_pos)
{
    return s->broken_mux ? (int64_t)decode_pos * FD
                         : (int64_t)(stream[decode_pos].display + DEPTH) * FD;
}

static void sim_init(Sim *s, int broken_mux, int hevc)
{
    memset(s, 0, sizeof(*s));
    ff_mediacodec_reorder_init(&s->r, DEPTH, hevc, 1);
    s->broken_mux = broken_mux;
    s->drop_display = -1;
    s->skip_rasl_of = -1;
    s->with_nal = hevc;
}

static void emit(Sim *s, int decode_pos)
{
    int64_t echo = queued_pts(s, decode_pos);
    s->out_display[s->nout] = stream[decode_pos].display;
    s->out_echo[s->nout] = echo;
    s->out_pts[s->nout] = ff_mediacodec_reorder_output(&s->r, echo);
    s->nout++;
}

// Output the waiting frame shown first.
static void bump(Sim *s)
{
    int best = 0;
    for (int i = 1; i < s->nwaiting; i++)
        if (stream[s->waiting[i]].display < stream[s->waiting[best]].display)
            best = i;
    emit(s, s->waiting[best]);
    s->waiting[best] = s->waiting[--s->nwaiting];
}

static void feed(Sim *s, int decode_pos)
{
    const Frame *f = &stream[decode_pos];
    uint8_t au[5];

    access_unit(au, f->type);
    ff_mediacodec_reorder_input(&s->r, queued_pts(s, decode_pos),
                                s->with_nal ? au : NULL, sizeof(au));

    if (!s->started) {
        s->started = 1;
        s->skip_rasl_of = f->type == CRA ? decode_pos : -1;
    } else if (f->type == CRA || f->type == IDR_W_RADL) {
        s->skip_rasl_of = -1;
    }
    if ((f->type == RASL_N || f->type == RASL_R) && f->irap == s->skip_rasl_of &&
        !s->output_rasl)
        return;
    if (f->display == s->drop_display)
        return;
    s->waiting[s->nwaiting++] = decode_pos;
    while (s->nwaiting > DEPTH)
        bump(s);
}

static void drain(Sim *s)
{
    while (s->nwaiting)
        bump(s);
}

static void flush(Sim *s)
{
    s->nwaiting = 0;
    s->started = 0;
    s->skip_rasl_of = -1;
    ff_mediacodec_reorder_flush(&s->r);
}

// Frames given a recovered time must come out strictly increasing.
static int check_increasing(const Sim *s, int from, const char *what)
{
    int bad = 0;
    for (int i = from + 1; i < s->nout; i++) {
        if (s->out_pts[i] <= s->out_pts[i - 1]) {
            if (!bad)
                fprintf(stderr, "  %s: output %d at %" PRId64 " after %" PRId64 "\n",
                        what, i, s->out_pts[i], s->out_pts[i - 1]);
            bad++;
        }
    }
    return bad;
}

// Outputs from `from` on that are not at their display time.
static int count_off_time(const Sim *s, int from)
{
    int off = 0;
    for (int i = from; i < s->nout; i++)
        off += s->out_pts[i] != (int64_t)s->out_display[i] * FD;
    return off;
}

// A well-formed B-frame stream: the first backwards timestamp settles the
// module off, and every frame keeps the timestamp the codec echoed.
static void test_well_formed_stream_passes_through(void)
{
    Sim s;
    int changed = 0;

    build_stream(200, 10);
    sim_init(&s, 0, 1);
    for (int i = 0; i < stream_len; i++)
        feed(&s, i);
    drain(&s);
    for (int i = 0; i < s.nout; i++)
        changed += s.out_pts[i] != s.out_echo[i];
    CHECK(s.r.mode == FF_MEDIACODEC_REORDER_OFF, "mode %d, expected off", s.r.mode);
    CHECK(changed == 0, "%d frames lost the codec's timestamp", changed);
}

// Decode-order timestamps: the output is recovered from the first frame the
// codec reorders and every frame after that is at its display time.
static void test_decode_order_timestamps_are_recovered(void)
{
    Sim s;
    int on_at = -1;

    build_stream(400, 10);
    sim_init(&s, 1, 1);
    for (int i = 0; i < stream_len; i++) {
        feed(&s, i);
        if (on_at < 0 && s.r.mode == FF_MEDIACODEC_REORDER_ON)
            on_at = s.nout - 1;
    }
    drain(&s);
    CHECK(s.r.mode == FF_MEDIACODEC_REORDER_ON, "mode %d, expected on", s.r.mode);
    CHECK(on_at >= 0 && on_at <= DEPTH + 2, "recovery started at output %d", on_at);
    CHECK(count_off_time(&s, on_at) == 0, "%d recovered frames off their display time",
          count_off_time(&s, on_at));
    CHECK(check_increasing(&s, on_at, "continuous") == 0, "timestamps went backwards");
    CHECK(s.nout == stream_len, "%d frames out of %d", s.nout, stream_len);
}

static int first_cra_after(int decode_pos)
{
    for (int i = decode_pos; i < stream_len; i++)
        if (stream[i].type == CRA)
            return i;
    return -1;
}

// A seek lands on a CRA: its RASL pictures are never output. The CRA keeps
// its own queued time; every frame after it is at the time continuous playback
// gives it, so a seek costs no lip sync. `depth` is the reorder bound the
// stream states, 0 for none (the module then assumes the largest DPB and the
// reorder bound can no longer write the RASL pictures off early).
static void test_seek_to_open_gop_cra(int hevc_aware, int depth)
{
    Sim s;
    int cra, from;

    build_stream(400, 10);
    sim_init(&s, 1, 1);
    ff_mediacodec_reorder_init(&s.r, depth, 1, 1);
    s.with_nal = hevc_aware;
    for (int i = 0; i < 300; i++)
        feed(&s, i);
    CHECK(s.r.mode == FF_MEDIACODEC_REORDER_ON, "mode %d before the seek", s.r.mode);

    for (int seek = 0; seek < 5; seek++) {
        cra = first_cra_after(500 + seek * 300);
        flush(&s);
        from = s.nout;
        for (int i = cra; i < cra + 250; i++)
            feed(&s, i);
        drain(&s);
        CHECK(s.out_display[from] == stream[cra].display,
              "first frame after the seek is display %d, not the CRA", s.out_display[from]);
        CHECK(s.out_pts[from] == (int64_t)cra * FD, "CRA at %" PRId64 ", queued at %d",
              s.out_pts[from], cra * FD);
        CHECK(check_increasing(&s, from, "after seek") == 0, "timestamps went backwards");
        if (hevc_aware) {
            CHECK(count_off_time(&s, from + 1) == 0,
                  "seek %d: %d frames after the CRA off their display time",
                  seek, count_off_time(&s, from + 1));
        } else {
            // Without the access units the RASL pictures are written off by
            // the reorder bound, within a mini-GOP of the CRA.
            CHECK(count_off_time(&s, from + 1 + 5) == 0,
                  "seek %d: %d frames off their display time a mini-GOP after the CRA",
                  seek, count_off_time(&s, from + 1 + 5));
        }
    }
    CHECK(s.r.mode == FF_MEDIACODEC_REORDER_ON, "mode %d after the seeks", s.r.mode);
}

static void test_seek_to_open_gop_cra_hevc(void)
{
    test_seek_to_open_gop_cra(1, DEPTH);
}

static void test_seek_to_open_gop_cra_hevc_without_stated_bound(void)
{
    test_seek_to_open_gop_cra(1, 0);
}

static void test_seek_to_open_gop_cra_without_access_units(void)
{
    test_seek_to_open_gop_cra(0, DEPTH);
}

// A decoder that outputs the RASL pictures it should skip: the pictures taken
// out when queued are counted back in, and the timeline stays exact.
static void test_rasl_output_after_all(void)
{
    Sim s;
    int cra, from;

    build_stream(400, 10);
    sim_init(&s, 1, 1);
    s.output_rasl = 1;
    for (int i = 0; i < 300; i++)
        feed(&s, i);
    cra = first_cra_after(600);
    flush(&s);
    from = s.nout;
    for (int i = cra; i < cra + 250; i++)
        feed(&s, i);
    drain(&s);
    CHECK(s.r.readmitted > 0, "no RASL picture was counted back in");
    CHECK(check_increasing(&s, from, "rasl output") == 0, "timestamps went backwards");
    // The CRA and its RASL pictures share the queued times from the CRA's on;
    // everything after them is exact.
    CHECK(count_off_time(&s, from + 5) == 0, "%d frames off their display time",
          count_off_time(&s, from + 5));
}

// A frame the decoder drops mid-stream is written off within the reorder
// bound, so the timeline does not stay shifted.
static void test_frame_dropped_by_decoder(void)
{
    Sim s;
    int drop_at = -1, late = 0;

    build_stream(400, 0);
    sim_init(&s, 1, 1);
    s.drop_display = 1002; // a non-reference B-frame
    for (int i = 0; i < stream_len; i++)
        feed(&s, i);
    drain(&s);
    for (int i = 0; i < s.nout; i++)
        if (drop_at < 0 && s.out_display[i] > s.drop_display)
            drop_at = i;
    for (int i = drop_at; i < s.nout; i++)
        if (s.out_pts[i] != (int64_t)s.out_display[i] * FD)
            late = i - drop_at + 1;
    CHECK(s.r.dropped == 1, "%" PRIu64 " frames written off, expected 1", s.r.dropped);
    CHECK(check_increasing(&s, DEPTH + 2, "drop") == 0, "timestamps went backwards");
    CHECK(late <= DEPTH + 1, "timeline shifted for %d frames after the drop", late);
    CHECK(s.r.mode == FF_MEDIACODEC_REORDER_ON, "mode %d", s.r.mode);
}

// Hours of playback with seeks: the ring never fills and the module stays on.
static void test_long_session_stays_bounded(void)
{
    Sim s;

    build_stream(3900, 7);
    sim_init(&s, 1, 1);
    for (int i = 0; i < stream_len; i++) {
        if (i && i % 3001 == 0) {
            int cra = first_cra_after(i);
            if (cra < 0)
                break;
            flush(&s);
            i = cra;
        }
        feed(&s, i);
        CHECK(s.r.count <= DEPTH + 8, "ring holds %d slots at %d", s.r.count, i);
        if (failures)
            return;
    }
    drain(&s);
    CHECK(s.r.mode == FF_MEDIACODEC_REORDER_ON, "mode %d", s.r.mode);
}

// Timestamps stepping back after recovery started (a discontinuity) settle
// the module off; frames keep the codec's timestamps from then on.
static void test_discontinuity_turns_recovery_off(void)
{
    Sim s;
    int from;

    build_stream(400, 10);
    sim_init(&s, 1, 1);
    for (int i = 0; i < 300; i++)
        feed(&s, i);
    CHECK(s.r.mode == FF_MEDIACODEC_REORDER_ON, "mode %d", s.r.mode);
    from = s.nout;
    ff_mediacodec_reorder_input(&s.r, 5 * FD, NULL, 0);
    CHECK(s.r.mode == FF_MEDIACODEC_REORDER_OFF, "mode %d after a step back", s.r.mode);
    for (int i = 300; i < 400; i++)
        feed(&s, i);
    drain(&s);
    for (int i = from; i < s.nout; i++)
        CHECK(s.out_pts[i] == s.out_echo[i], "output %d changed after recovery stopped", i);
}

static void test_missing_timestamp_turns_recovery_off(void)
{
    MediaCodecReorder r;

    ff_mediacodec_reorder_init(&r, DEPTH, 0, 1);
    ff_mediacodec_reorder_input(&r, 0, NULL, 0);
    ff_mediacodec_reorder_input(&r, AV_NOPTS_VALUE, NULL, 0);
    CHECK(r.mode == FF_MEDIACODEC_REORDER_OFF, "mode %d", r.mode);
    CHECK(ff_mediacodec_reorder_output(&r, 7) == 7, "timestamp changed while off");
}

// A stream the decoder never reorders (P-frames only) that also loses a frame:
// the module never switches on and every frame keeps its timestamp.
static void test_in_order_stream_is_untouched(void)
{
    MediaCodecReorder r;
    int changed = 0;

    ff_mediacodec_reorder_init(&r, 0, 0, 1);
    for (int i = 0; i < 5000; i++) {
        ff_mediacodec_reorder_input(&r, (int64_t)i * FD, NULL, 0);
        if (i % 97 == 50)
            continue; // decoded, never output
        changed += ff_mediacodec_reorder_output(&r, (int64_t)i * FD) != (int64_t)i * FD;
        CHECK(r.count <= 20, "ring holds %d slots", r.count);
        if (failures)
            return;
    }
    CHECK(r.mode == FF_MEDIACODEC_REORDER_ARMED, "mode %d", r.mode);
    CHECK(changed == 0, "%d frames changed", changed);
}

static void test_disabled_passes_through(void)
{
    MediaCodecReorder r;

    ff_mediacodec_reorder_init(&r, DEPTH, 0, 0);
    ff_mediacodec_reorder_input(&r, 100, NULL, 0);
    ff_mediacodec_reorder_input(&r, 200, NULL, 0);
    CHECK(ff_mediacodec_reorder_output(&r, 200) == 200, "changed while disabled");
    CHECK(ff_mediacodec_reorder_output(&r, 100) == 100, "changed while disabled");
    CHECK(r.mode == FF_MEDIACODEC_REORDER_OFF, "mode %d", r.mode);
}

int main(void)
{
    test_well_formed_stream_passes_through();
    test_decode_order_timestamps_are_recovered();
    test_seek_to_open_gop_cra_hevc();
    test_seek_to_open_gop_cra_hevc_without_stated_bound();
    test_seek_to_open_gop_cra_without_access_units();
    test_rasl_output_after_all();
    test_frame_dropped_by_decoder();
    test_long_session_stays_bounded();
    test_discontinuity_turns_recovery_off();
    test_missing_timestamp_turns_recovery_off();
    test_in_order_stream_is_untouched();
    test_disabled_passes_through();

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("mediacodec reorder: all checks passed\n");
    return 0;
}
