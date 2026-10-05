// Behavioral regression for tcp.c's per-family hostname lookup (patch 0029),
// extracted by test_tcp_family_lookup.sh.
//
// With one AF_UNSPEC getaddrinfo(), a resolver that never answers AAAA held
// every connection for its full retry timeout although the A answer was in
// within milliseconds (edde746/plezy discussion #2566: 18 s to the first byte,
// 10 s more per seek). What must hold: the race starts with the family that
// answered, a family that answers later still joins it, a lookup that failed
// or is still pending never ends the open early, the caller's interrupt
// callback ends any wait, and a lookup left behind frees what it shares.
//
// getaddrinfo() and ff_connect_parallel() are scripted here; everything
// between them is the production code.
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// --- environment the extracted code compiles against.

#define HAVE_PTHREADS 1
#define HAVE_W32THREADS 0
#define HAVE_WINRT 0

#define AVERROR(e) (-(e))
#define AVERROR_EXIT (-0x54495845)
#define AV_LOG_ERROR 16
#define AV_LOG_VERBOSE 40
#define FFMIN(a, b) ((a) > (b) ? (b) : (a))

#define AVMutex pthread_mutex_t
#define AVCond pthread_cond_t
#define ff_mutex_init pthread_mutex_init
#define ff_mutex_lock pthread_mutex_lock
#define ff_mutex_unlock pthread_mutex_unlock
#define ff_mutex_destroy pthread_mutex_destroy
#define ff_cond_init pthread_cond_init
#define ff_cond_destroy pthread_cond_destroy
#define ff_cond_broadcast pthread_cond_broadcast
#define ff_cond_timedwait pthread_cond_timedwait

typedef struct AVIOInterruptCB {
    int (*callback)(void *);
    void *opaque;
} AVIOInterruptCB;

typedef struct TCPContext {
    int fd;
    int open_timeout;
} TCPContext;

typedef struct URLContext {
    void *priv_data;
    AVIOInterruptCB interrupt_callback;
    int is_streamed;
} URLContext;

static int ff_check_interrupt(AVIOInterruptCB *cb)
{
    return cb->callback ? cb->callback(cb->opaque) : 0;
}

static int64_t clock_us(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int64_t av_gettime(void) { return clock_us(CLOCK_REALTIME); }
static int64_t av_gettime_relative(void) { return clock_us(CLOCK_MONOTONIC); }

static atomic_int live_allocs;

static void *av_mallocz(size_t size)
{
    void *p = calloc(1, size);
    if (p)
        atomic_fetch_add(&live_allocs, 1);
    return p;
}

static void av_free(void *p)
{
    if (p)
        atomic_fetch_sub(&live_allocs, 1);
    free(p);
}

static size_t av_strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    return len;
}

static int logged_errors;

static void av_log(void *avcl, int level, const char *fmt, ...)
{
    (void)avcl;
    (void)fmt;
    if (level <= AV_LOG_ERROR)
        logged_errors++;
}

static int customize_fd(void *ctx, int fd, int family)
{
    (void)ctx;
    (void)fd;
    (void)family;
    return 0;
}

// --- scripted resolver: each family answers after a delay, with addresses
// or an error. AF_UNSPEC must never reach it for a hostname.

typedef struct FakeFamily {
    int delay_ms;
    int err;
    const char *addrs[3];
} FakeFamily;

static FakeFamily fake_v6, fake_v4;
static atomic_int live_results, running_lookups, family_lookups, unspec_lookups;

static struct addrinfo *fake_entry(const char *text, const char *service)
{
    struct addrinfo *ai = calloc(1, sizeof(*ai) + sizeof(struct sockaddr_storage));
    struct sockaddr_storage *ss = (struct sockaddr_storage *)(ai + 1);
    int port = atoi(service);

    ai->ai_socktype = SOCK_STREAM;
    ai->ai_addr = (struct sockaddr *)ss;
    if (strchr(text, ':')) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ss;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(port);
        inet_pton(AF_INET6, text, &sin6->sin6_addr);
        ai->ai_family = AF_INET6;
        ai->ai_addrlen = sizeof(*sin6);
    } else {
        struct sockaddr_in *sin = (struct sockaddr_in *)ss;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(port);
        inet_pton(AF_INET, text, &sin->sin_addr);
        ai->ai_family = AF_INET;
        ai->ai_addrlen = sizeof(*sin);
    }
    return ai;
}

