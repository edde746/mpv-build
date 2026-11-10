// Behavioral regression for mpv patch 0240, extracted by
// test_demux_mkv_defer_tags.sh from demux/demux_mkv.c: read_trailing_headers()
// (what demux_mkv_open() reads behind the clusters), read_deferred_cues()
// (what the first seek reads), trailing_headers_can_wait(),
// read_deferred_element() and process_tags(), with the real struct mkv_demuxer
// and struct demux_mkv_opts. The stream, the EBML reader and the tag store are
// stand-ins that record what was read and where the stream had to seek.
//
// mkvmerge writes the Cues and then the Tags after the clusters. mpv used to
// defer the Cues only when they were the one element there, so every such
// file was opened with a seek to the end and one back: over HTTP, two more
// connections before the first packet.
//
// What must hold:
//   - on a network stream, Cues and Tags behind the clusters are both left
//     alone when the file is opened, and the first seek reads them in file
//     order with a single seek;
//   - on a local file, with --demuxer-mkv-defer-tags=no, or with replaygain
//     enabled, opening reads exactly what it read before;
//   - anything else behind the clusters (chapters here) is still read on
//     opening, with everything that sits beside it;
//   - Tags that waited are never fetched on their own: a file whose index is
//     already complete, or has no Cues, does not seek for them;
//   - Tags read after opening reach the global metadata once, with a metadata
//     change, and leave track, chapter and edition tags untouched; read on
//     opening they go where they always went;
//   - on a network stream a seek to a header element that fails is tried once
//     more (a dropped connection must not cost the index, without which a seek
//     reads the file from its start); a local file is asked once, as before.
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MATROSKA_ID_CUES     0x1C53BB6B
#define MATROSKA_ID_TAGS     0x1254C367
#define MATROSKA_ID_CHAPTERS 0x1043A770

static int failures;
#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s: ", scenario); \
            printf(__VA_ARGS__); \
            printf("\n"); \
            failures++; \
        } \
    } while (0)
static const char *scenario = "";

// ---- stand-ins for what the extracted functions reach -------------------

static int warnings;
static void log_sink(const char *fmt, ...) { (void)fmt; }
#define MP_VERBOSE(ctx, ...) ((void)(ctx), log_sink(__VA_ARGS__))
#define MP_WARN(ctx, ...) ((void)(ctx), warnings++, log_sink(__VA_ARGS__))
#define MP_ERR(ctx, ...) ((void)(ctx), log_sink(__VA_ARGS__))

struct mp_tags {
    char *keys[16], *values[16];
    int num_keys;
};

static char *mp_tags_get_str(struct mp_tags *tags, const char *key)
{
    for (int n = 0; n < tags->num_keys; n++) {
        if (!strcmp(tags->keys[n], key))
            return tags->values[n];
    }
    return NULL;
}

static void mp_tags_set_str(struct mp_tags *tags, const char *key, const char *value)
{
    for (int n = 0; n < tags->num_keys; n++) {
        if (!strcmp(tags->keys[n], key)) {
            tags->values[n] = strdup(value);
            return;
        }
    }
    tags->keys[tags->num_keys] = strdup(key);
    tags->values[tags->num_keys++] = strdup(value);
}

static char *talloc_asprintf(void *parent, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return strdup(buf);
}
#define talloc_free free

typedef struct stream {
    bool seekable;
    int64_t pos;
} stream_t;

struct sh_stream { struct mp_tags *tags; };
typedef struct mkv_track {
    uint64_t uid;
    struct sh_stream *stream;
} mkv_track_t;
typedef struct mkv_index mkv_index_t;

struct ebml_simple_tag { char *tag_name, *tag_string; };
struct ebml_targets {
    uint64_t target_chapter_uid, target_edition_uid, target_track_uid, target_attachment_uid;
};
struct ebml_tag {
    struct ebml_targets targets;
    struct ebml_simple_tag *simple_tag;
    int n_simple_tag;
};
struct ebml_tags {
    struct ebml_tag *tag;
    int n_tag;
};

typedef struct demux_chapter {
    uint64_t demuxer_id;
    struct mp_tags *metadata;
} demux_chapter_t;
struct demux_edition {
    uint64_t demuxer_id;
    struct mp_tags *metadata;
};
struct demux_opts { int index_mode; };

