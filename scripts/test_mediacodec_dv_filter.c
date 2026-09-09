// Behavioral regressions for the exact freestanding Dolby Vision access-unit
// filter extracted from ffmpeg patch 0001 by test_mediacodec_dv_filter.sh.
// libavutil's buffer/packet API and libdovi are stubbed with counting fakes;
// the access units are hand-built Annex B HEVC.
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_LIBDOVI 1
#define AV_INPUT_BUFFER_PADDING_SIZE 64
#define AVERROR(e) (-(e))
#define AV_LOG_WARNING 24
#define FFMAX(a, b) ((a) > (b) ? (a) : (b))
#define FFMIN(a, b) ((a) > (b) ? (b) : (a))

#define DV_FILTER_NONE    0
#define DV_FILTER_CONVERT 1
#define DV_FILTER_STRIP   2

typedef struct AVBufferRef {
    uint8_t *data;
    size_t size;
    int refcount;
} AVBufferRef;

typedef struct AVPacket {
    AVBufferRef *buf;
    uint8_t *data;
    int size;
} AVPacket;

typedef struct AVCodecContext {
    void *priv_data;
} AVCodecContext;

typedef struct MediaCodecH264DecContext {
    int dv_filter;
    int dv_filter_warned;
    int dv_strip_hdr10plus;
    AVBufferRef *dv_out;
    uint8_t *dv_sei_scratch;
    unsigned dv_sei_scratch_size;
} MediaCodecH264DecContext;

static int buffer_allocs, scratch_allocs, warnings, live_buffers;

static void av_log(void *avctx, int level, const char *fmt, ...)
{
    warnings++;
}

static int av_buffer_is_writable(const AVBufferRef *buf)
{
    return buf->refcount == 1;
}

static int av_buffer_realloc(AVBufferRef **buf, size_t size)
{
    if (!*buf) {
        *buf = calloc(1, sizeof(**buf));
        if (!*buf)
            return AVERROR(ENOMEM);
        (*buf)->refcount = 1;
        live_buffers++;
    } else if (!av_buffer_is_writable(*buf)) {
        fprintf(stderr, "FAIL: av_buffer_realloc on a shared buffer\n");
        exit(1);
    }
    uint8_t *data = realloc((*buf)->data, size);
    if (!data)
        return AVERROR(ENOMEM);
    (*buf)->data = data;
    (*buf)->size = size;
    buffer_allocs++;
    return 0;
}

static void av_buffer_unref(AVBufferRef **buf)
{
    if (!*buf)
        return;
    if (--(*buf)->refcount == 0) {
        free((*buf)->data);
        free(*buf);
        live_buffers--;
    }
    *buf = NULL;
}

static AVBufferRef *av_buffer_ref(AVBufferRef *buf)
{
    buf->refcount++;
    return buf;
}

static void av_packet_unref(AVPacket *pkt)
{
    av_buffer_unref(&pkt->buf);
    pkt->data = NULL;
    pkt->size = 0;
}

static void av_fast_malloc(void *ptr, unsigned int *size, size_t min_size)
{
    void **p = ptr;
    if (*p && *size >= min_size)
        return;
    free(*p);
    *p = malloc(min_size);
    *size = *p ? min_size : 0;
    scratch_allocs++;
}

// libdovi: a conversion succeeds unless the RPU payload starts with the
// poison byte, and writes a recognizable replacement NAL.
typedef struct DoviRpuOpaque { int poisoned; } DoviRpuOpaque;
typedef struct DoviData { const uint8_t *data; size_t len; } DoviData;
static const uint8_t converted_rpu[] = { 0x7C, 0x01, 0xC0, 0x4E, 0x56 };
#define RPU_POISON 0xEE

static DoviRpuOpaque *dovi_parse_unspec62_nalu(const uint8_t *buf, size_t len)
{
    DoviRpuOpaque *rpu = calloc(1, sizeof(*rpu));
    rpu->poisoned = len >= 3 && buf[2] == RPU_POISON;
    return rpu;
}

static const char *dovi_rpu_get_error(const DoviRpuOpaque *rpu)
{
    return rpu->poisoned ? "poisoned" : NULL;
}

static int dovi_convert_rpu_with_mode(DoviRpuOpaque *rpu, uint8_t mode)
{
    return mode == 2 ? 0 : -1;
}

static const DoviData *dovi_write_unspec62_nalu(const DoviRpuOpaque *rpu)
{
    DoviData *out = calloc(1, sizeof(*out));
    out->data = converted_rpu;
    out->len = sizeof(converted_rpu);
    return out;
}

static void dovi_data_free(const DoviData *data)
{
    free((void *)data);
}

static void dovi_rpu_free(DoviRpuOpaque *rpu)
{
    free(rpu);
}

#include "mediacodec_dv_filter.inc"

