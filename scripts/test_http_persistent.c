// Behavioral regression for ffmpeg patches 0080 (persistent HTTP range
// requests, backported) and 0081 (when a connection may be reused), linked by
// test_http_persistent.sh against a minimal host build of the patched FFmpeg:
// the real libavformat http and tcp protocols, driven through avio against
// HTTP/1.1 servers on loopback that this file runs.
//
// mpv's stream layer reads a media file the way walk() does: the headers at
// the start, the index at the end, then a cluster in the middle, later a seek
// backwards and one to the very end. n8.0.1 opened a connection for each of
// those seeks. With the backport a seek reuses the connection when the reply
// in progress ends within short_seek_size bytes, which a bounded request
// (initial_request_size) makes the common case while a file is opened.
//
// What must hold:
//   - every byte read is the byte the resource has at that offset, in every
//     profile and against every kind of server below;
//   - without the new options nothing changes on the wire: one connection
//     per seek, "Connection: close", an open-ended Range;
//   - with them, the walk takes the connections and requests listed in
//     main(), no more;
//   - a server that ignores Range, answers 200, closes after every reply
//     (announced or not), sends chunked bodies, redirects, cuts a reply short,
//     stalls, caps the size of a range or does not know the total size is read
//     correctly, and a reply whose Content-Range ends before it starts is an
//     error instead of an endless loop;
//   - a connection is not given another request (0081) while a chunked reply
//     is unfinished on it, when it belongs to the target of a redirect that
//     is not remembered, after it sat idle for 4 seconds, or when the rest of
//     its reply stops arriving: the seek then connects anew at once instead
//     of reading chunk framing as a reply, asking the wrong server, or
//     waiting for the timeout.
#define _GNU_SOURCE // memmem
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>

#define SIZE (64ull * 1024 * 1024 + 4321)
#define MiB (1024 * 1024)

static int failures;

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    failures++;
}

static uint8_t resource_byte(uint64_t i)
{
    return (uint8_t)((i * 2654435761u) >> 11 ^ (i >> 24));
}

// ---------------------------------------------------------------- server

#define MAX_LOG 128
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
// The second server is where /xredirect sends the client. Asked for
// /xredirect itself, it serves other bytes: a request on the wrong connection.
static int port, port_b, nb_conns, nb_reqs, nb_close_headers, nb_open_ended;
static int nb_misdirected, nb_into_idle;
static char req_log[MAX_LOG][96];
static int once_drop, once_stall, once_half;