static int fake_getaddrinfo(const char *node, const char *service,
                            const struct addrinfo *hints, struct addrinfo **res)
{
    const FakeFamily *script;
    struct addrinfo **tail = res;
    unsigned char probe[sizeof(struct in6_addr)];
    int i;

    *res = NULL;
    if (hints->ai_flags & AI_NUMERICHOST) {
        if (inet_pton(AF_INET, node, probe) != 1 && inet_pton(AF_INET6, node, probe) != 1)
            return EAI_NONAME;
        *res = fake_entry(node, service);
        atomic_fetch_add(&live_results, 1);
        return 0;
    }
    if (hints->ai_family != AF_INET6 && hints->ai_family != AF_INET) {
        atomic_fetch_add(&unspec_lookups, 1);
        return EAI_FAIL;
    }
    atomic_fetch_add(&family_lookups, 1);
    atomic_fetch_add(&running_lookups, 1);
    script = hints->ai_family == AF_INET6 ? &fake_v6 : &fake_v4;
    usleep(script->delay_ms * 1000);
    atomic_fetch_sub(&running_lookups, 1);
    if (script->err)
        return script->err;
    for (i = 0; script->addrs[i]; i++) {
        *tail = fake_entry(script->addrs[i], service);
        tail = &(*tail)->ai_next;
    }
    atomic_fetch_add(&live_results, 1);
    return 0;
}

static void fake_freeaddrinfo(struct addrinfo *ai)
{
    atomic_fetch_sub(&live_results, 1);
    while (ai) {
        struct addrinfo *next = ai->ai_next;
        free(ai);
        ai = next;
    }
}

#define getaddrinfo fake_getaddrinfo
#define freeaddrinfo fake_freeaddrinfo

// --- scripted connection race: records each list it is handed.

typedef enum ConnectMode {
    CONNECT_ANY,          // the first address connects
    CONNECT_IPV6_ONLY,    // IPv4 is blackholed: a race without IPv6 waits out its timeout
    CONNECT_IPV6_REFUSED, // IPv6 refuses at once; IPv4 connects
    CONNECT_IPV4_REFUSED, // IPv4 refuses at once; IPv6 connects
    CONNECT_NONE,         // nothing answers
} ConnectMode;

static ConnectMode connect_mode;
static char races[4][256];
static int nb_races;

static int race_has(const struct addrinfo *ai, int family)
{
    for (; ai; ai = ai->ai_next)
        if (ai->ai_family == family)
            return 1;
    return 0;
}

static int wait_out_race(URLContext *h)
{
    int64_t end = av_gettime_relative() + 3000000;

    while (av_gettime_relative() < end) {
        if (ff_check_interrupt(&h->interrupt_callback))
            return AVERROR_EXIT;
        usleep(5000);
    }
    return AVERROR(ETIMEDOUT);
}

static int ff_connect_parallel(struct addrinfo *addrs, int timeout_ms_per_address,
                               int parallel, URLContext *h, int *fd,
                               int (*customize)(void *, int, int), void *customize_ctx)
{
    char *out = races[nb_races++];
    const struct addrinfo *ai;
    int has_v6 = race_has(addrs, AF_INET6), has_v4 = race_has(addrs, AF_INET);

    (void)timeout_ms_per_address;
    (void)parallel;
    (void)customize;
    (void)customize_ctx;
    out[0] = 0;
    for (ai = addrs; ai; ai = ai->ai_next) {
        char text[INET6_ADDRSTRLEN];
        const void *addr = ai->ai_family == AF_INET6
            ? (const void *)&((const struct sockaddr_in6 *)ai->ai_addr)->sin6_addr
            : (const void *)&((const struct sockaddr_in *)ai->ai_addr)->sin_addr;
        int port = ntohs(ai->ai_family == AF_INET6
                             ? ((const struct sockaddr_in6 *)ai->ai_addr)->sin6_port
                             : ((const struct sockaddr_in *)ai->ai_addr)->sin_port);
        inet_ntop(ai->ai_family, addr, text, sizeof(text));
        snprintf(out + strlen(out), 256 - strlen(out), "%s%s/%d", out[0] ? " " : "", text, port);
    }
    switch (connect_mode) {
    case CONNECT_ANY:
        break;
    case CONNECT_IPV6_ONLY:
        if (!has_v6)
            return wait_out_race(h);
        break;
    case CONNECT_IPV6_REFUSED:
        if (!has_v4)
            return AVERROR(ECONNREFUSED);
        break;
    case CONNECT_IPV4_REFUSED:
        if (!has_v6)
            return AVERROR(ECONNREFUSED);
        break;
    case CONNECT_NONE:
        return wait_out_race(h);
    }
    *fd = 42;
    return 0;
}