// --- access-unit builder

struct au {
    uint8_t data[1 << 17];
    size_t size;
};

static void put(struct au *au, const void *bytes, size_t n)
{
    memcpy(au->data + au->size, bytes, n);
    au->size += n;
}

#define PUT(au, ...) do { \
    const uint8_t bytes_[] = { __VA_ARGS__ }; \
    put((au), bytes_, sizeof(bytes_)); \
} while (0)

enum { PREFIX_SEI = 39, SUFFIX_SEI = 40, RPU = 62, EL = 63, VPS = 32, IDR = 19 };

// Start code (3 or 4 bytes), HEVC NAL header for type, then the payload.
static void nal(struct au *au, int sc4, int type, const void *payload, size_t n)
{
    if (sc4)
        PUT(au, 0);
    PUT(au, 0, 0, 1, (uint8_t)(type << 1), 1);
    put(au, payload, n);
}

#define NAL(au, sc4, type, ...) do { \
    const uint8_t payload_[] = { __VA_ARGS__ }; \
    nal((au), (sc4), (type), payload_, sizeof(payload_)); \
} while (0)

static void slice(struct au *au, int type, size_t n)
{
    uint8_t body[1 << 16];
    for (size_t i = 0; i < n; i++)
        body[i] = 0x10 + (uint8_t)(i * 7 % 200); // never a start code or EPB
    nal(au, 1, type, body, n);
}

#define PIC_TIMING 0x01, 0x03, 0x12, 0x34, 0x56
#define HDR10PLUS  0x04, 0x0A, 0xB5, 0x00, 0x3C, 0x00, 0x01, 0x04, 0x00, 0x11, 0x22, 0x33
#define HASH       0x84, 0x03, 0xAA, 0xBB, 0xCC

// --- harness

static AVCodecContext avctx;
static MediaCodecH264DecContext ctx;
static AVPacket pkt;

static void reset(int filter)
{
    av_packet_unref(&pkt);
    av_buffer_unref(&ctx.dv_out);
    free(ctx.dv_sei_scratch);
    memset(&ctx, 0, sizeof(ctx));
    ctx.dv_filter = filter;
    ctx.dv_strip_hdr10plus = 1;
    avctx.priv_data = &ctx;
    buffer_allocs = scratch_allocs = warnings = 0;
}

// A fresh refcounted packet holding a copy of the access unit.
static void load(const struct au *au)
{
    av_packet_unref(&pkt);
    av_buffer_realloc(&pkt.buf, au->size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(pkt.buf->data, au->data, au->size);
    memset(pkt.buf->data + au->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    pkt.data = pkt.buf->data;
    pkt.size = (int)au->size;
    buffer_allocs = 0;
}

static void filter(const char *scenario)
{
    int ret = dv_filter_packet(&avctx, &pkt);
    if (ret < 0) {
        fprintf(stderr, "FAIL: %s: filter returned %d\n", scenario, ret);
        exit(1);
    }
}

static void expect_bytes(const struct au *expected, const char *scenario)
{
    if ((size_t)pkt.size != expected->size ||
        memcmp(pkt.data, expected->data, expected->size)) {
        fprintf(stderr, "FAIL: %s: output %d bytes, expected %zu\n", scenario,
                pkt.size, expected->size);
        size_t n = FFMIN((size_t)pkt.size, expected->size);
        for (size_t i = 0; i < n; i++) {
            if (pkt.data[i] != expected->data[i]) {
                fprintf(stderr, "  first difference at %zu: %02x vs %02x\n",
                        i, pkt.data[i], expected->data[i]);
                break;
            }
        }
        exit(1);
    }
    for (int i = 0; i < AV_INPUT_BUFFER_PADDING_SIZE; i++) {
        if (pkt.data[pkt.size + i]) {
            fprintf(stderr, "FAIL: %s: padding not zeroed\n", scenario);
            exit(1);
        }
    }
}

static void expect_untouched(const struct au *au, const char *scenario)
{
    if (pkt.data != pkt.buf->data || buffer_allocs) {
        fprintf(stderr, "FAIL: %s: unchanged access unit was rebuilt "
                "(%d allocations)\n", scenario, buffer_allocs);
        exit(1);
    }
    expect_bytes(au, scenario);
}

static void check(bool ok, const char *scenario)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", scenario);
        exit(1);
    }
}

// --- cases

