#include "../src/event_loop.c"
#include "utest.h"
#include <string.h>

/* Allocation counter (--wrap=malloc): the loop must stay zero-heap */
extern void *__real_malloc(size_t size);

static int g_malloc_calls;

__attribute__((visibility("default"))) void *__wrap_malloc(size_t size) {
    ++g_malloc_calls;
    return __real_malloc(size);
}

/* iov fixtures: fake read-only tags instead of bare pointer literals.
 * define + #undef: a struct initializer needs a constant
 * expression, which a static const pointer is not in C. */
#define TAG_A ((void *)0x10)
#define TAG_B ((void *)0x20)
#define TAG_A_PLUS_2 ((void *)0x12)
#define TAG_B_PLUS_3 ((void *)0x23)

/* "Some occupied descriptor": its value is irrelevant, only != -1 */
#define FD_IN_USE 5

UTEST(pool, find_free_first) {
    struct core_conn conns[2];
    memset(conns, 0, sizeof(conns));
    conns[0].fd = -1;
    conns[1].fd = -1;
    ASSERT_EQ(&conns[0], conn_find_free(conns, 2));
}

UTEST(pool, find_free_skips_used) {
    struct core_conn conns[2];
    memset(conns, 0, sizeof(conns));
    conns[0].fd = FD_IN_USE;
    conns[1].fd = -1;
    ASSERT_EQ(&conns[1], conn_find_free(conns, 2));
}

UTEST(pool, find_free_full) {
    struct core_conn conns[1];
    memset(conns, 0, sizeof(conns));
    conns[0].fd = FD_IN_USE;
    ASSERT_EQ(NULL, conn_find_free(conns, 1));
}

UTEST(req, complete_none) {
    ASSERT_EQ((size_t)0, request_end("GET / HTTP/1.1\r\n", strlen("GET / HTTP/1.1\r\n")));
}
UTEST(req, complete_single_crlf) { ASSERT_EQ((size_t)0, request_end("a\r\nb", strlen("a\r\nb"))); }
UTEST(req, complete_at_end) {
    ASSERT_TRUE(request_end("GET / HTTP/1.1\r\n\r\n", strlen("GET / HTTP/1.1\r\n\r\n")));
}
UTEST(req, complete_in_middle) {
    ASSERT_TRUE(request_end("GET / HTTP/1.1\r\n\r\nbody", strlen("GET / HTTP/1.1\r\n\r\nbody")));
}
UTEST(req, complete_empty) { ASSERT_EQ((size_t)0, request_end("", 0)); }
UTEST(req, complete_split_crlf) {
    ASSERT_EQ((size_t)0, request_end("...\r\n\r", strlen("...\r\n\r")));
}

UTEST(iov, skip_none) {
    struct iovec src[2] = {{TAG_A, 3}, {TAG_B, 5}};
    struct iovec dst[2];
    ASSERT_EQ(2, iov_skip(src, 2, 0, dst));
    ASSERT_EQ(src[1].iov_base, dst[1].iov_base);
}

UTEST(iov, skip_inside_first) {
    struct iovec src[2] = {{TAG_A, 3}, {TAG_B, 5}};
    struct iovec dst[2];
    ASSERT_EQ(2, iov_skip(src, 2, 2, dst));
    ASSERT_EQ(TAG_A_PLUS_2, dst[0].iov_base);
    ASSERT_EQ((size_t)1, dst[0].iov_len);
}

UTEST(iov, skip_exactly_first) {
    struct iovec src[2] = {{TAG_A, 3}, {TAG_B, 5}};
    struct iovec dst[2];
    ASSERT_EQ(1, iov_skip(src, 2, 3, dst));
    ASSERT_EQ(TAG_B, dst[0].iov_base);
}

UTEST(iov, skip_spans_two) {
    struct iovec src[2] = {{TAG_A, 3}, {TAG_B, 5}};
    struct iovec dst[2];
    ASSERT_EQ(1, iov_skip(src, 2, 6, dst));
    ASSERT_EQ(TAG_B_PLUS_3, dst[0].iov_base);
    ASSERT_EQ((size_t)2, dst[0].iov_len);
}

