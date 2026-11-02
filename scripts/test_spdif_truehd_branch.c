// Behavioral regression for spdifenc's TrueHD MAT packing across a
// seamless-branch timing jump (patch 0039, edde746/plezy#2925).
//
// A TrueHD unit's input_timing names its place on the IEC 61937 carrier: 64
// carrier bytes per 48 kHz sample, so 2560 bytes per 40-sample unit, and
// spdifenc pads between units to keep them there. Remuxes of
// seamless-branching Blu-rays jump input_timing forward at a branch while
// output_timing runs on; the reporter's stream logged "Unusual frame timing:
// 55960 => 57781". n8.0.1 rejects any padding of half a MAT frame or more, so
// it dropped the ~114 KB that jump asks for and put every later unit that far
// early on the carrier, which the receiver played as a dropout.
//
// This drives the real spdif muxer with synthetic units across two such jumps
// and a uint16 wrap, unpacks the MAT frames it writes, and checks that every
// unit arrives whole, in order, at the carrier offset its input_timing names.

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mem.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define UNITS 400
#define UNIT_SIZE 700 // a ~6.7 Mb/s Atmos track's unit
#define SAMPLES_PER_UNIT 40 // 48 kHz family
#define CARRIER_BYTES_PER_SAMPLE 64 // 2560 bytes per 40-sample unit
#define MAJOR_SYNC_EVERY 16
#define OUTPUT_TIMING_BASE 7000

// spdifenc's MAT layout: a 61424-byte frame every 61440 carrier bytes, each
// burst led by the 8-byte IEC 61937 preamble and closed by 8 zero bytes.
#define MAT_PKT_OFFSET 61440
#define MAT_FRAME_SIZE 61424
#define BURST_HEADER 8
#define MAT_START_LEN 20
#define MAT_MIDDLE_POS 30708
#define MAT_MIDDLE_LEN 12
#define MAT_END_POS (MAT_FRAME_SIZE - 16)

// A unit whose carrier offset falls inside the end code, the gap after it or
// the next frame's start code begins once they are past: at most 16 + 16 + 20
// bytes late. The next unit's padding takes the difference back.
#define CODE_SLACK 52

static const uint8_t mat_start_code[MAT_START_LEN] = {
    0x07, 0x9E, 0x00, 0x03, 0x84, 0x01, 0x01, 0x01, 0x80, 0x00,
    0x56, 0xA5, 0x3B, 0xF4, 0x81, 0x83, 0x49, 0x80, 0x77, 0xE0,
};

// Where input_timing jumps: the reporter's 55960 => 57781, then the other
// jump that stream logged, after which input_timing wraps past 65535.
static const struct {
    int unit;
    uint16_t delta;
} branches[] = {
    {100, 1821},
    {250, 1757},
};

static uint16_t input_timing[UNITS];
static uint8_t units[UNITS][UNIT_SIZE];

struct sink {
    uint8_t *data;
    size_t len, cap;
};

static int failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static int sink_write(void *opaque, const uint8_t *buf, int size)
{
    struct sink *s = opaque;
    if (s->len + size > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 1 << 20;
        while (cap < s->len + size)
            cap *= 2;
        uint8_t *data = realloc(s->data, cap);
        if (!data)
            return AVERROR(ENOMEM);
        s->data = data;
        s->cap = cap;
    }
    memcpy(s->data + s->len, buf, size);
    s->len += size;
    return size;
}

static void build_units(void)
{
    uint16_t t = 52000; // unit 99 lands on 55960
    for (int k = 0; k < UNITS; k++) {
        if (k) {
            uint16_t delta = SAMPLES_PER_UNIT;
            for (size_t b = 0; b < ARRAY_LEN(branches); b++)
                if (branches[b].unit == k)
                    delta = branches[b].delta;
            t += delta;
        }
        input_timing[k] = t;

        uint8_t *u = units[k];
        for (int i = 0; i < UNIT_SIZE; i++)
            u[i] = (uint8_t)(k * 7 + i * 13 + 1);
        // Check nibble and length in 16-bit words: never zero, so the first
        // byte of a unit is told apart from the padding before it.
        AV_WB16(u, 0xf000 | UNIT_SIZE / 2);
        AV_WB16(u + 2, t);
        if (k % MAJOR_SYNC_EVERY == 0) {
            AV_WB32(u + 4, 0xf8726fba); // TrueHD major sync
            u[8] = 0x00;                // 48 kHz: 40 samples per unit
            u[20] = 0x10;               // one substream
            u[29] &= ~1;                // no major sync extension
            AV_WB16(u + 32, 0);         // its directory entry, no extra word
            // Block and restart header present, the restart sync word, noise
            // type 0, then output_timing: one unit ahead of the unit, and
            // continuous across both branches, as at The Martian's 00:00:23.
            uint16_t out = (uint16_t)(OUTPUT_TIMING_BASE + SAMPLES_PER_UNIT * (k + 1));
            AV_WB32(u + 34, 0xc0000000u | (0x18f5u << 17) | out);
        } else if (AV_RB24(u + 4) == 0xf8726f) {
            u[4] ^= 1; // the fill must not read as a major sync
        }
    }
}