static void test_unchanged_access_unit_is_not_copied(void)
{
    // A profile 8 access unit with the usual per-frame SEI, RPU and no HDR10+.
    struct au au = {0};
    NAL(&au, 1, VPS, 0x0C, 0x01, 0xFF, 0xFF);
    NAL(&au, 1, PREFIX_SEI, PIC_TIMING, 0x80);
    slice(&au, IDR, 60000);
    NAL(&au, 0, SUFFIX_SEI, HASH, 0x80);
    NAL(&au, 0, RPU, 0x01, 0x02, 0x03);

    reset(DV_FILTER_NONE);
    for (int i = 0; i < 3; i++) {
        load(&au);
        scratch_allocs = 0;
        filter("unchanged access unit");
        expect_untouched(&au, "unchanged access unit");
        check(scratch_allocs == (i == 0), "SEI scratch is allocated once");
        check(!ctx.dv_out, "unchanged access unit never materializes");
    }
    // The empty packet used to drain is a no-op too.
    load(&au);
    pkt.size = 0;
    filter("empty packet");
    check(pkt.data == pkt.buf->data && pkt.size == 0 && !buffer_allocs,
          "empty packet is untouched");
}

static void test_hdr10plus_only_sei_is_removed(void)
{
    struct au au = {0}, expected = {0};
    NAL(&au, 1, VPS, 0x0C, 0x01);
    NAL(&expected, 1, VPS, 0x0C, 0x01);
    NAL(&au, 0, PREFIX_SEI, HDR10PLUS, 0x80); // 3-byte start code goes with it
    slice(&au, IDR, 300);
    slice(&expected, IDR, 300);
    // Of the two trailing zero bytes, one is the next NAL's zero_byte and
    // stays; the other goes with the removed NAL.
    NAL(&au, 1, SUFFIX_SEI, HDR10PLUS, 0x80, 0x00, 0x00);
    NAL(&au, 0, RPU, 0x01, 0x02);
    NAL(&expected, 1, RPU, 0x01, 0x02);

    reset(DV_FILTER_NONE);
    load(&au);
    filter("HDR10+ only SEI");
    expect_bytes(&expected, "HDR10+ only SEI removed with its start code");
    check(pkt.data != pkt.buf->data || pkt.buf == ctx.dv_out,
          "rewritten packet lives in the reused output buffer");
}

static void test_bundled_sei_keeps_other_messages(void)
{
    // pic_timing carrying an emulation prevention byte, then HDR10+, then a
    // mastering display message: only the middle one goes, and the survivors
    // are re-escaped behind a fresh rbsp trailer.
    struct au au = {0}, expected = {0};
    NAL(&au, 1, PREFIX_SEI,
        0x01, 0x03, 0x00, 0x00, 0x03, 0x01,
        HDR10PLUS,
        0x89, 0x03, 0x00, 0x00, 0x03, 0x00,
        0x80);
    NAL(&expected, 1, PREFIX_SEI,
        0x01, 0x03, 0x00, 0x00, 0x03, 0x01,
        0x89, 0x03, 0x00, 0x00, 0x03, 0x00,
        0x80);
    slice(&au, IDR, 100);
    slice(&expected, IDR, 100);

    reset(DV_FILTER_NONE);
    load(&au);
    filter("bundled SEI");
    expect_bytes(&expected, "bundled SEI keeps the other messages");

    // HDR10+ split across two messages of one NAL, all of them removed: the
    // whole NAL goes, including its leading zero_byte.
    struct au all = {0}, none = {0};
    slice(&all, VPS, 8);
    slice(&none, VPS, 8);
    NAL(&all, 1, PREFIX_SEI, HDR10PLUS, HDR10PLUS, 0x80);
    slice(&all, IDR, 100);
    slice(&none, IDR, 100);
    load(&all);
    filter("all messages removed");
    expect_bytes(&none, "SEI with only HDR10+ messages is removed whole");
}