static void reset_counters(void)
{
    pthread_mutex_lock(&lock);
    nb_conns = nb_reqs = nb_close_headers = nb_open_ended = 0;
    nb_misdirected = nb_into_idle = 0;
    once_drop = once_stall = once_half = 0;
    pthread_mutex_unlock(&lock);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static int send_str(int fd, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return send_all(fd, buf, n);
}

// Returns the body bytes sent, or -1 when the connection is gone.
static int64_t send_body(int fd, uint64_t start, uint64_t len, int chunked, int64_t cut, int stall_ms,
                         uint8_t flip)
{
    static _Thread_local uint8_t buf[32768];
    uint64_t sent = 0;
    while (sent < len) {
        size_t n = len - sent < sizeof(buf) ? len - sent : sizeof(buf);
        if (cut >= 0 && sent < (uint64_t)cut && sent + n > (uint64_t)cut)
            n = cut - sent;
        for (size_t i = 0; i < n; i++)
            buf[i] = resource_byte(start + sent + i) ^ flip;
        if (chunked && send_str(fd, "%zx\r\n", n) < 0)
            return -1;
        if (send_all(fd, buf, n) < 0)
            return -1;
        if (chunked && send_all(fd, "\r\n", 2) < 0)
            return -1;
        sent += n;
        if (cut >= 0 && sent == (uint64_t)cut && sent < len) {
            if (!stall_ms)
                return sent;        // cut the reply short
            usleep(stall_ms * 1000);
            cut = -1;
        }
    }
    if (chunked && send_all(fd, "0\r\n\r\n", 5) < 0)
        return -1;
    return sent;
}

static void *serve(void *arg)
{
    int fd = (int)((intptr_t)arg >> 1), second = (intptr_t)arg & 1;
    char head[4096];
    size_t have = 0;
    int nosig = 1;
    int64_t last_reply = now_ms();
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nosig, sizeof(nosig));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif
    for (;;) {
        char *end;
        while (!(end = memmem(head, have, "\r\n\r\n", 4))) {
            ssize_t n = have < sizeof(head) - 1 ? recv(fd, head + have, sizeof(head) - 1 - have, 0) : 0;
            if (n <= 0)
                goto done;
            have += n;
        }
        *end = 0;
        char method[8] = "", path[64] = "";
        sscanf(head, "%7s %63s", method, path);
        uint64_t start = 0, last = SIZE - 1;
        int ranged = 0, open_ended = 0;
        char *r = strstr(head, "\r\nRange: bytes=");
        if (r) {
            char *p;
            start = strtoull(r + 15, &p, 10);
            ranged = 1;
            if (*p == '-' && p[1] >= '0' && p[1] <= '9')
                last = strtoull(p + 1, NULL, 10);
            else
                open_ended = 1;
            if (last > SIZE - 1)
                last = SIZE - 1;
        }
        int wants_close = strstr(head, "\r\nConnection: close") != NULL;

        pthread_mutex_lock(&lock);
        if (nb_reqs < MAX_LOG) {
            char *eol = r ? strstr(r + 2, "\r\n") : NULL;
            snprintf(req_log[nb_reqs], sizeof(req_log[0]), "%s %s %.*s", method, path,
                     r ? (int)((eol ? eol : r + strlen(r)) - (r + 9)) : 1, r ? r + 9 : "-");
        }
        nb_reqs++;
        nb_close_headers += wants_close;
        nb_open_ended += open_ended;
        int drop = 0, stall = 0, half = 0;
        if (!strcmp(path, "/drop") && !once_drop)
            drop = once_drop = 1;
        if (!strcmp(path, "/stall") && !once_stall)
            stall = once_stall = 1;
        if (!strcmp(path, "/halfstall") && !once_half)
            half = once_half = 1;
        // A connection left idle is gone without a word, as behind a NAT that
        // forgot it: nothing sent into it is ever answered.
        int into_idle = !strcmp(path, "/idle") && now_ms() - last_reply > 2000;
        nb_into_idle += into_idle;
        int misdirected = second && !strcmp(path, "/xredirect");
        nb_misdirected += misdirected;
        pthread_mutex_unlock(&lock);
        if (into_idle) {
            sleep(30);
            goto done;
        }

        have -= end + 4 - head;
        memmove(head, end + 4, have);

        int close_after = wants_close, announce_close = wants_close, chunked = 0;
        if (!strcmp(path, "/xredirect") && !second) {
            if (send_str(fd, "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:%d/plain\r\n"
                         "Content-Length: 0\r\n\r\n", port_b) < 0)
                goto done;
            continue;
        }
        if (!strcmp(path, "/redirect302") || !strcmp(path, "/redirect301")) {
            if (send_str(fd, "HTTP/1.1 %s\r\nLocation: /plain\r\nContent-Length: 0\r\n\r\n",
                         path[11] == '2' ? "302 Found" : "301 Moved Permanently") < 0)
                goto done;
            continue;
        }
        if (!strcmp(path, "/norange")) {
            ranged = 0;
            start = 0;
            last = SIZE - 1;
        } else if (!strcmp(path, "/close")) {
            close_after = announce_close = 1;
        } else if (!strcmp(path, "/silentclose")) {
            close_after = 1;
        } else if (!strcmp(path, "/chunked")) {
            chunked = 1;
        } else if (!strcmp(path, "/clamp")) {
            if (last - start + 1 > 65536)
                last = start + 65535;
        }
        if (ranged && start >= SIZE) {
            if (send_str(fd, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%" PRIu64
                         "\r\nContent-Length: 0\r\n\r\n", (uint64_t)SIZE) < 0)
                goto done;
            continue;
        }
        uint64_t len = last - start + 1;
        char total[32];
        snprintf(total, sizeof(total), "%" PRIu64, (uint64_t)SIZE);
        if (!strcmp(path, "/star"))
            strcpy(total, "*");
        char hdr[512];
        int n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 %s\r\nContent-Type: application/octet-stream\r\n",
                         ranged ? "206 Partial Content" : "200 OK");
        if (strcmp(path, "/norange"))
            n += snprintf(hdr + n, sizeof(hdr) - n, "Accept-Ranges: bytes\r\n");
        if (chunked)
            n += snprintf(hdr + n, sizeof(hdr) - n, "Transfer-Encoding: chunked\r\n");
        else
            n += snprintf(hdr + n, sizeof(hdr) - n, "Content-Length: %" PRIu64 "\r\n", len);
        if (ranged && !strcmp(path, "/badrange"))
            n += snprintf(hdr + n, sizeof(hdr) - n, "Content-Range: bytes %" PRIu64 "-%" PRIu64 "/%s\r\n",
                          start + 10, start, total);
        else if (ranged)
            n += snprintf(hdr + n, sizeof(hdr) - n, "Content-Range: bytes %" PRIu64 "-%" PRIu64 "/%s\r\n",
                          start, last, total);
        if (announce_close)
            n += snprintf(hdr + n, sizeof(hdr) - n, "Connection: close\r\n");
        n += snprintf(hdr + n, sizeof(hdr) - n, "\r\n");
        if (send_all(fd, hdr, n) < 0)
            goto done;
        if (strcmp(method, "HEAD")) {
            // /halfstall: the first 300000 bytes arrive, the rest never does.
            int64_t sent = send_body(fd, start, len, chunked,
                                     half ? 300000 : drop || stall ? 200000 : -1,
                                     half ? 30000 : stall ? 600 : 0, misdirected ? 0xff : 0);
            if (sent < 0 || (uint64_t)sent < len)
                goto done;
        }
        last_reply = now_ms();
        if (close_after)
            goto done;
    }