typedef struct demuxer {
    void *priv;
    stream_t *stream;
    struct demux_opts *opts;
    bool is_network;
    struct mp_tags *metadata;
    demux_chapter_t *chapters;
    int num_chapters;
    struct demux_edition *editions;
    int num_editions;
} demuxer_t;

#include "demux_mkv_types.inc"

// The file: the level 1 elements behind the clusters.
#define FILE_SIZE 9000000000LL
struct element {
    uint32_t id;
    int64_t pos, size;
};
static struct element layout[4];
static int num_layout;

// What the functions under test did.
static uint32_t read_ids[8];
static int num_reads, num_seeks, metadata_changes;
// How many of the next seeks away from the current position fail.
static int failing_seeks;

static int64_t stream_get_size(stream_t *s) { return FILE_SIZE; }

static bool stream_seek(stream_t *s, int64_t pos)
{
    if (pos != s->pos && failing_seeks > 0) {
        failing_seeks--;
        return false;
    }
    if (pos != s->pos)
        num_seeks++;
    s->pos = pos;
    return true;
}

static uint32_t ebml_read_id(stream_t *s)
{
    for (int n = 0; n < num_layout; n++) {
        if (layout[n].pos == s->pos)
            return layout[n].id;
    }
    return 0;
}

static struct ebml_tags file_tags;

// demux_mkv.c's read_header_element(): marks the element read and parses it.
static int read_header_element(struct demuxer *demuxer, uint32_t id, int64_t start_filepos)
{
    struct mkv_demuxer *mkv_d = demuxer->priv;
    for (int n = 0; n < mkv_d->num_headers; n++) {
        if (mkv_d->headers[n].id == (int32_t)id)
            mkv_d->headers[n].parsed = true;
    }
    for (int n = 0; n < num_layout; n++) {
        if (layout[n].id == id)
            demuxer->stream->pos = layout[n].pos + layout[n].size;
    }
    read_ids[num_reads++] = id;
    if (id == MATROSKA_ID_CUES)
        mkv_d->index_complete = true;
    if (id == MATROSKA_ID_TAGS)
        mkv_d->tags = &file_tags;
    return 0;
}

static void demux_metadata_changed(demuxer_t *demuxer) { metadata_changes++; }

#include "demux_mkv_defer_tags.inc"

// ---- one opened file ----------------------------------------------------

#define TRACK_UID 11
#define CHAPTER_UID 22
#define EDITION_UID 33

struct file {
    struct mkv_demuxer mkv;
    struct demux_mkv_opts mkv_opts;
    struct demux_opts opts;
    struct demuxer demuxer;
    struct stream stream;
    struct mp_tags global, track_tags, chapter_tags, edition_tags;
    struct sh_stream sh;
    struct mkv_track track, *tracks[1];
    struct demux_chapter chapter;
    struct demux_edition edition;
    struct header_elem headers[4];
};

static struct ebml_simple_tag st_global = {"TITLE", "A film"};
static struct ebml_simple_tag st_track = {"BPS", "10033513"};
static struct ebml_simple_tag st_chapter = {"COMMENT", "chapter"};
static struct ebml_simple_tag st_edition = {"COMMENT", "edition"};
static struct ebml_tag tag_list[] = {
    {.simple_tag = &st_global, .n_simple_tag = 1},
    {.targets = {.target_track_uid = TRACK_UID}, .simple_tag = &st_track, .n_simple_tag = 1},
    {.targets = {.target_chapter_uid = CHAPTER_UID}, .simple_tag = &st_chapter, .n_simple_tag = 1},
    {.targets = {.target_edition_uid = EDITION_UID}, .simple_tag = &st_edition, .n_simple_tag = 1},
};

