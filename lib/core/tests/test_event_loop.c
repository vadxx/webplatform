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
    ASSERT_EQ(0, request_complete("GET / HTTP/1.1\r\n", strlen("GET / HTTP/1.1\r\n")));
}
UTEST(req, complete_single_crlf) { ASSERT_EQ(0, request_complete("a\r\nb", strlen("a\r\nb"))); }
UTEST(req, complete_at_end) {
    ASSERT_EQ(1, request_complete("GET / HTTP/1.1\r\n\r\n", strlen("GET / HTTP/1.1\r\n\r\n")));
}
UTEST(req, complete_in_middle) {
    ASSERT_EQ(1,
              request_complete("GET / HTTP/1.1\r\n\r\nbody", strlen("GET / HTTP/1.1\r\n\r\nbody")));
}
UTEST(req, complete_empty) { ASSERT_EQ(0, request_complete("", 0)); }
UTEST(req, complete_split_crlf) {
    ASSERT_EQ(0, request_complete("...\r\n\r", strlen("...\r\n\r")));
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
    loop_dispatch(-1, events, 1, pair[0] + 1, pool, 1, NULL);
    ASSERT_EQ(-1, pool[0].fd); /* old code had no EPOLLOUT branch: the slot would hang */
    close(pair[1]);
}

UTEST(dispatch, module_is_zero_heap) {
    struct iovec src[1] = {{(void *)0x10, 3}};
    struct iovec dst[1];
    struct core_conn pool[1];
    memset(pool, 0, sizeof(pool));
    pool[0].fd = -1;
    g_malloc_calls = 0;
    ASSERT_EQ(NULL, conn_find_free(pool, 0));
    ASSERT_EQ(0, request_complete("ab", strlen("ab")));
    ASSERT_EQ(1, iov_skip(src, 1, 0, dst));
    ASSERT_EQ(0, g_malloc_calls);
}

UTEST_MAIN();