done:
    close(fd);
    return NULL;
}

static void *acceptor(void *arg)
{
    int lfd = (int)((intptr_t)arg >> 1), second = (intptr_t)arg & 1;
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0)
            continue;
        pthread_mutex_lock(&lock);
        nb_conns++;
        pthread_mutex_unlock(&lock);
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&t, &attr, serve, (void *)(((intptr_t)fd << 1) | second));
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

static int start_server(int second)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t len = sizeof(addr);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(lfd, 64) < 0 ||
        getsockname(lfd, (struct sockaddr *)&addr, &len) < 0) {
        perror("server");
        exit(2);
    }
    pthread_t t;
    pthread_create(&t, NULL, acceptor, (void *)(((intptr_t)lfd << 1) | second));
    return ntohs(addr.sin_port);
}

// ---------------------------------------------------------------- client

// What mpv's stream_lavf sets on every open, then the profile under test.
#define MPV "reconnect=1:reconnect_delay_max=7:icy=1:user_agent=libmpv:timeout=60000000"
#define LEGACY MPV
#define KEEPALIVE MPV ":multiple_requests=1:short_seek_size=262144"
#define BOUNDED MPV ":multiple_requests=1:initial_request_size=1048576:short_seek_size=1048576"

static AVIOContext *open_path(const char *name, const char *path, const char *opts, int expect)
{
    char url[128];
    AVDictionary *dict = NULL;
    AVIOContext *io = NULL;
    snprintf(url, sizeof(url), "http://127.0.0.1:%d%s", port, path);
    av_dict_parse_string(&dict, opts, "=", ":", 0);
    int ret = avio_open2(&io, url, AVIO_FLAG_READ, NULL, &dict);
    if (av_dict_count(dict))
        fail("%s: option not taken: %s", name, av_dict_iterate(dict, NULL)->key);
    av_dict_free(&dict);
    if ((ret < 0) != (expect < 0))
        fail("%s: open returned %s", name, ret < 0 ? av_err2str(ret) : "success");
    return ret < 0 ? NULL : io;
}

