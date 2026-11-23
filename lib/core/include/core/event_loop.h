#pragma once
#include <stddef.h>
#include <stdint.h>

#include "core/plugin_api.h"

enum {
    CORE_REQ_BUF_SIZE = 4096,
    CORE_BODY_CAP = 2048, /* request body limit; larger bodies drop the conn */
};

/* State of a single connection. The caller owns the pool (static array):
 * fd == -1 marks a free slot. Responses are per-conn: concurrency-safe.
 * After the headers arrive, Content-Length bytes of body accumulate in
 * body[] and only then the handler runs (conn->body/body_len). */
struct core_conn {
    int fd;
    char req_buf[CORE_REQ_BUF_SIZE];
    size_t req_len;
    size_t body_expect; /* Content-Length of the current request */
    char body[CORE_BODY_CAP];
    size_t body_len;
    struct plugin_response_t resp;
    size_t sent_len;
};

/* Handler for a completed request (req_buf holds it up to \r\n\r\n).
 * Fills resp: head is the full HTTP response with headers, iov/iov_count
 * are the writev parts. 0 = ok, otherwise the connection is closed. */
typedef int (*core_handler_fn)(struct core_conn *conn);

/* Extra poll source: any readable fd (timerfd, signalfd, ...) joined into
 * the loop. The caller owns the array (static storage, zero heap). */
struct core_poll_source {
    int fd;
    void (*on_readable)(void *ctx);
    void *ctx;
};

/* Poll sources with their count; items may be NULL when count is 0. */
struct core_poll_set {
    const struct core_poll_source *items;
    size_t count;
};

/* The connection pool the loop serves. */
struct core_conn_pool {
    struct core_conn *conns;
    size_t count;
};

/* Non-blocking timer fd: interval >= 1 s arms a periodic tick, 0 arms a
 * one-shot that fires immediately. -1 on error. */
int core_timer_fd(int interval_sec);

/* Socket + SO_REUSEADDR + bind(INADDR_ANY) + listen; -1 on error */
int core_listen(uint16_t port);

/* Zero-heap epoll loop: blocks forever. Level-triggered, Connection: close
 * semantics. Partial writes resume on EPOLLOUT. Returns -1 only on a
 * fatal initialization failure. */
int core_loop_run(int listen_fd, const struct core_conn_pool *pool, core_handler_fn handler,
                  const struct core_poll_set *sources);
