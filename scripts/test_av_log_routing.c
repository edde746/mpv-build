// Behavioral regression for patch 0209, extracted by test_av_log_routing.sh
// from common/av_log.c: the instance list, init_libav(), uninit_libav(),
// mp_av_log_set_thread() and the av_log callback they install.
//
// What must hold: with two mpv instances alive at once, an FFmpeg line logged
// on a thread of one of them reaches that instance and no other, before and
// after the other one is gone; a line from a thread no instance owns goes to
// the oldest instance alive; the callback stays installed while any instance
// is and gives way to FFmpeg's default after the last; a partial line (no
// newline yet) of one instance doesn't take the prefix off the next line of
// another. Before 0209 the first instance took every line, and once it was
// destroyed nobody did: Tortuga's next player, created while the last one
// still terminated, never saw "http: HTTP error 404 Not Found" for a link it
// could not open.
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HAVE_LIBAVDEVICE 0

// --- mpv's environment: logs record what reaches them.

enum { MSGL_FATAL, MSGL_ERR, MSGL_WARN, MSGL_INFO, MSGL_STATUS, MSGL_V, MSGL_DEBUG, MSGL_TRACE };

struct mp_log {
    const char *name;
    struct mp_log *parent;
};

struct mpv_global {
    struct mp_log *log;
};

typedef struct bstr {
    unsigned char *start;
    size_t len;
} bstr;
#define BSTR_P(bs) (int)((bs).len), ((bs).start ? (char *)(bs).start : "")

typedef pthread_mutex_t mp_static_mutex;
#define MP_STATIC_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define mp_mutex_lock pthread_mutex_lock
#define mp_mutex_unlock pthread_mutex_unlock
#define thread_local _Thread_local

// talloc: the instance list's growth and removal, and frees; a log or a
// buffer an instance owned is leaked with it, which a test can afford.
#define talloc_zero(ctx, type) ((type *)calloc(1, sizeof(type)))
static void talloc_free(void *p) { free(p); }
#define TA_FREEP(pp) do { free(*(pp)); *(pp) = NULL; } while (0)
#define MP_TARRAY_APPEND(ctx, p, idxvar, ...) do { \
        (p) = realloc((p), sizeof(*(p)) * ((idxvar) + 1)); \
        (p)[(idxvar)++] = (__VA_ARGS__); \
    } while (0)
#define MP_TARRAY_REMOVE_AT(p, idxvar, at) do { \
        memmove(&(p)[at], &(p)[(at) + 1], sizeof(*(p)) * ((idxvar) - (at) - 1)); \
        (idxvar)--; \
    } while (0)

static struct mp_log *mp_log_new(void *ctx, struct mp_log *parent, const char *name)
{
    struct mp_log *log = calloc(1, sizeof(*log));
    log->name = name;
    log->parent = parent;
    return log;
}

static int bstr_xappend_vasprintf(void *ctx, bstr *s, const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int size = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    s->start = realloc(s->start, s->len + size + 1);
    vsnprintf((char *)s->start + s->len, size + 1, fmt, ap);
    s->len += size;
    return size;
}

struct line {
    struct mp_log *log;
    char text[256];
};
static struct line lines[64];
static int num_lines;

static bool mp_msg_test(struct mp_log *log, int lev) { return true; }

static void mp_msg(struct mp_log *log, int lev, const char *fmt, ...)
{
    if (num_lines == 64)
        abort();
    struct line *l = &lines[num_lines++];
    l->log = log;
    va_list va;
    va_start(va, fmt);
    vsnprintf(l->text, sizeof(l->text), fmt, va);
    va_end(va);
}
#define mp_warn(log, ...) mp_msg(log, MSGL_WARN, __VA_ARGS__)

// --- FFmpeg: the callback slot, and the context an http URLContext logs as.

enum { AV_LOG_FATAL = 8, AV_LOG_ERROR = 16, AV_LOG_WARNING = 24, AV_LOG_INFO = 32, AV_LOG_VERBOSE = 40 };
enum AVMediaType { AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO };

typedef struct AVClass {
    const char *class_name;
    const char *(*item_name)(void *ctx);
} AVClass;
typedef struct AVCodec { enum AVMediaType type; } AVCodec;
typedef struct AVCodecContext { const AVClass *av_class; const AVCodec *codec; } AVCodecContext;
typedef struct AVFormatContext { const AVClass *av_class; const void *iformat; } AVFormatContext;

static int av_codec_is_decoder(const AVCodec *codec) { return 1; }
static const char *av_default_item_name(void *ctx) { return "default"; }
static int avformat_network_init(void) { return 0; }

typedef void (*log_callback)(void *, int, const char *, va_list);
static void av_log_default_callback(void *ptr, int level, const char *fmt, va_list vl) {}
static log_callback installed = av_log_default_callback;
static void av_log_set_callback(log_callback cb) { installed = cb; }