// Read len bytes at the current position and compare them with the resource.
static void read_check(const char *name, AVIOContext *io, uint64_t off, int len)
{
    static uint8_t buf[65536];
    int done = 0;
    while (done < len) {
        int want = len - done < (int)sizeof(buf) ? len - done : (int)sizeof(buf);
        int n = avio_read(io, buf, want);
        if (n <= 0) {
            fail("%s: read at %" PRIu64 " returned %s", name, off + done, n ? av_err2str(n) : "0");
            return;
        }
        for (int i = 0; i < n; i++) {
            if (buf[i] != resource_byte(off + done + i)) {
                fail("%s: wrong byte at %" PRIu64, name, off + done + i);
                return;
            }
        }
        done += n;
    }
}

static void expect_eof(const char *name, AVIOContext *io, const char *where)
{
    uint8_t b;
    int n = avio_read(io, &b, 1);
    if (n != AVERROR_EOF)
        fail("%s: %s: read returned %d (%s), want EOF", name, where, n, n < 0 ? av_err2str(n) : "data");
}

static void seek_to(const char *name, AVIOContext *io, uint64_t off)
{
    int64_t r = avio_seek(io, off, SEEK_SET);
    if (r != (int64_t)off)
        fail("%s: seek to %" PRIu64 " returned %" PRId64, name, off, r);
}

static void expect_counts(const char *name, int conns, int reqs)
{
    pthread_mutex_lock(&lock);
    if (nb_conns != conns || nb_reqs != reqs) {
        fail("%s: %d connection(s), %d request(s); want %d, %d", name, nb_conns, nb_reqs, conns, reqs);
        for (int i = 0; i < nb_reqs && i < MAX_LOG; i++)
            printf("      %s\n", req_log[i]);
    }
    pthread_mutex_unlock(&lock);
}

// The access pattern of an open, a resume and two seeks; returns nothing, every
// read is checked. step, when not NULL, receives the requests seen so far
// after each of the 7 steps.
static void walk(const char *name, AVIOContext *io)
{
    if (avio_size(io) != (int64_t)SIZE)
        fail("%s: size %" PRId64, name, avio_size(io));
    read_check(name, io, 0, 300000);                    // headers, first packets
    seek_to(name, io, SIZE - 90000);                    // the index at the end
    read_check(name, io, SIZE - 90000, 90000);
    expect_eof(name, io, "end of file");
    seek_to(name, io, 20 * MiB + 7);                    // resume position
    read_check(name, io, 20 * MiB + 7, 400000);
    seek_to(name, io, 100000);                          // seek backwards
    read_check(name, io, 100000, 50000);
    seek_to(name, io, SIZE - 1);                        // last byte
    read_check(name, io, SIZE - 1, 1);
    expect_eof(name, io, "after the last byte");
    seek_to(name, io, SIZE);                            // at the end: no request
    expect_eof(name, io, "seek to the end");
    seek_to(name, io, 40 * MiB);                        // and back into the file
    read_check(name, io, 40 * MiB, 70000);
}