static void test_malformed_sei_is_kept_verbatim(void)
{
    const struct { const char *name; uint8_t bytes[24]; size_t n; } cases[] = {
        {"escape followed by a byte above 3", {HDR10PLUS, 0x00, 0x00, 0x03, 0x05, 0x80}, 17},
        {"unescaped zero run", {0x01, 0x03, 0x00, 0x00, 0x00, HDR10PLUS, 0x80}, 18},
        {"missing rbsp trailer", {HDR10PLUS}, 12},
        {"message size past the payload", {0x04, 0x20, 0xB5, 0x00, 0x3C, 0x00, 0x01, 0x04, 0x00, 0x80}, 10},
        {"truncated header", {0}, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct au au = {0};
        slice(&au, VPS, 4);
        nal(&au, 1, PREFIX_SEI, cases[i].bytes, cases[i].n);
        slice(&au, IDR, 50);
        reset(DV_FILTER_NONE);
        load(&au);
        filter(cases[i].name);
        expect_untouched(&au, cases[i].name);
    }
}

static void test_strip_and_native_modes(void)
{
    struct au au = {0}, stripped = {0};
    slice(&au, VPS, 4);
    slice(&stripped, VPS, 4);
    NAL(&au, 1, RPU, 0x01, 0x02, 0x03);
    slice(&au, IDR, 200);
    slice(&stripped, IDR, 200);
    NAL(&au, 0, EL, 0x09, 0x08);
    NAL(&au, 1, SUFFIX_SEI, HASH, 0x80);
    NAL(&stripped, 1, SUFFIX_SEI, HASH, 0x80);

    reset(DV_FILTER_STRIP);
    load(&au);
    filter("strip");
    expect_bytes(&stripped, "strip drops the RPU and the enhancement layer");

    reset(DV_FILTER_NONE);
    load(&au);
    filter("native");
    expect_untouched(&au, "native profile 7 keeps both layers");
}

static void test_convert_rewrites_rpu(void)
{
    struct au au = {0}, expected = {0};
    slice(&au, VPS, 4);
    slice(&expected, VPS, 4);
    slice(&au, IDR, 200);
    slice(&expected, IDR, 200);
    NAL(&au, 0, EL, 0x09, 0x08);
    NAL(&au, 0, RPU, 0x01, 0x02, 0x03);
    PUT(&expected, 0, 0, 0, 1);
    put(&expected, converted_rpu, sizeof(converted_rpu));

    reset(DV_FILTER_CONVERT);
    load(&au);
    filter("convert");
    expect_bytes(&expected, "convert replaces the RPU and drops the layer");
    check(!warnings, "successful conversion does not warn");

    // A poisoned RPU is dropped rather than forwarded as profile 7 metadata,
    // with a single warning for the session.
    struct au bad = {0}, dropped = {0};
    slice(&bad, IDR, 200);
    slice(&dropped, IDR, 200);
    NAL(&bad, 1, RPU, RPU_POISON, 0x02, 0x03);
    NAL(&bad, 1, SUFFIX_SEI, HASH, 0x80);
    NAL(&dropped, 1, SUFFIX_SEI, HASH, 0x80);
    for (int i = 0; i < 2; i++) {
        load(&bad);
        filter("poisoned RPU");
        expect_bytes(&dropped, "unconvertible RPU is dropped");
    }
    check(warnings == 1, "conversion failure warns once");
}

static void test_emptied_access_unit(void)
{
    struct au au = {0};
    NAL(&au, 1, PREFIX_SEI, HDR10PLUS, 0x80);
    reset(DV_FILTER_NONE);
    load(&au);
    filter("emptied");
    check(pkt.size == 0 && !pkt.data && !pkt.buf,
          "access unit reduced to nothing becomes an empty packet");
}

static void test_output_buffer_is_reused(void)
{
    struct au au = {0}, expected = {0};
    NAL(&au, 1, PREFIX_SEI, HDR10PLUS, 0x80);
    slice(&au, IDR, 4000);
    slice(&expected, IDR, 4000);

    reset(DV_FILTER_NONE);
    load(&au);
    filter("first rewrite");
    expect_bytes(&expected, "first rewrite");
    const uint8_t *first = pkt.data;
    int allocs = buffer_allocs;
    check(allocs == 1, "first rewrite allocates the output buffer once");

    // The consumer copied the packet into the codec and released it: the
    // next rewrite lands in the same buffer without allocating.
    load(&au);
    filter("second rewrite");
    expect_bytes(&expected, "second rewrite");
    check(pkt.data == first && buffer_allocs == 0,
          "released output buffer is reused without allocating");

    // A consumer still holding the previous packet forces a fresh buffer.
    AVBufferRef *held = av_buffer_ref(pkt.buf);
    load(&au);
    filter("third rewrite");
    expect_bytes(&expected, "third rewrite");
    check(pkt.data != first && buffer_allocs == 1,
          "held output buffer is never written again");
    av_buffer_unref(&held);

    // Growth past the reserved size (a converted RPU larger than the input's)
    // keeps the padding invariant.
    struct au tiny = {0}, grown = {0};
    NAL(&tiny, 0, RPU, 0x01);
    PUT(&grown, 0, 0, 0, 1);
    put(&grown, converted_rpu, sizeof(converted_rpu));
    reset(DV_FILTER_CONVERT);
    load(&tiny);
    filter("growth");
    expect_bytes(&grown, "output larger than the input keeps its padding");
}

int main(void)
{
    test_unchanged_access_unit_is_not_copied();
    test_hdr10plus_only_sei_is_removed();
    test_bundled_sei_keeps_other_messages();
    test_malformed_sei_is_kept_verbatim();
    test_strip_and_native_modes();
    test_convert_rewrites_rpu();
    test_emptied_access_unit();
    test_output_buffer_is_reused();
    reset(DV_FILTER_NONE);
    check(live_buffers == 0, "no buffer leaks");
    puts("PASS: MediaCodec Dolby Vision filter leaves unchanged access units "
         "alone, strips HDR10+ SEI, converts RPUs and reuses its scratch");
    return 0;
}
