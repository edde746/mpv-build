// Feed IEC 61937 TrueHD bursts from FFmpeg's spdif muxer through the
// production reassembly and check every access unit it hands the raw track
// against the unit stream FFmpeg packed: same bytes, same order, whole groups
// of THD_GROUP_UNITS, credited one unit at a time. See test_audiotrack_truehd.sh.
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AF_FORMAT_S_DTS 99
#define AF_FORMAT_S_DTSHD 98
#define AF_FORMAT_S_TRUEHD 97

typedef int jint;
typedef void *jobject, *jbyteArray, *jshortArray, *jfloatArray;
typedef int mp_thread, mp_mutex, mp_cond;
struct ao { void *priv; int samplerate, sstride, format; };

static int warnings, reloads;
static void count_warning(const char *fmt, ...) { (void)fmt; warnings++; }
static void discard(const char *fmt, ...) { (void)fmt; }
#define MP_WARN(ao, ...) ((void)(ao), count_warning(__VA_ARGS__))
#define MP_VERBOSE(ao, ...) ((void)(ao), discard(__VA_ARGS__))
static const char *af_fmt_to_str(int format) { (void)format; return "spdif"; }
static void ao_request_reload(struct ao *ao) { (void)ao; reloads++; }

#include "audiotrack_truehd.inc"

static void fail(const char *what)
{
    fprintf(stderr, "FAIL: %s\n", what);
    exit(1);
}

static uint8_t *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        fail(path);
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(*len);
    if (fread(data, 1, *len, f) != *len)
        fail(path);
    fclose(f);
    return data;
}

static int unit_len(const uint8_t *unit)
{
    return ((unit[0] & 0xf) << 8 | unit[1]) * 2;
}

static bool major_sync(const uint8_t *unit)
{
    return unit_len(unit) >= 10 && unit[4] == 0xf8 && unit[5] == 0x72 &&
           unit[6] == 0x6f && unit[7] == 0xba;
}

struct units { const uint8_t *data; size_t *off; int count; };

// The .thd file is the unit stream FFmpeg's truehd muxer wrote: one packet
// per unit, back to back.
static struct units split(const uint8_t *data, size_t len)
{
    struct units u = {.data = data, .off = malloc(sizeof(size_t) * (len / 4 + 1))};
    size_t off = 0;
    while (off + 4 <= len) {
        int n = unit_len(data + off);
        if (n < 4 || off + n > len)
            fail("reference is not a TrueHD unit stream");
        u.off[u.count++] = off;
        off += n;
    }
    u.off[u.count] = off;
    return u;
}

struct run {
    struct ao ao;
    struct priv p;
    uint8_t *out;
    size_t out_len;
    int out_units;
    int credit;
};

static void begin(struct run *r, size_t cap)
{
    memset(r, 0, sizeof(*r));
    r->ao = (struct ao){.priv = &r->p, .samplerate = 192000, .sstride = 16,
                        .format = AF_FORMAT_S_TRUEHD};
    r->p.thd_capacity = MAT_BURST_SIZE + (THD_GROUP_UNITS + 1) * THD_MAX_UNIT;
    r->p.rawbuf = malloc(r->p.thd_capacity);
    r->p.raw_state = RAW_SYNC_PA;
    r->p.raw_passthrough = true;
    r->out = malloc(cap);
    AudioTrack_thdReset(&r->p);
    warnings = reloads = 0;
    atomic_store(thd_raw_failed(), false);
}

// The worker's pass: unwrap one read, bank the credit, and take whole groups
// the way AudioTrack_write hands them to the track.
static void feed(struct run *r, const uint8_t *spdif, size_t len, size_t chunk)
{
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = len - off < chunk ? len - off : chunk;
        AudioTrack_unwrapIEC61937(&r->ao, NULL, spdif + off, (int)n);
        r->credit += r->p.thd_credit;
        r->p.thd_credit = 0;
        int units;
        int bytes = AudioTrack_thdGroup(&r->p, &units);
        if (units % THD_GROUP_UNITS)
            fail("a write is not whole groups");
        if (!bytes)
            continue;
        memcpy(r->out + r->out_len, r->p.rawbuf, bytes);
        r->out_len += bytes;
        r->out_units += units;
        AudioTrack_thdTake(&r->p, bytes, units);
        if (r->p.thd_complete >= THD_GROUP_UNITS)
            fail("a whole group stayed behind");
    }
}

// Every unit written must be a reference unit, in order. Returns the index of
// the first one; *gaps counts jumps over reference units.
static int match(const struct run *r, const struct units *ref, int *gaps)
{
    int first = -1, next = 0;
    *gaps = 0;
    for (size_t off = 0; off < r->out_len;) {
        const uint8_t *unit = r->out + off;
        int n = unit_len(unit);
        int at = next;
        while (at < ref->count &&
               (ref->off[at + 1] - ref->off[at] != (size_t)n ||
                memcmp(ref->data + ref->off[at], unit, n)))
            at++;
        if (at == ref->count)
            fail("a written unit is not a unit FFmpeg packed");
        if (first < 0)
            first = at;
        else if (at != next)
            (*gaps)++;
        next = at + 1;
        off += n;
    }
    return first;
}