static const char *http_name(void *ctx) { return "http"; }
static const AVClass url_class = {.class_name = "URLContext", .item_name = http_name};
static struct { const AVClass *av_class; } http = {&url_class};

#include "av_log_routing.inc"

// --- harness

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static void av_log(void *ptr, int level, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    installed(ptr, level, fmt, va);
    va_end(va);
}

struct job {
    struct mpv_global *owner; // NULL: a thread no instance owns
    const char *text;
};

static void *log_on_thread(void *arg)
{
    struct job *job = arg;
    if (job->owner)
        mp_av_log_set_thread(job->owner);
    av_log(&http, AV_LOG_WARNING, "%s", job->text);
    return NULL;
}

// Logs one line on a new thread of `owner` and returns the global whose log
// it reached, or NULL when no mpv log got it.
static struct mpv_global *globals[2];
static struct mpv_global *log_from(struct mpv_global *owner, const char *text)
{
    int before = num_lines;
    struct job job = {owner, text};
    pthread_t thread;
    if (pthread_create(&thread, NULL, log_on_thread, &job) || pthread_join(thread, NULL))
        abort();
    if (num_lines == before)
        return NULL;
    struct mp_log *log = lines[num_lines - 1].log;
    while (log->parent)
        log = log->parent;
    for (int n = 0; n < 2; n++) {
        if (globals[n]->log == log)
            return globals[n];
    }
    return NULL;
}

static const char *name(struct mpv_global *global)
{
    return global ? global->log->name : "nobody";
}

static struct mpv_global *new_global(const char *log_name)
{
    struct mpv_global *global = calloc(1, sizeof(*global));
    global->log = mp_log_new(NULL, NULL, log_name);
    return global;
}

int main(void)
{
    struct mpv_global *a = globals[0] = new_global("A");
    struct mpv_global *b = globals[1] = new_global("B");
    struct mpv_global *got;

    init_libav(a);
    CHECK(installed == mp_msg_av_log_callback, "the first instance did not install the callback");

    // B starts while A is still alive (A's player terminating). Before 0209
    // B's 404 went to A.
    init_libav(b);
    got = log_from(b, "HTTP error 404 Not Found\n");
    CHECK(got == b, "a line logged on B's thread with A alive reached %s, not B", name(got));
    CHECK(num_lines && !strcmp(lines[num_lines - 1].text, "http: HTTP error 404 Not Found\n"),
          "B's line read \"%s\", not \"http: HTTP error 404 Not Found\\n\"",
          num_lines ? lines[num_lines - 1].text : "");
    got = log_from(a, "HTTP error 403 Forbidden\n");
    CHECK(got == a, "a line logged on A's thread with B alive reached %s, not A", name(got));
    got = log_from(NULL, "a line from FFmpeg's own thread\n");
    CHECK(got == a, "a line from a thread no instance owns reached %s, not the oldest (A)",
          name(got));

    // A partial line of A doesn't take the prefix off B's next line, and A's
    // continuation carries none.
    got = log_from(a, "first half, ");
    CHECK(got == a, "A's partial line reached %s", name(got));
    got = log_from(b, "HTTP error 410 Gone\n");
    CHECK(got == b && !strcmp(lines[num_lines - 1].text, "http: HTTP error 410 Gone\n"),
          "B's line after A's partial one reached %s as \"%s\"", name(got), lines[num_lines - 1].text);
    got = log_from(a, "second half\n");
    CHECK(got == a && !strcmp(lines[num_lines - 1].text, "second half\n"),
          "A's continuation reached %s as \"%s\", not \"second half\\n\"", name(got),
          lines[num_lines - 1].text);

    // A is gone. Before 0209 FFmpeg's default callback took B's lines from
    // here on.
    uninit_libav(a);
    CHECK(installed == mp_msg_av_log_callback,
          "destroying A took the callback away while B is alive");
    got = log_from(b, "HTTP error 404 Not Found\n");
    CHECK(got == b, "a line logged on B's thread after A was destroyed reached %s, not B",
          name(got));
    got = log_from(NULL, "a line from FFmpeg's own thread\n");
    CHECK(got == b, "a line from a thread no instance owns reached %s, not the only one (B)",
          name(got));
    got = log_from(a, "a line from a thread of the destroyed A\n");
    CHECK(got == b, "a line from a thread of the destroyed A reached %s, not the oldest (B)",
          name(got));

    uninit_libav(b);
    CHECK(installed == av_log_default_callback,
          "the callback stayed installed after the last instance was destroyed");

    // The next instance starts from an empty list.
    init_libav(a);
    got = log_from(a, "HTTP error 404 Not Found\n");
    CHECK(got == a, "an instance created after all were destroyed got its line at %s", name(got));
    uninit_libav(a);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("av_log routing harness: ok\n");
    return 0;
}