static void run_walk(const char *name, const char *path, const char *opts, int conns, int reqs)
{
    reset_counters();
    AVIOContext *io = open_path(name, path, opts, 0);
    if (!io)
        return;
    if (!(io->seekable & AVIO_SEEKABLE_NORMAL))
        fail("%s: not seekable", name);
    walk(name, io);
    avio_closep(&io);
    if (conns >= 0)
        expect_counts(name, conns, reqs);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    av_log_set_level(AV_LOG_FATAL);
    avformat_network_init();
    port = start_server(0);
    port_b = start_server(1);

    // The wire as n8.0.1 left it: a connection per seek (the seek to the very
    // end sends nothing), every request open-ended and "Connection: close".
    run_walk("legacy", "/plain", LEGACY, 6, 6);
    if (nb_close_headers != 6 || nb_open_ended != 6)
        fail("legacy: %d of 6 requests say close, %d are open-ended", nb_close_headers, nb_open_ended);

    // Keep-alive alone: only a reply read to its end (the index at the end of
    // the file, the last byte) hands its connection to the next request.
    run_walk("keepalive", "/plain", KEEPALIVE, 4, 6);
    if (nb_close_headers)
        fail("keepalive: %d request(s) say close", nb_close_headers);

    // Bounded requests: every seek finds at most 1 MiB left to drain, so the
    // walk stays on one connection.
    run_walk("bounded", "/plain", BOUNDED, 1, 6);
    if (nb_open_ended)
        fail("bounded: %d open-ended request(s) before the first range was read out", nb_open_ended);

    // Reading on past a bounded range asks for the rest on the same
    // connection, once, open-ended.
    reset_counters();
    AVIOContext *io = open_path("continue", "/plain", BOUNDED, 0);
    if (io) {
        read_check("continue", io, 0, 3 * MiB);
        avio_closep(&io);
        expect_counts("continue", 1, 2);
        if (nb_open_ended != 1)
            fail("continue: %d open-ended request(s), want 1", nb_open_ended);
    }

    // A server without Range: one 200 reply carries the whole file whatever
    // was asked for, and the stream is not seekable.
    for (int i = 0; i < 2; i++) {
        const char *name = i ? "norange bounded" : "norange legacy";
        reset_counters();
        io = open_path(name, "/norange", i ? BOUNDED : LEGACY, 0);
        if (!io)
            continue;
        if (io->seekable & AVIO_SEEKABLE_NORMAL)
            fail("%s: seekable", name);
        read_check(name, io, 0, 3 * MiB);
        avio_closep(&io);
        expect_counts(name, 1, 1);
    }

    // Servers that close after every reply, announced or not: a connection
    // per request, and the request sent into the closed one is repeated.
    run_walk("close legacy", "/close", LEGACY, 6, 6);
    run_walk("close bounded", "/close", BOUNDED, 6, 6);
    run_walk("silentclose keepalive", "/silentclose", KEEPALIVE, 6, 6);
    run_walk("silentclose bounded", "/silentclose", BOUNDED, 6, 6);
    reset_counters();
    io = open_path("silentclose continue", "/silentclose", BOUNDED, 0);
    if (io) {
        read_check("silentclose continue", io, 0, 3 * MiB);
        avio_closep(&io);
        expect_counts("silentclose continue", 2, 2);
    }

    // Chunked bodies. The bytes left of a chunked reply are not what is left
    // on the wire, so only one read to its last chunk hands its connection on:
    // the same connections as keep-alive alone, and no request sent twice.
    run_walk("chunked legacy", "/chunked", LEGACY, 6, 6);
    run_walk("chunked keepalive", "/chunked", KEEPALIVE, 4, 6);
    run_walk("chunked bounded", "/chunked", BOUNDED, 4, 6);
    // A seek to the start of the file out of an unfinished chunked reply must
    // not take the rest of that reply for the new one.
    for (int i = 0; i < 2; i++) {
        const char *name = i ? "chunked rewind bounded" : "chunked rewind keepalive";
        reset_counters();
        io = open_path(name, "/chunked", i ? BOUNDED : KEEPALIVE, 0);
        if (!io)
            continue;
        read_check(name, io, 0, 300000);
        seek_to(name, io, 0);
        read_check(name, io, 0, 500000);
        avio_closep(&io);
        expect_counts(name, 2, 2);
    }
    reset_counters();
    io = open_path("chunked continue", "/chunked", BOUNDED, 0);
    if (io) {
        read_check("chunked continue", io, 0, 3 * MiB);
        avio_closep(&io);
        expect_counts("chunked continue", 1, 2);
    }

    // Redirects: a temporary one is asked again by every request, a permanent
    // one is remembered. Either way the data is the target's.
    run_walk("redirect302 legacy", "/redirect302", LEGACY, 12, 12);
    run_walk("redirect302 bounded", "/redirect302", BOUNDED, 12, 12);
    run_walk("redirect301 legacy", "/redirect301", LEGACY, 7, 7);
    run_walk("redirect301 bounded", "/redirect301", BOUNDED, 2, 7);
    if (nb_open_ended)
        fail("redirect301 bounded: %d open-ended request(s) behind the redirect", nb_open_ended);
    // A redirect to another server: the request for the original address
    // never goes out on the connection to the target.
    run_walk("xredirect legacy", "/xredirect", LEGACY, 12, 12);
    run_walk("xredirect bounded", "/xredirect", BOUNDED, 12, 12);
    if (nb_misdirected)
        fail("xredirect bounded: %d request(s) for the first server reached the second", nb_misdirected);

    // A reply cut short and one that stalls past the timeout: reconnect=1
    // resumes at the offset.
    run_walk("drop legacy", "/drop", LEGACY, 7, 7);
    run_walk("drop bounded", "/drop", BOUNDED, 2, 7);
    run_walk("stall bounded", "/stall", MPV ":multiple_requests=1:initial_request_size=1048576:"
             "short_seek_size=1048576:timeout=200000", 2, 7);

    // The rest of a reply that stops arriving is not waited for: the seek
    // gives up draining and connects anew long before the 5 s timeout.
    reset_counters();
    io = open_path("halfstall", "/halfstall", BOUNDED ":timeout=5000000", 0);
    if (io) {
        read_check("halfstall", io, 0, 200000);
        int64_t t0 = now_ms();
        seek_to("halfstall", io, 20 * MiB);
        read_check("halfstall", io, 20 * MiB, 100000);
        if (now_ms() - t0 > 1500)
            fail("halfstall: the seek took %d ms", (int)(now_ms() - t0));
        avio_closep(&io);
        expect_counts("halfstall", 2, 2);
    }

    // A connection that sat idle is not asked again, by a seek or by reading
    // on: the server here never answers a request that arrives on one. Both
    // streams have read their first range to its last byte, so nothing is
    // left to drain and only the idle time speaks against the connection.
    reset_counters();
    io = open_path("idle seek", "/idle", BOUNDED ":timeout=5000000", 0);
    AVIOContext *io2 = open_path("idle read", "/idle", BOUNDED ":timeout=5000000", 0);
    if (io && io2) {
        read_check("idle seek", io, 0, 1 * MiB);
        read_check("idle read", io2, 0, 1 * MiB);
        usleep(4300000);
        int64_t t0 = now_ms();
        seek_to("idle seek", io, 20 * MiB);
        read_check("idle seek", io, 20 * MiB, 100000);
        read_check("idle read", io2, 1 * MiB, 100000);
        if (now_ms() - t0 > 1500)
            fail("idle: a seek and a read took %d ms", (int)(now_ms() - t0));
        expect_counts("idle", 4, 4);
        if (nb_into_idle)
            fail("idle: %d request(s) sent into an idle connection", nb_into_idle);
    }
    avio_closep(&io);
    avio_closep(&io2);

    // A server that caps every range at 64 KiB is read range by range.
    for (int i = 0; i < 2; i++) {
        const char *name = i ? "clamp bounded" : "clamp legacy";
        reset_counters();
        io = open_path(name, "/clamp", i ? BOUNDED : LEGACY, 0);
        if (!io)
            continue;
        read_check(name, io, 0, 1 * MiB);
        seek_to(name, io, 30 * MiB + 1);
        read_check(name, io, 30 * MiB + 1, 200000);
        avio_closep(&io);
    }

    // "bytes A-B/*": the total size is unknown, the data is still read and a
    // read past the end of the resource ends with EOF.
    reset_counters();
    io = open_path("star", "/star", BOUNDED, 0);
    if (io) {
        read_check("star", io, 0, 2 * MiB);
        seek_to("star", io, SIZE - 5000);
        read_check("star", io, SIZE - 5000, 5000);
        expect_eof("star", io, "end of a resource of unknown size");
        avio_closep(&io);
    }

    // A Content-Range that ends before it starts is refused.
    io = open_path("badrange", "/badrange", BOUNDED, -1);
    avio_closep(&io);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_http_persistent: ok\n");
    return 0;
}
