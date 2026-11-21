// Behavioral regression for patch 0244, compiled by
// test_demux_raw_input_bytes.sh from the patched tree: stream_read_unbuffered()
// from stream/stream.c, which counts the bytes a stream reads, and
// update_bytes_read() and get_raw_input_bytes() from demux/demux.c, which
// collect that count and add up demuxer-cache-state's raw-input-bytes.
//
// What must hold: bytes the stream reads while nothing collects them (a low
// level seek building a Matroska index, plezy#3128) show in raw-input-bytes
// at once; collecting them neither counts a byte twice nor loses one, so the
// total a client samples never goes back, whatever the demuxer thread is doing;
// and the rate statistics still get every byte exactly once.
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define mp_assert assert

struct mp_cancel;
static bool mp_cancel_test(struct mp_cancel *c)
{
    (void)c;
    return false;
}

// The fields the three functions reach; the rest of struct stream is the
// buffer and the stream implementation.
typedef struct stream {
    int (*fill_buffer)(struct stream *s, void *buffer, int max_len);
    int64_t pos;
    int eof;
    struct mp_cancel *cancel;
    _Atomic uint64_t total_unbuffered_read_bytes;
    uint64_t total_stream_seeks;
} stream_t;

struct demuxer {
    struct stream *stream;
};

struct demux_internal {
    struct demuxer *d_thread;
    struct demuxer *d_user;
    uint64_t raw_input_bytes;
    int64_t slave_unbuffered_read_bytes;
    int64_t hack_unbuffered_read_bytes;
    int64_t cache_unbuffered_read_bytes;
    int64_t byte_level_seeks;
};

#include "stream_read_unbuffered.inc"
#include "demux_raw_input_bytes.inc"

static int failures;
#define CHECK(cond, ...) do {                                   \
        if (!(cond)) {                                          \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                       \
            fprintf(stderr, "\n");                              \
            failures++;                                         \
        }                                                       \
    } while (0)

// A network read returns whatever arrived: anything from 1 byte to the ask.
// Only one thread reads at a time.
static uint32_t rng_state = 0x3128;
static int fill_partial(struct stream *s, void *buffer, int max_len)
{
    (void)s;
    (void)buffer;
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return 1 + (int)(rng_state % (uint32_t)max_len);
}

struct fixture {
    struct stream stream;
    struct demuxer d_thread, d_user;
    struct demux_internal in;
};

static void fixture_init(struct fixture *f)
{
    *f = (struct fixture){0};
    f->stream.fill_buffer = fill_partial;
    f->d_thread.stream = &f->stream;
    f->d_user.stream = &f->stream;
    f->in.d_thread = &f->d_thread;
    f->in.d_user = &f->d_user;
}

static char scratch[65536];

// A low level seek: the demuxer thread reads and nothing collects. Every byte
// must show the moment it is read, and collecting after the seek must leave
// the total where it was while the rate statistics take the bytes.
static void test_seek_reads_show_at_once(void)
{
    struct fixture f;
    fixture_init(&f);
    uint64_t read = 0;
    for (int n = 0; n < 1000; n++) {
        read += stream_read_unbuffered(&f.stream, scratch, sizeof(scratch));
        CHECK(get_raw_input_bytes(&f.in) == read,
              "read %d: raw-input-bytes %" PRIu64 ", read %" PRIu64,
              n, get_raw_input_bytes(&f.in), read);
    }
    // A demuxer that reads around the stream (lavf's own protocols) reports
    // its bytes for the next collection.
    f.in.slave_unbuffered_read_bytes += 4096;
    update_bytes_read(&f.in);
    CHECK(get_raw_input_bytes(&f.in) == read + 4096,
          "after collecting: %" PRIu64 ", expected %" PRIu64,
          get_raw_input_bytes(&f.in), read + 4096);
    CHECK(f.in.cache_unbuffered_read_bytes == (int64_t)(read + 4096) &&
          f.in.hack_unbuffered_read_bytes == (int64_t)(read + 4096),
          "rate statistics got %" PRId64 " and %" PRId64 ", expected %" PRIu64,
          f.in.cache_unbuffered_read_bytes, f.in.hack_unbuffered_read_bytes, read + 4096);
    // Collecting again finds nothing new and changes nothing.
    update_bytes_read(&f.in);
    CHECK(get_raw_input_bytes(&f.in) == read + 4096 &&
          f.in.cache_unbuffered_read_bytes == (int64_t)(read + 4096),
          "a second collection counted bytes again");
}

// A demuxer that has closed its stream (a fully read subtitle file) reports
// what it collected.
static void test_no_stream(void)
{
    struct fixture f;
    fixture_init(&f);
    f.in.raw_input_bytes = 777;
    f.d_user.stream = NULL;
    CHECK(get_raw_input_bytes(&f.in) == 777, "without a stream: %" PRIu64,
          get_raw_input_bytes(&f.in));
}

// The demuxer thread reads and, between reads, collects with the lock held
// (update_cache()); the player samples with the lock held
// (demux_get_reader_state()), as often as it likes.
#define READS 2000000
static struct fixture shared;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t produced;
static _Atomic bool done;

static void *demuxer_thread(void *arg)
{
    (void)arg;
    for (int n = 0; n < READS; n++) {
        int got = stream_read_unbuffered(&shared.stream, scratch, 4096);
        atomic_fetch_add(&produced, (uint64_t)got);
        if (n % 97 == 0) {
            pthread_mutex_lock(&lock);
            update_bytes_read(&shared.in);
            pthread_mutex_unlock(&lock);
        }
    }
    atomic_store(&done, true);
    return NULL;
}

static void test_samples_never_go_back(void)
{
    fixture_init(&shared);
    pthread_t thread;
    if (pthread_create(&thread, NULL, demuxer_thread, NULL)) {
        fprintf(stderr, "pthread_create failed\n");
        exit(1);
    }
    uint64_t last = 0, samples = 0;
    while (!atomic_load(&done)) {
        pthread_mutex_lock(&lock);
        uint64_t sample = get_raw_input_bytes(&shared.in);
        pthread_mutex_unlock(&lock);
        uint64_t ceiling = atomic_load(&produced);
        samples++;
        // A read counts in the stream before the thread adds it to produced,
        // so a sample may run ahead by at most the one read in flight.
        CHECK(sample >= last, "sample %" PRIu64 " went back from %" PRIu64, sample, last);
        CHECK(sample <= ceiling + 4096, "sample %" PRIu64 " exceeds the %" PRIu64 " bytes read",
              sample, ceiling);
        if (failures)
            break;
        last = sample;
    }
    pthread_join(thread, NULL);
    update_bytes_read(&shared.in);
    uint64_t total = atomic_load(&produced);
    CHECK(get_raw_input_bytes(&shared.in) == total,
          "final total %" PRIu64 ", read %" PRIu64, get_raw_input_bytes(&shared.in), total);
    CHECK(shared.in.cache_unbuffered_read_bytes == (int64_t)total &&
          shared.in.hack_unbuffered_read_bytes == (int64_t)total,
          "rate statistics got %" PRId64 " and %" PRId64 ", read %" PRIu64,
          shared.in.cache_unbuffered_read_bytes, shared.in.hack_unbuffered_read_bytes, total);
    printf("%" PRIu64 " samples while %d reads were collected\n", samples, READS);
}

int main(void)
{
    test_seek_reads_show_at_once();
    test_no_stream();
    test_samples_never_go_back();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("raw-input-bytes: all checks passed\n");
    return 0;
}
