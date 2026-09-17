// Behavioral regressions for mpv's lavc_process under the busy policy a
// MediaCodec decoder runs with (patch 0111), extracted by
// test_lavc_busy_policy.sh. The decoder is a pair of scripted callbacks and
// the filter graph a recording fake. The case that matters: a polled decoder
// answering EAGAIN after the EOF packet was accepted. Nothing upstream will
// ever feed it again, so unless the policy re-polls, the end of stream it
// still owes (its EOS submission waiting on a codec input slot, or the codec
// on its last frames) never arrives and the file never ends. That is what a
// hardware session on API <= 30 did from the first vd-queue build on.
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AVERROR(e) (-(e))
#define AVERROR_EOF (-0x20464F45)

enum mp_frame_type { MP_FRAME_NONE = 0, MP_FRAME_EOF, MP_FRAME_PACKET, MP_FRAME_VIDEO };
struct mp_frame { enum mp_frame_type type; void *data; };
#define MP_EOF_FRAME ((struct mp_frame){ .type = MP_FRAME_EOF })

struct demux_packet { int id; };
struct mp_pin { struct mp_frame pending; bool needs_data; };
struct mp_filter { struct mp_pin **ppins; void *packet_pool; };

// --- graph fake: the input pin hands out what the test lined up, the output
// pin records what was written, and the filter records what it was asked for.

static struct {
    struct mp_pin in, out;
    struct mp_pin *pins[2];
    struct mp_filter f;
    int wakeups, progress, failed, warnings, errors;
    struct mp_frame written[8];
    int nwritten;
    int unread;
} graph;

static bool mp_pin_in_needs_data(struct mp_pin *p) { return p->needs_data; }

static void mp_pin_in_write(struct mp_pin *p, struct mp_frame frame)
{
    if (graph.nwritten == 8) { fprintf(stderr, "FAIL: too many writes\n"); exit(1); }
    graph.written[graph.nwritten++] = frame;
}

static struct mp_frame mp_pin_out_read(struct mp_pin *p)
{
    struct mp_frame f = p->pending;
    p->pending = (struct mp_frame){0};
    return f;
}

static void mp_pin_out_unread(struct mp_pin *p, struct mp_frame frame)
{
    p->pending = frame;
    graph.unread++;
}

static void mp_frame_unref(struct mp_frame *f) { *f = (struct mp_frame){0}; }
static void mp_filter_wakeup(struct mp_filter *f) { graph.wakeups++; }
static void mp_filter_internal_mark_progress(struct mp_filter *f) { graph.progress++; }
static void mp_filter_internal_mark_failed(struct mp_filter *f) { graph.failed++; }
static void demux_packet_pool_push(void *pool, struct demux_packet *pkt) {}
#define MP_ERR(f, ...) (graph.errors++)
#define MP_WARN(f, ...) (graph.warnings++)

#include "lavc_busy_policy.inc"

// --- decoder fake: what the next receive answers, and what a send answers.

static struct {
    int recv_ret;              // receive's return when it yields no frame
    bool recv_frame;           // receive yields a video frame instead
    int send_ret;
    int sends, null_sends, receives;
} dec;

static int fake_send(struct mp_filter *f, struct demux_packet *pkt)
{
    dec.sends++;
    if (!pkt)
        dec.null_sends++;
    return dec.send_ret;
}

static int fake_receive(struct mp_filter *f, struct mp_frame *res)
{
    dec.receives++;
    if (dec.recv_frame) {
        *res = (struct mp_frame){ .type = MP_FRAME_VIDEO };
        return 0;
    }
    return dec.recv_ret;
}

// --- harness

static void check(bool ok, const char *scenario)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", scenario);
        exit(1);
    }
}

static struct lavc_state state;
static struct demux_packet packet = { 1 };

static void reset(enum lavc_busy_policy policy)
{
    memset(&graph, 0, sizeof(graph));
    graph.pins[0] = &graph.in;
    graph.pins[1] = &graph.out;
    graph.f.ppins = graph.pins;
    graph.out.needs_data = true;
    memset(&dec, 0, sizeof(dec));
    state = (struct lavc_state){ .busy_policy = policy };
}