static int mux(struct sink *out)
{
    AVFormatContext *oc = NULL;
    int ret = avformat_alloc_output_context2(&oc, NULL, "spdif", NULL);
    if (ret < 0)
        return ret;
    AVStream *st = avformat_new_stream(oc, NULL);
    uint8_t *iobuf = av_malloc(4096);
    if (!st || !iobuf) {
        av_free(iobuf);
        avformat_free_context(oc);
        return AVERROR(ENOMEM);
    }
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id = AV_CODEC_ID_TRUEHD;
    st->codecpar->sample_rate = 48000;
    av_channel_layout_default(&st->codecpar->ch_layout, 8);
    oc->pb = avio_alloc_context(iobuf, 4096, 1, out, NULL, sink_write, NULL);
    if (!oc->pb) {
        av_free(iobuf);
        avformat_free_context(oc);
        return AVERROR(ENOMEM);
    }

    AVPacket *pkt = av_packet_alloc();
    ret = pkt ? avformat_write_header(oc, NULL) : AVERROR(ENOMEM);
    // As mpv's ad_spdif feeds it: one unit per packet, no timestamps, the
    // output flushed after each.
    for (int k = 0; ret >= 0 && k < UNITS; k++) {
        ret = av_new_packet(pkt, UNIT_SIZE);
        if (ret < 0)
            break;
        memcpy(pkt->data, units[k], UNIT_SIZE);
        pkt->pts = pkt->dts = 0;
        ret = av_write_frame(oc, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            fprintf(stderr, "unit %d: av_write_frame failed (%d)\n", k, ret);
        avio_flush(oc->pb);
    }

    av_packet_free(&pkt);
    av_freep(&oc->pb->buffer);
    avio_context_free(&oc->pb);
    avformat_free_context(oc);
    return ret;
}

static int in_code(size_t pos)
{
    size_t j = pos % MAT_PKT_OFFSET;
    return j < MAT_START_LEN || (j >= MAT_MIDDLE_POS && j < MAT_MIDDLE_POS + MAT_MIDDLE_LEN) ||
           j >= MAT_END_POS;
}

int main(void)
{
    build_units();

    struct sink out = {0};
    int ret = mux(&out);
    CHECK(ret >= 0, "muxing failed (%d)", ret);
    CHECK(out.len % MAT_PKT_OFFSET == 0, "%zu output bytes are not whole bursts", out.len);
    const size_t bursts = out.len / MAT_PKT_OFFSET;

    // The carrier timeline: each burst's MAT frame, un-swapped from the
    // 16-bit words the muxer wrote, then the inter-frame gap.
    const size_t len = bursts * MAT_PKT_OFFSET;
    uint8_t *tl = calloc(len ? len : 1, 1);
    if (!tl)
        return 1;
    for (size_t b = 0; b < bursts; b++) {
        const uint8_t *burst = out.data + b * MAT_PKT_OFFSET;
        CHECK(AV_RL16(burst) == 0xF872 && AV_RL16(burst + 2) == 0x4E1F &&
                  AV_RL16(burst + 4) == 0x16 && AV_RL16(burst + 6) == MAT_FRAME_SIZE,
              "burst %zu: not a TrueHD MAT burst preamble", b);
        for (size_t j = 0; j < MAT_FRAME_SIZE; j++)
            tl[b * MAT_PKT_OFFSET + j] = burst[BURST_HEADER + (j ^ 1)];
        CHECK(!memcmp(tl + b * MAT_PKT_OFFSET, mat_start_code, MAT_START_LEN),
              "burst %zu: MAT frame does not open with the start code", b);
    }

    // Walk the units in order: zero padding and MAT codes before a unit, its
    // bytes around the codes. Units still in the frame being filled when the
    // input ran out were never written and end the walk.
    static size_t start[UNITS];
    int emitted = 0;
    size_t pos = 0;
    for (int k = 0; k < UNITS && !failures; k++) {
        while (pos < len && (in_code(pos) || !tl[pos]))
            pos++;
        if (pos >= len)
            break;
        start[k] = pos;
        int i = 0;
        for (; i < UNIT_SIZE; i++, pos++) {
            while (pos < len && in_code(pos))
                pos++;
            if (pos >= len)
                break;
            if (tl[pos] != units[k][i]) {
                CHECK(0, "unit %d: byte %d differs at carrier offset %zu", k, i, pos);
                break;
            }
        }
        if (i < UNIT_SIZE)
            break;
        emitted = k + 1;
    }
    CHECK(emitted > branches[ARRAY_LEN(branches) - 1].unit + 2 * SAMPLES_PER_UNIT,
          "only %d of %d units were written; the walk stops short of the last branch", emitted,
          UNITS);

    // The cadence: each unit at the carrier offset its input_timing names,
    // counted from the first, which owns offset 0 and its start code.
    int64_t nominal = 0;
    for (int k = 0; k < emitted; k++) {
        if (k)
            nominal += (int64_t)(uint16_t)(input_timing[k] - input_timing[k - 1]) *
                       CARRIER_BYTES_PER_SAMPLE;
        const int64_t late = (int64_t)start[k] - nominal;
        CHECK(late >= 0 && late <= CODE_SLACK,
              "unit %d (input_timing %" PRIu16 "): carrier offset %zu, expected %" PRId64
              " (+%d), %" PRId64 " bytes off",
              k, input_timing[k], start[k], nominal, CODE_SLACK, late);
        if (failures)
            break;
    }

    free(tl);
    free(out.data);
    if (failures)
        return 1;
    printf("spdif TrueHD: %d units across %zu input_timing branches and a wrap hold their "
           "carrier offsets in %zu MAT frames\n",
           emitted, ARRAY_LEN(branches), bursts);
    return 0;
}