static void check_stream(const char *thd_path, const char *spdif_path)
{
    size_t thd_len, spdif_len;
    uint8_t *thd = load(thd_path, &thd_len);
    uint8_t *spdif = load(spdif_path, &spdif_len);
    struct units ref = split(thd, thd_len);
    if (!major_sync(thd))
        fail("reference does not open on a major sync");

    // Production shape: one burst per read, from the start of the stream.
    struct run whole;
    begin(&whole, thd_len);
    feed(&whole, spdif, spdif_len, MAT_BURST_SIZE);
    int gaps;
    if (match(&whole, &ref, &gaps) != 0 || gaps)
        fail("units from the start are not the reference prefix");
    if (memcmp(whole.out, thd, whole.out_len))
        fail("output is not byte-identical to the reference prefix");
    // Lost at the end: FFmpeg never emits the last, unfinished MAT frame,
    // and fewer than a group stays held.
    if (whole.out_units < ref.count - 2 * THD_GROUP_UNITS - 8)
        fail("units went missing mid-stream");
    if (whole.credit != whole.out_units + whole.p.thd_complete)
        fail("credit is not one per completed unit");
    if (warnings || reloads)
        fail("a clean stream warned or demoted");

    // Reads that split words and bursts anywhere: identical output.
    const size_t chunks[] = {1001, 4096, 61439};
    for (size_t n = 0; n < sizeof(chunks) / sizeof(chunks[0]); n++) {
        struct run r;
        begin(&r, thd_len);
        feed(&r, spdif, spdif_len, chunks[n]);
        if (r.out_len != whole.out_len || memcmp(r.out, whole.out, r.out_len))
            fail("output depends on how the carrier was read");
        free(r.out);
        free(r.p.rawbuf);
    }

    // Picked up mid-burst and mid-unit, on a carrier frame as the core always
    // resumes: after the reset a pause does, the parse must find a major sync
    // before it writes anything, then follow the stream exactly.
    struct run mid;
    begin(&mid, thd_len);
    size_t skip = 2 * MAT_BURST_SIZE + 30000; // 1875 frames of 16 bytes
    feed(&mid, spdif + skip, spdif_len - skip, MAT_BURST_SIZE);
    int first = match(&mid, &ref, &gaps);
    if (first < 0 || gaps || !major_sync(ref.data + ref.off[first]))
        fail("a mid-stream pickup did not resume on a major sync");
    if (first > 2 * THD_GROUP_UNITS + 200)
        fail("a mid-stream pickup took too long to resync");

    // A burst whose MAT start code is damaged: its units cannot be placed,
    // so they are dropped and the parse resyncs; nothing wrong is written.
    struct run bad;
    begin(&bad, thd_len);
    uint8_t *damaged = malloc(spdif_len);
    memcpy(damaged, spdif, spdif_len);
    damaged[5 * MAT_BURST_SIZE + 8] ^= 0xff; // byte-swapped start code, first byte
    feed(&bad, damaged, spdif_len, MAT_BURST_SIZE);
    if (match(&bad, &ref, &gaps) != 0 || gaps != 1 || warnings != 1)
        fail("a damaged MAT code was not dropped cleanly");
    free(damaged);

    printf("PASS: %s: %d of %d units rebuilt byte-exact in %d-unit groups; "
           "mid-stream pickup resumes at unit %d, a major sync\n",
           thd_path, whole.out_units, ref.count, THD_GROUP_UNITS, first);
}

// 44.1 kHz-family units last 1/1102.5 s: the raw track cannot clock them, so
// the first major sync demotes TrueHD and nothing is written.
static void check_demote(const char *thd_path, const char *spdif_path)
{
    size_t thd_len, spdif_len;
    uint8_t *thd = load(thd_path, &thd_len);
    uint8_t *spdif = load(spdif_path, &spdif_len);
    if (!major_sync(thd) || !(thd[8] & 0x80))
        fail("fixture is not 44.1 kHz-family TrueHD");
    struct run r;
    begin(&r, thd_len);
    feed(&r, spdif, spdif_len, MAT_BURST_SIZE);
    if (reloads != 1 || !atomic_load(thd_raw_failed()) || !atomic_load(&r.p.failed))
        fail("44.1 kHz TrueHD did not demote once");
    if (r.out_units || r.credit)
        fail("44.1 kHz TrueHD reached the raw track");
    printf("PASS: 44.1 kHz TrueHD demotes to decoding and writes nothing\n");
}

int main(int argc, char **argv)
{
    if (argc != 4)
        fail("usage: test stream|demote UNITS.thd BURSTS.spdif");
    if (!strcmp(argv[1], "stream"))
        check_stream(argv[2], argv[3]);
    else if (!strcmp(argv[1], "demote"))
        check_demote(argv[2], argv[3]);
    else
        fail("unknown mode");
    return 0;
}