// Lay out `n` trailing elements in the given order, the first at 8.9 GB, each
// starting where the previous one ends, and register them the way the seek
// head does: known, not parsed.
static struct file *open_file(const uint32_t *ids, int n, bool network, bool defer_option,
                              bool replaygain, bool index_in_front, bool seekable)
{
    struct file *f = calloc(1, sizeof(*f));
    f->mkv_opts.defer_tags = defer_option;
    f->opts.index_mode = 1;
    f->stream.seekable = seekable;
    f->stream.pos = 5601;       // the first cluster
    f->sh.tags = &f->track_tags;
    f->track = (struct mkv_track){.uid = TRACK_UID, .stream = &f->sh};
    f->tracks[0] = &f->track;
    f->chapter = (struct demux_chapter){.demuxer_id = CHAPTER_UID, .metadata = &f->chapter_tags};
    f->edition = (struct demux_edition){.demuxer_id = EDITION_UID, .metadata = &f->edition_tags};
    f->mkv.opts = &f->mkv_opts;
    f->mkv.tracks = f->tracks;
    f->mkv.num_tracks = 1;
    f->mkv.headers = f->headers;
    f->mkv.index_complete = index_in_front;
    f->demuxer = (struct demuxer){
        .priv = &f->mkv, .stream = &f->stream, .opts = &f->opts, .is_network = network,
        .metadata = &f->global, .chapters = &f->chapter, .num_chapters = 1,
        .editions = &f->edition, .num_editions = 1,
    };
    file_tags = (struct ebml_tags){.tag = tag_list, .n_tag = 4};
    num_layout = n;
    int64_t pos = 8900000000LL;
    for (int i = 0; i < n; i++) {
        layout[i] = (struct element){ids[i], pos, 1000 * (i + 1)};
        f->headers[i] = (struct header_elem){.id = ids[i], .pos = pos};
        pos += layout[i].size;
    }
    f->mkv.num_headers = n;
    num_reads = num_seeks = metadata_changes = warnings = 0;

    // demux_mkv_open(), from the first cluster on.
    int r = read_trailing_headers(&f->demuxer, replaygain);
    CHECK(r == 0, "read_trailing_headers returned %d", r);
    stream_seek(&f->stream, 5601);
    process_tags(&f->demuxer);
    f->mkv.opened = true;
    return f;
}

static void expect_reads(const char *when, const uint32_t *want, int n, int seeks)
{
    bool same = num_reads == n;
    for (int i = 0; same && i < n; i++)
        same = read_ids[i] == want[i];
    if (!same) {
        printf("FAIL: %s: %s read", scenario, when);
        for (int i = 0; i < num_reads; i++)
            printf(" %08" PRIx32, read_ids[i]);
        printf(", want");
        for (int i = 0; i < n; i++)
            printf(" %08" PRIx32, want[i]);
        printf("\n");
        failures++;
    }
    CHECK(num_seeks == seeks, "%s took %d seek(s), want %d", when, num_seeks, seeks);
    num_reads = num_seeks = 0;
}

static void expect_tag(struct mp_tags *tags, const char *where, const char *key, const char *want)
{
    char *got = mp_tags_get_str(tags, key);
    CHECK((!got && !want) || (got && want && !strcmp(got, want)), "%s %s is %s, want %s", where, key,
          got ? got : "(unset)", want ? want : "(unset)");
}

// The tags as they are after a read on opening.
static void expect_tags_complete(struct file *f)
{
    expect_tag(&f->global, "global", "TITLE", "A film");
    expect_tag(&f->track_tags, "track", "BPS", "10033513");
    expect_tag(&f->chapter_tags, "chapter", "COMMENT", "chapter");
    expect_tag(&f->edition_tags, "edition", "COMMENT", "edition");
    CHECK(metadata_changes == 0, "%d metadata change(s) for tags read on opening", metadata_changes);
}

static void expect_tags(struct file *f, const char *global)
{
    expect_tag(&f->global, "global", "TITLE", global);
    expect_tag(&f->track_tags, "track", "BPS", NULL);
    expect_tag(&f->chapter_tags, "chapter", "COMMENT", NULL);
    expect_tag(&f->edition_tags, "edition", "COMMENT", NULL);
}

