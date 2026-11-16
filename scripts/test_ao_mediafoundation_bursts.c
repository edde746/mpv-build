// Behavioral test for the IEC 61937 parser of ao_mediafoundation.c (windows
// patch 0300), extracted by test_ao_mediafoundation_bursts.sh: bursts_feed()
// and bursts_reset() as the AO calls them from write() and reset(), fed the
// way audio/out/buffer.c feeds a push AO, any number of frames at a time.
//
// What must hold: every E-AC-3 payload comes out exactly once, complete and
// unswapped, with the stream offset of its burst (the AO's timestamps), however
// the writes split the stream. Garbage and zero padding are passed over, and so
// are bursts of other data types, which are reported. A payload the callback
// refuses (out of memory) is kept and handed out again with the next write.
// reset() forgets a burst that was not complete yet.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bursts.inc"

// --audio-spdif=eac3: a stereo 16 bit carrier, BURST_FRAMES frames a burst.
#define BURST_BYTES (BURST_FRAMES * 4)
#define MAX_PAYLOAD (BURST_BYTES - BURST_HEADER)

static int failures;

#define CHECK(cond, ...) do {                                   \
        if (!(cond)) {                                          \
            printf("FAIL line %d: ", __LINE__);                 \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
            failures++;                                         \
        }                                                       \
    } while (0)

#define MAX_GOT 8192

struct got {
    uint8_t *data;                      // every payload, concatenated
    size_t len, cap;
    int64_t offsets[MAX_GOT];
    size_t sizes[MAX_GOT];
    int count;
    int refuse_at;                      // refuse the call with this index once
    int refused;
};

static bool collect(void *ctx, const uint8_t *payload, size_t size, int64_t offset)
{
    struct got *g = ctx;
    if (g->count == g->refuse_at && !g->refused) {
        g->refused = 1;
        return false;
    }
    if (g->len + size > g->cap) {
        g->cap = (g->len + size) * 2;
        g->data = realloc(g->data, g->cap);
        if (!g->data) {
            printf("out of memory\n");
            exit(2);
        }
    }
    memcpy(g->data + g->len, payload, size);
    g->len += size;
    if (g->count < MAX_GOT) {
        g->offsets[g->count] = offset;
        g->sizes[g->count] = size;
    }
    g->count++;
    return true;
}

static uint32_t rng = 12345;

static uint32_t next_rand(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}

// One burst as libavformat's spdifenc writes it: the preamble, the payload as
// byte-swapped 16 bit words (an odd length padded with a zero byte), zeros.
static void put_burst(uint8_t *dst, unsigned type, size_t length_field,
                      const uint8_t *payload, size_t size)
{
    memset(dst, 0, BURST_BYTES);
    dst[0] = 0x72;
    dst[1] = 0xF8;
    dst[2] = 0x1F;
    dst[3] = 0x4E;
    dst[4] = type;
    dst[6] = length_field & 0xFF;
    dst[7] = (length_field >> 8) & 0xFF;
    for (size_t n = 0; n < size; n += 2) {
        dst[BURST_HEADER + n] = n + 1 < size ? payload[n + 1] : 0;
        dst[BURST_HEADER + n + 1] = payload[n];
    }
}

enum split { WHOLE, BYTES, FRAMES, RANDOM, NUM_SPLITS };
static const char *const split_names[] = {"one write", "single bytes",
                                          "whole frames", "random sizes"};

// Feed data the way the AO gets it, and hand a refused payload out again with
// the next (here: an empty) write, as audio_write() does.
static void feed(struct bursts *b, const uint8_t *data, size_t len,
                 enum split split, struct got *g)
{
    static const uint8_t none[1];
    size_t pos = 0;
    while (pos < len) {
        size_t chunk = len - pos;
        if (split == BYTES)
            chunk = 1;
        else if (split == FRAMES)
            chunk = 4 * (1 + next_rand() % 3000);
        else if (split == RANDOM)
            chunk = 1 + next_rand() % 9000;
        if (chunk > len - pos)
            chunk = len - pos;
        if (!bursts_feed(b, data + pos, chunk, MAX_PAYLOAD, collect, g)) {
            CHECK(g->refused == 1, "bursts_feed failed without a refusal");
            CHECK(bursts_feed(b, none, 0, MAX_PAYLOAD, collect, g),
                  "the kept payload did not come out with the next write");
        }
        pos += chunk;
    }
}

struct expect {
    uint8_t *payloads;
    size_t len;
    int64_t offsets[64];
    size_t sizes[64];
    int count;
};

// E-AC-3 bursts of every awkward length, with garbage, an AC-3 burst, a burst
// with an impossible length and one with none in between.
static size_t make_stream(uint8_t *out, struct expect *e)
{
    static const size_t lengths[] = {
        2, 1, 3, 100, 1001, 3072, MAX_PAYLOAD, MAX_PAYLOAD - 1, 4, 5,
    };
    static uint8_t payload[MAX_PAYLOAD];
    size_t pos = 0;
    e->len = 0;
    e->count = 0;
    for (size_t k = 0; k < sizeof(lengths) / sizeof(lengths[0]); k++) {
        size_t size = lengths[k];
        for (size_t n = 0; n < size; n++)
            payload[n] = next_rand() & 0xFF;
        put_burst(out + pos, BURST_TYPE_EAC3, size, payload, size);
        memcpy(e->payloads + e->len, payload, size);
        e->len += size;
        e->offsets[e->count] = (int64_t)pos;
        e->sizes[e->count] = size;
        e->count++;
        pos += BURST_BYTES;

        switch (k) {
        case 1:     // garbage that is not a preamble, a word count not a burst
            for (int n = 0; n < 37; n++) {
                out[pos++] = 0x55;
                out[pos++] = 0xAA;
            }
            break;
        case 3:     // an AC-3 burst: not ours
            put_burst(out + pos, 0x01, 1536, payload, 1536);
            pos += BURST_BYTES;
            break;
        case 5:     // E-AC-3 claiming more than a burst can carry
            put_burst(out + pos, BURST_TYPE_EAC3, 0xFFFF, payload, 64);
            pos += BURST_BYTES;
            break;
        case 7:     // E-AC-3 with no payload
            put_burst(out + pos, BURST_TYPE_EAC3, 0, payload, 0);
            pos += BURST_BYTES;
            break;
        }
    }
    return pos;
}