#include "tcp_family_lookup.inc"

// --- test driver

static int failures;
static int64_t interrupt_at_us;

static int caller_interrupt(void *opaque)
{
    (void)opaque;
    return interrupt_at_us && av_gettime_relative() >= interrupt_at_us;
}

static void check(int condition, const char *scenario, const char *what)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s: %s\n", scenario, what);
        failures++;
    }
}

typedef struct Scenario {
    const char *name;
    const char *host;
    FakeFamily v6, v4;
    ConnectMode mode;
    int interrupt_ms;
    int want_ret;
    int max_ms;     // the open returns within this
    int min_ms;     // and not before this (a lookup it had to wait for)
    const char *want_races[3];
} Scenario;

static void run(const Scenario *sc)
{
    TCPContext s = { .fd = -1, .open_timeout = 60000000 };
    URLContext h = { .priv_data = &s, .interrupt_callback = { caller_interrupt, &s } };
    struct addrinfo hints = { 0 };
    int64_t start, waited_until;
    int ret, elapsed_ms, i, races_wanted = 0;
    char what[512];

    fake_v6 = sc->v6;
    fake_v4 = sc->v4;
    connect_mode = sc->mode;
    nb_races = 0;
    logged_errors = 0;
    atomic_store(&family_lookups, 0);
    atomic_store(&unspec_lookups, 0);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    start = av_gettime_relative();
    interrupt_at_us = sc->interrupt_ms ? start + sc->interrupt_ms * 1000LL : 0;
    ret = tcp_open_by_family(&h, sc->host, "32400", &hints);
    elapsed_ms = (int)((av_gettime_relative() - start) / 1000);
    interrupt_at_us = 0;

    snprintf(what, sizeof(what), "returned %d, want %d", ret, sc->want_ret);
    check(ret == sc->want_ret, sc->name, what);
    snprintf(what, sizeof(what), "returned after %d ms, want %d..%d ms", elapsed_ms, sc->min_ms, sc->max_ms);
    check(elapsed_ms >= sc->min_ms && elapsed_ms <= sc->max_ms, sc->name, what);
    check(!atomic_load(&unspec_lookups), sc->name, "a hostname was resolved with AF_UNSPEC");
    check(h.interrupt_callback.callback == caller_interrupt && h.interrupt_callback.opaque == &s,
          sc->name, "the caller's interrupt callback was not restored");
    if (ret == 0)
        check(s.fd == 42 && h.is_streamed, sc->name, "the connected socket was not adopted");
    if (ret == 1)
        check(!atomic_load(&family_lookups), sc->name, "a literal started per-family lookups");
    while (races_wanted < 3 && sc->want_races[races_wanted])
        races_wanted++;
    snprintf(what, sizeof(what), "%d races, want %d", nb_races, races_wanted);
    check(nb_races == races_wanted, sc->name, what);
    for (i = 0; i < nb_races && i < races_wanted; i++) {
        snprintf(what, sizeof(what), "race %d raced [%s], want [%s]", i + 1, races[i], sc->want_races[i]);
        check(!strcmp(races[i], sc->want_races[i]), sc->name, what);
    }

    // A lookup the open stopped waiting for finishes on its own and frees
    // the state it shares; nothing else may outlive the open.
    waited_until = av_gettime_relative() + 3000000;
    while ((atomic_load(&running_lookups) || atomic_load(&live_allocs) || atomic_load(&live_results)) &&
           av_gettime_relative() < waited_until)
        usleep(10000);
    snprintf(what, sizeof(what), "left %d allocations, %d resolver results, %d lookups running",
             atomic_load(&live_allocs), atomic_load(&live_results), atomic_load(&running_lookups));
    check(!atomic_load(&live_allocs) && !atomic_load(&live_results) && !atomic_load(&running_lookups),
          sc->name, what);
}