static void run(void)
{
    lavc_process(&graph.f, &state, fake_send, fake_receive);
}

static void feed(struct mp_frame frame)
{
    graph.in.pending = frame;
}

static struct mp_frame packet_frame(void)
{
    return (struct mp_frame){ .type = MP_FRAME_PACKET, .data = &packet };
}

static int eofs_written(void)
{
    int n = 0;
    for (int i = 0; i < graph.nwritten; i++)
        n += graph.written[i].type == MP_FRAME_EOF;
    return n;
}

// The path to the end of the stream every case shares: one packet accepted,
// then the demuxer's single EOF accepted.
static void reach_eof(void)
{
    dec.recv_ret = AVERROR(EAGAIN);
    feed(packet_frame());
    run();
    check(dec.sends == 1 && graph.progress == 1, "a packet is fed and counts as progress");
    feed(MP_EOF_FRAME);
    run();
    check(dec.null_sends == 1 && graph.progress == 2 && !graph.wakeups,
          "the EOF packet is fed and counts as progress");
}

// --- cases

static void test_polled_decoder_is_asked_again_past_eof(void)
{
    reset(LAVC_BUSY_POLL);
    reach_eof();

    // Nothing left to feed; the decoder still answers EAGAIN.
    run();
    check(graph.wakeups == 1 && dec.receives == 3 && !graph.nwritten,
          "a polled decoder that owes the end of stream is asked again");
    run();
    check(graph.wakeups == 2, "and again, for as long as it keeps stalling");

    // Its last frames come out, then its EOF.
    dec.recv_frame = true;
    run();
    check(graph.nwritten == 1 && graph.written[0].type == MP_FRAME_VIDEO && graph.wakeups == 2,
          "a frame it finally yields goes downstream without a re-poll");
    dec.recv_frame = false;
    dec.recv_ret = AVERROR_EOF;
    run();
    check(eofs_written() == 1, "its EOF goes downstream once");
    run();
    check(eofs_written() == 1, "a repeated EOF is not written again");

    // Past EOF the decoder is idle again: nothing to ask for.
    dec.recv_ret = AVERROR(EAGAIN);
    run();
    check(graph.wakeups == 2, "once the stream has ended there is nothing to poll for");
}

static void test_polled_decoder_is_not_spun_while_the_demuxer_starves(void)
{
    reset(LAVC_BUSY_POLL);
    dec.recv_ret = AVERROR(EAGAIN);
    feed(packet_frame());
    run();
    // No packet ready and no EOF: the demuxer will wake the filter itself.
    run();
    run();
    check(!graph.wakeups && dec.receives == 3,
          "a polled decoder waiting for the demuxer is left to the demuxer");
}

static void test_polled_decoder_refusing_a_packet_is_asked_again(void)
{
    reset(LAVC_BUSY_POLL);
    dec.recv_ret = AVERROR(EAGAIN);
    dec.send_ret = AVERROR(EAGAIN);
    feed(packet_frame());
    run();
    check(graph.wakeups == 1 && graph.unread == 1 && !graph.warnings && !graph.progress,
          "a refused packet is put back and the decoder asked again");
}

static void test_woken_decoder_is_left_alone_past_eof(void)
{
    reset(LAVC_BUSY_WAKEUP);
    reach_eof();
    run();
    run();
    check(!graph.wakeups && !graph.warnings, "a decoder that wakes the filter itself is not polled");
}

static void test_ordinary_decoder_keeps_libavcodec_contract_handling(void)
{
    // A decoder libavcodec's contract binds never answers EAGAIN past EOF;
    // the default policy does not start polling it.
    reset(LAVC_BUSY_UNEXPECTED);
    reach_eof();
    run();
    check(!graph.wakeups, "an ordinary decoder past EOF is not polled");
}

int main(void)
{
    test_polled_decoder_is_asked_again_past_eof();
    test_polled_decoder_is_not_spun_while_the_demuxer_starves();
    test_polled_decoder_refusing_a_packet_is_asked_again();
    test_woken_decoder_is_left_alone_past_eof();
    test_ordinary_decoder_keeps_libavcodec_contract_handling();
    puts("PASS: a polled decoder that still owes the end of stream is asked "
         "again until it delivers it, and only then");
    return 0;
}