static void check_synthetic(enum split split, int refuse_at)
{
    static uint8_t stream[BURST_BYTES * 16];
    static uint8_t expected_bytes[BURST_BYTES * 16];
    struct expect e = {.payloads = expected_bytes};
    size_t len = make_stream(stream, &e);

    struct bursts b = {0};
    struct got g = {.refuse_at = refuse_at};
    feed(&b, stream, len, split, &g);

    const char *how = split_names[split];
    CHECK(g.count == e.count, "%s: %d payloads, expected %d", how, g.count, e.count);
    CHECK(g.len == e.len && !memcmp(g.data, e.payloads, e.len),
          "%s: the payloads differ from what went in", how);
    for (int n = 0; n < g.count && n < e.count; n++) {
        CHECK(g.sizes[n] == e.sizes[n], "%s: payload %d has %zu bytes, expected %zu",
              how, n, g.sizes[n], e.sizes[n]);
        CHECK(g.offsets[n] == e.offsets[n], "%s: payload %d at %lld, expected %lld",
              how, n, (long long)g.offsets[n], (long long)e.offsets[n]);
    }
    CHECK(b.skipped_other, "%s: the bursts that are not E-AC-3 went unreported", how);
    CHECK(b.len < BURST_HEADER, "%s: %zu bytes left over after the last burst", how, b.len);
    if (refuse_at >= 0)
        CHECK(g.refused == 1, "%s: the refusal never happened", how);
    bursts_free(&b);
    free(g.data);
}

// A burst cut off by a reset must not come back, and offsets start over.
static void check_reset(void)
{
    static uint8_t stream[BURST_BYTES * 2];
    static uint8_t payload[3000];
    for (size_t n = 0; n < sizeof(payload); n++)
        payload[n] = next_rand() & 0xFF;
    put_burst(stream, BURST_TYPE_EAC3, sizeof(payload), payload, sizeof(payload));
    put_burst(stream + BURST_BYTES, BURST_TYPE_EAC3, sizeof(payload), payload,
              sizeof(payload));

    struct bursts b = {0};
    struct got g = {.refuse_at = -1};
    bursts_feed(&b, stream, 1000, MAX_PAYLOAD, collect, &g);
    CHECK(g.count == 0, "reset: a payload came out of a third of a burst");
    bursts_reset(&b);
    bursts_feed(&b, stream + BURST_BYTES, BURST_BYTES, MAX_PAYLOAD, collect, &g);
    CHECK(g.count == 1 && g.offsets[0] == 0 && g.len == sizeof(payload) &&
          !memcmp(g.data, payload, sizeof(payload)),
          "reset: expected the one burst after it, at offset 0");
    bursts_free(&b);
    free(g.data);
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(size > 0 ? size : 1);
    if (!data || fread(data, 1, size, f) != (size_t)size) {
        printf("cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    *len = size;
    return data;
}

// FFmpeg's spdif muxer, which ad_spdif uses: the payloads, in order, are the
// E-AC-3 frames it was given, one burst every BURST_BYTES.
static void check_ffmpeg(const char *carrier_path, const char *frames_path)
{
    size_t carrier_len, frames_len;
    uint8_t *carrier = read_file(carrier_path, &carrier_len);
    uint8_t *frames = read_file(frames_path, &frames_len);
    for (enum split split = 0; split < NUM_SPLITS; split++) {
        if (split == BYTES && carrier_len > 4 * 1024 * 1024)
            continue;
        struct bursts b = {0};
        struct got g = {.refuse_at = 7};
        feed(&b, carrier, carrier_len, split, &g);
        const char *how = split_names[split];
        CHECK(g.count > 10, "ffmpeg, %s: only %d payloads", how, g.count);
        CHECK(g.len == frames_len && !memcmp(g.data, frames, frames_len),
              "ffmpeg, %s: %zu payload bytes do not match the %zu bytes of E-AC-3 frames",
              how, g.len, frames_len);
        for (int n = 0; n < g.count && n < MAX_GOT; n++) {
            if (g.offsets[n] != (int64_t)n * BURST_BYTES) {
                CHECK(0, "ffmpeg, %s: burst %d at %lld", how, n, (long long)g.offsets[n]);
                break;
            }
        }
        CHECK(!b.skipped_other, "ffmpeg, %s: reported a burst that is not E-AC-3", how);
        bursts_free(&b);
        free(g.data);
    }
    free(carrier);
    free(frames);
}

int main(int argc, char **argv)
{
    for (enum split split = 0; split < NUM_SPLITS; split++) {
        check_synthetic(split, -1);
        check_synthetic(split, 4);
    }
    check_reset();
    if (argc == 3)
        check_ffmpeg(argv[1], argv[2]);
    else
        printf("no FFmpeg stream given: skipped that part\n");

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("ao_mediafoundation bursts: all checks passed\n");
    return 0;
}