#define V6A "2001:db8::1"
#define V6B "2001:db8::2"
#define V4A "192.0.2.1"
#define V4B "192.0.2.2"
#define HANG 1500

int main(void)
{
    static const Scenario scenarios[] = {
        { "AAAA never answers: connect over IPv4 without waiting for it",
          "server.example", { HANG, 0, { V6A } }, { 0, 0, { V4A } }, CONNECT_ANY, 0,
          0, 500, 0, { V4A "/32400" } },
        { "A never answers: connect over IPv6 without waiting for it",
          "server.example", { 0, 0, { V6A } }, { HANG, 0, { V4A } }, CONNECT_ANY, 0,
          0, 500, 0, { V6A "/32400" } },
        { "both answer: one race, IPv6 first",
          "server.example", { 0, 0, { V6A, V6B } }, { 10, 0, { V4A, V4B } }, CONNECT_ANY, 0,
          0, 500, 0, { V6A "/32400 " V6B "/32400 " V4A "/32400 " V4B "/32400" } },
        { "late AAAA joins a race stuck on blackholed IPv4",
          "server.example", { 300, 0, { V6A } }, { 0, 0, { V4A } }, CONNECT_IPV6_ONLY, 0,
          0, 1000, 300, { V4A "/32400", V6A "/32400 " V4A "/32400" } },
        { "every IPv4 address refused: the late AAAA is awaited and raced alone",
          "server.example", { 300, 0, { V6A } }, { 0, 0, { V4A } }, CONNECT_IPV4_REFUSED, 0,
          0, 1000, 300, { V4A "/32400", V6A "/32400" } },
        { "every IPv6 address refused: the late A is awaited and raced alone",
          "server.example", { 0, 0, { V6A } }, { 300, 0, { V4A } }, CONNECT_IPV6_REFUSED, 0,
          0, 1000, 300, { V6A "/32400", V4A "/32400" } },
        { "a failed AAAA does not end the open while A is pending",
          "server.example", { 0, EAI_NONAME, { 0 } }, { 300, 0, { V4A } }, CONNECT_ANY, 0,
          0, 1000, 300, { V4A "/32400" } },
        { "both lookups fail: EIO and no race",
          "server.example", { 0, EAI_NONAME, { 0 } }, { 0, EAI_NONAME, { 0 } }, CONNECT_ANY, 0,
          AVERROR(EIO), 500, 0, { 0 } },
        { "refused with no late family: the race's error",
          "server.example", { 0, EAI_NONAME, { 0 } }, { 0, 0, { V4A } }, CONNECT_IPV4_REFUSED, 0,
          AVERROR(ECONNREFUSED), 500, 0, { V4A "/32400" } },
        { "interrupt while both lookups stall",
          "server.example", { HANG, 0, { V6A } }, { HANG, 0, { V4A } }, CONNECT_ANY, 150,
          AVERROR_EXIT, 500, 150, { 0 } },
        { "interrupt while the late AAAA is awaited after IPv4 was refused",
          "server.example", { HANG, 0, { V6A } }, { 0, 0, { V4A } }, CONNECT_IPV4_REFUSED, 300,
          AVERROR_EXIT, 800, 300, { V4A "/32400" } },
        { "interrupt during the race is not taken for a late family",
          "server.example", { HANG, 0, { V6A } }, { 0, 0, { V4A } }, CONNECT_NONE, 300,
          AVERROR_EXIT, 800, 300, { V4A "/32400" } },
        { "literal address: left to the AF_UNSPEC path",
          "192.0.2.7", { 0, 0, { V6A } }, { 0, 0, { V4A } }, CONNECT_ANY, 0,
          1, 100, 0, { 0 } },
    };
    size_t i;

    for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
        run(&scenarios[i]);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("tcp family lookup: %zu scenarios passed\n", sizeof(scenarios) / sizeof(scenarios[0]));
    return 0;
}