int main(void)
{
    static const uint32_t cues_tags[] = {MATROSKA_ID_CUES, MATROSKA_ID_TAGS};
    static const uint32_t tags_cues[] = {MATROSKA_ID_TAGS, MATROSKA_ID_CUES};
    static const uint32_t cues[] = {MATROSKA_ID_CUES};
    static const uint32_t tags[] = {MATROSKA_ID_TAGS};
    static const uint32_t all[] = {MATROSKA_ID_CUES, MATROSKA_ID_CHAPTERS, MATROSKA_ID_TAGS};
    struct file *f;

    scenario = "mkvmerge layout over the network";
    f = open_file(cues_tags, 2, true, true, false, false, true);
    expect_reads("opening", NULL, 0, 0);
    expect_tags(f, NULL);
    read_deferred_cues(&f->demuxer);
    expect_reads("the first seek", cues_tags, 2, 1);
    expect_tags(f, "A film");
    CHECK(metadata_changes == 1, "%d metadata change(s) for the late tags, want 1", metadata_changes);
    read_deferred_cues(&f->demuxer);
    expect_reads("the second seek", NULL, 0, 0);
    CHECK(metadata_changes == 1, "the second seek changed the metadata again");

    scenario = "mkvmerge layout, local file";
    f = open_file(cues_tags, 2, false, true, false, false, true);
    expect_reads("opening", cues_tags, 2, 2);       // to the end, and back to the cluster
    expect_tags_complete(f);
    read_deferred_cues(&f->demuxer);
    expect_reads("the first seek", NULL, 0, 0);

    scenario = "mkvmerge layout over the network, --demuxer-mkv-defer-tags=no";
    f = open_file(cues_tags, 2, true, false, false, false, true);
    expect_reads("opening", cues_tags, 2, 2);
    expect_tags_complete(f);

    scenario = "mkvmerge layout over the network, replaygain enabled";
    f = open_file(cues_tags, 2, true, true, true, false, true);
    expect_reads("opening", cues_tags, 2, 2);
    expect_tags_complete(f);

    for (int network = 0; network < 2; network++) {
        scenario = network ? "only Cues behind the clusters, network" : "only Cues behind the clusters, local";
        f = open_file(cues, 1, network, true, false, false, true);
        expect_reads("opening", NULL, 0, 0);
        read_deferred_cues(&f->demuxer);
        expect_reads("the first seek", cues, 1, 1);
        CHECK(metadata_changes == 0, "metadata changed without tags");
    }

    scenario = "Tags before the Cues, network";
    f = open_file(tags_cues, 2, true, true, false, false, true);
    expect_reads("opening", NULL, 0, 0);
    read_deferred_cues(&f->demuxer);
    expect_reads("the first seek", tags_cues, 2, 1);
    expect_tags(f, "A film");

    scenario = "chapters behind the clusters too, network";
    f = open_file(all, 3, true, true, false, false, true);
    expect_reads("opening", all, 3, 2);
    expect_tags_complete(f);

    scenario = "Tags behind the clusters, index in front, network";
    f = open_file(tags, 1, true, true, false, true, true);
    expect_reads("opening", NULL, 0, 0);
    read_deferred_cues(&f->demuxer);
    expect_reads("the first seek", NULL, 0, 0);
    expect_tags(f, NULL);

    scenario = "Tags behind the clusters, no Cues at all, network";
    f = open_file(tags, 1, true, true, false, false, true);
    expect_reads("opening", NULL, 0, 0);
    read_deferred_cues(&f->demuxer);
    expect_reads("the first seek", NULL, 0, 0);
    read_deferred_cues(&f->demuxer);
    expect_reads("the second seek", NULL, 0, 0);
    expect_tags(f, NULL);

    scenario = "Tags behind the clusters, index in front, local file";
    f = open_file(tags, 1, false, true, false, true, true);
    expect_reads("opening", tags, 1, 2);
    expect_tags_complete(f);

    scenario = "the connection for the Cues fails once, network";
    f = open_file(cues_tags, 2, true, true, false, false, true);
    failing_seeks = 1;
    read_deferred_cues(&f->demuxer);
    expect_reads("the first seek", cues_tags, 2, 1);
    CHECK(f->mkv.index_complete, "no index after one failed seek");

    scenario = "the connection for the Cues fails twice, network";
    f = open_file(cues_tags, 2, true, true, false, false, true);
    failing_seeks = 2;
    read_deferred_cues(&f->demuxer);
    expect_reads("the first seek", tags, 1, 1);
    CHECK(!f->mkv.index_complete, "an index without Cues");
    CHECK(warnings == 1, "%d warning(s) for the Cues that could not be reached", warnings);

    scenario = "a seek fails once, local file";
    failing_seeks = 1;
    f = open_file(cues_tags, 2, false, true, false, false, true);
    expect_reads("opening", tags, 1, 2);
    CHECK(failing_seeks == 0 && warnings == 1, "the failed seek was repeated");

    scenario = "stream that cannot seek";
    f = open_file(cues_tags, 2, true, true, false, false, false);
    expect_reads("opening", NULL, 0, 0);
    CHECK(warnings == 2, "%d warning(s), want one per element", warnings);
    read_deferred_cues(&f->demuxer);
    expect_reads("the first seek", NULL, 0, 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_demux_mkv_defer_tags: ok\n");
    return 0;
}
