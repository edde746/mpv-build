// Behavioral regressions for how sd_ass tells a subtitle packet it decoded
// before (packet_key, check_packet_seen), compiled verbatim from the patched
// tree by test_sd_ass_seen_packets.sh. sd_ass keeps its events across a seek
// and decodes only the packets it has not seen: one it takes for new is a
// line drawn twice, stacked; one it takes for seen is a line missing.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct demux_packet {
    double pts;
    double duration;
    int64_t pos;
    unsigned char *buffer;
    size_t len;
    int seen_pos;
};

#include "sd_ass_seen_packet_types.inc"

struct sd_ass_priv {
    struct seen_packet *seen_packets;
    int num_seen_packets;
};

struct sd {
    struct sd_ass_priv *priv;
};

#define MP_TARRAY_INSERT_AT(ctx, p, idxvar, at, ...) do {                     \
        (void)(ctx);                                                          \
        (p) = realloc((p), sizeof((p)[0]) * ((size_t)(idxvar) + 1));          \
        memmove((p) + (at) + 1, (p) + (at), sizeof((p)[0]) * ((idxvar) - (at))); \
        (p)[at] = (__VA_ARGS__);                                              \
        (idxvar)++;                                                           \
    } while (0)

#include "sd_ass_seen_packets.inc"

static int failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static struct demux_packet packet(int64_t pos, double pts, double duration, const char *text)
{
    return (struct demux_packet){
        .pts = pts, .duration = duration, .pos = pos,
        .buffer = (unsigned char *)text, .len = strlen(text),
    };
}

static bool seen(struct sd *sd, struct demux_packet p)
{
    return check_packet_seen(sd, &p);
}

int main(void)
{
    struct sd_ass_priv priv = {0};
    struct sd sd = {.priv = &priv};

    // HLS: no positions. A line read again after a seek is seen; a different
    // line at the same time, or the same text at another time or for another
    // duration, is new.
    check(!seen(&sd, packet(-1, 1.0, 2.0, "Hello")), "an HLS line read first is new");
    check(seen(&sd, packet(-1, 1.0, 2.0, "Hello")), "an HLS line read again is seen");
    check(!seen(&sd, packet(-1, 1.0, 2.0, "World")), "another HLS line at the same time is new");
    check(!seen(&sd, packet(-1, 5.0, 2.0, "Hello")), "the same HLS text later is new");
    check(!seen(&sd, packet(-1, 1.0, 3.0, "Hello")), "the same HLS text for longer is new");
    check(seen(&sd, packet(-1, 1.0, 2.0, "World")), "the other HLS line read again is seen");

    // A file with positions keeps its keys: the position and pts decide, so
    // the same text in two places is two lines and a line read again is one.
    check(!seen(&sd, packet(1000, 1.0, 2.0, "Hello")), "a line at a position read first is new");
    check(seen(&sd, packet(1000, 1.0, 2.0, "Hello")), "a line at a position read again is seen");
    check(!seen(&sd, packet(2000, 1.0, 2.0, "Hello")), "the same text at another position is new");

    // A key for a packet without a position never equals a file position.
    for (int n = 0; n < 1000; n++) {
        char text[32];
        snprintf(text, sizeof(text), "line %d", n);
        struct demux_packet p = packet(-1, n, 1.0, text);
        if (packet_key(&p) >= 0) {
            check(false, "a key without a position is negative");
            break;
        }
    }

    free(priv.seen_packets);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("sd_ass seen packets: all checks passed\n");
    return 0;
}