UTEST(iov, skip_all) {
    struct iovec src[1] = {{TAG_A, 3}};
    struct iovec dst[1];
    ASSERT_EQ(0, iov_skip(src, 1, 3, dst));
}

#undef TAG_A
#undef TAG_B
#undef TAG_A_PLUS_2
#undef TAG_B_PLUS_3
#undef FD_IN_USE

#include <sys/socket.h>

/* Regression: EPOLLOUT must resume writes. A finished connection
 * (nothing left to send) must be released on EPOLLOUT. */
UTEST(dispatch, epollout_releases_finished_conn) {
    int pair[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    struct core_conn pool[1];
    memset(pool, 0, sizeof(pool));
    pool[0].fd = pair[0];
    pool[0].resp.total_len = 0; /* fully sent: nothing to resume */
    pool[0].resp.iov_count = 0;

    struct epoll_event events[1];
    memset(events, 0, sizeof(events));
    events[0].events = EPOLLOUT;
    events[0].data.fd = pair[0];

    /* listen_fd deliberately differs from pair[0]; the handler is not called */
    const struct core_loop loop = {
        .ep = -1,
        .listen_fd = pair[0] + 1,
        .conns = pool,
        .max_conns = 1,
    };
    loop_dispatch(&loop, events, 1);
    ASSERT_EQ(-1, pool[0].fd); /* old code had no EPOLLOUT branch: the slot would hang */
    close(pair[1]);
}

/* --- Poll sources (core_poll_source routed by loop_dispatch) --- */

static void count_tick(void *ctx) { ++*(int *)ctx; }

UTEST(source, event_fires_callback) {
    int pair[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    struct core_conn pool[1];
    memset(pool, 0, sizeof(pool));
    pool[0].fd = -1;

    struct epoll_event events[1];
    memset(events, 0, sizeof(events));
    events[0].events = EPOLLIN;
    events[0].data.fd = pair[1];

    int fired = 0;
    const struct core_poll_source sources[1] = {{pair[1], count_tick, &fired}};
    const struct core_loop loop = {
        .ep = -1,
        .listen_fd = pair[1] + 1,
        .conns = pool,
        .max_conns = 1,
        .sources = sources,
        .source_count = 1,
    };

    loop_dispatch(&loop, events, 1);
    ASSERT_EQ(1, fired);
    ASSERT_EQ(-1, pool[0].fd); /* the connection pool is untouched */
    close(pair[0]);
    close(pair[1]);
}

UTEST(source, foreign_fd_no_callback) {
    int pair[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    struct core_conn pool[1];
    memset(pool, 0, sizeof(pool));
    pool[0].fd = -1;

    struct epoll_event events[1];
    memset(events, 0, sizeof(events));
    events[0].events = EPOLLIN;
    events[0].data.fd = pair[0]; /* not the source fd */

    int fired = 0;
    const struct core_poll_source sources[1] = {{pair[1], count_tick, &fired}};
    const struct core_loop loop = {
        .ep = -1,
        .listen_fd = pair[1] + 1,
        .conns = pool,
        .max_conns = 1,
        .sources = sources,
        .source_count = 1,
    };

    loop_dispatch(&loop, events, 1);
    ASSERT_EQ(0, fired);
    close(pair[0]);
    close(pair[1]);
}

UTEST(source, empty_table_no_crash) {
    const struct core_loop loop = {0};
    loop_dispatch(&loop, NULL, 0);
}

/* --- Timer fds (core_timer_fd) --- */

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

UTEST(timer, one_shot_fires_immediately) {
    const int fd = core_timer_fd(0); /* interval 0: one-shot */
    ASSERT_GE(fd, 0);
    usleep(50000); /* 50 ms */
    uint64_t ticks = 0;
    ASSERT_EQ((ssize_t)sizeof(ticks), (ssize_t)read(fd, &ticks, sizeof(ticks)));
    close(fd);
}

UTEST(timer, periodic_not_fired_yet) {
    const int fd = core_timer_fd(1);
    ASSERT_GE(fd, 0);
    uint64_t ticks = 0;
    ASSERT_EQ(-1, (int)read(fd, &ticks, sizeof(ticks))); /* armed, silent */
    ASSERT_EQ(EAGAIN, errno);
    close(fd);
}

UTEST(timer, negative_interval_rejected) { ASSERT_EQ(-1, core_timer_fd(-1)); }

/* --- Request bodies (loop_read waits for Content-Length bytes) --- */

static char g_seen_body[128];
static size_t g_seen_body_len;
static int g_handler_calls;

static int capture_body(struct core_conn *conn) {
    ++g_handler_calls;
    g_seen_body_len =
        conn->body_len < sizeof(g_seen_body) - 1 ? conn->body_len : sizeof(g_seen_body) - 1;
    memcpy(g_seen_body, conn->body, g_seen_body_len);
    g_seen_body[g_seen_body_len] = '\0';
    return 0;
}

/* Feeds `data` into the conn's socket and dispatches one EPOLLIN event.
 * ASSERT macros only work inside UTEST bodies, so this stays silent. */
static void feed_request(int fd, struct core_conn *pool, const char *data, size_t data_len) {
    (void)write(fd, data, data_len);
    struct epoll_event events[1];
    memset(events, 0, sizeof(events));
    events[0].events = EPOLLIN;
    events[0].data.fd = pool[0].fd;
    const struct core_loop loop = {
        .ep = -1,
        .listen_fd = pool[0].fd + 1,
        .conns = pool,
        .max_conns = 1,
        .handler = capture_body,
    };
    loop_dispatch(&loop, events, 1);
}

UTEST(reqbody, arrives_with_headers) {
    int pair[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    struct core_conn pool[1];
    memset(pool, 0, sizeof(pool));
    pool[0].fd = pair[0];
    g_handler_calls = 0;
    g_seen_body_len = 99;
    feed_request(pair[1], pool, "POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello",
                 strlen("POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello"));
    ASSERT_EQ(1, g_handler_calls);
    ASSERT_EQ((size_t)5, g_seen_body_len);
    ASSERT_EQ(0, strcmp("hello", g_seen_body));
    close(pair[1]);
    close(pair[0]);
}

UTEST(reqbody, split_reads_wait_for_full_body) {
    int pair[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    struct core_conn pool[1];
    memset(pool, 0, sizeof(pool));
    pool[0].fd = pair[0];
    g_handler_calls = 0;
    const char first[] = "POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\nhe";
    const char rest[] = "llo";
    feed_request(pair[1], pool, first, strlen(first));
    ASSERT_EQ(0, g_handler_calls);  /* body incomplete: no handler call */
    ASSERT_EQ(pair[0], pool[0].fd); /* connection still alive */
    feed_request(pair[1], pool, rest, strlen(rest));
    ASSERT_EQ(1, g_handler_calls);
    ASSERT_EQ((size_t)5, g_seen_body_len);
    ASSERT_EQ(0, strcmp("hello", g_seen_body));
    close(pair[1]);
    close(pair[0]);
}

UTEST(reqbody, oversized_body_drops_connection) {
    int pair[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    struct core_conn pool[1];
    memset(pool, 0, sizeof(pool));
    pool[0].fd = pair[0];
    g_handler_calls = 0;
    const char req[] = "POST /x HTTP/1.1\r\nContent-Length: 999999\r\n\r\n";
    feed_request(pair[1], pool, req, strlen(req));
    ASSERT_EQ(0, g_handler_calls);
    ASSERT_EQ(-1, pool[0].fd); /* dropped without a response */
    close(pair[1]);
    close(pair[0]);
}

UTEST(dispatch, module_is_zero_heap) {
    struct iovec src[1] = {{(void *)0x10, 3}};
    struct iovec dst[1];
    struct core_conn pool[1];
    memset(pool, 0, sizeof(pool));
    pool[0].fd = -1;
    g_malloc_calls = 0;
    ASSERT_EQ(NULL, conn_find_free(pool, 0));
    ASSERT_EQ((size_t)0, request_end("ab", strlen("ab")));
    ASSERT_EQ(1, iov_skip(src, 1, 0, dst));
    ASSERT_EQ(0, g_malloc_calls);
}

UTEST_MAIN();
