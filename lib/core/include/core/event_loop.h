#pragma once
#include <stddef.h>
#include <stdint.h>

#include "core/plugin_api.h"

enum {
    CORE_REQ_BUF_SIZE = 4096,
};

/* State of a single connection. The caller owns the pool (static array):
 * fd == -1 marks a free slot. Responses are per-conn: concurrency-safe. */
struct core_conn {
    int fd;
    char req_buf[CORE_REQ_BUF_SIZE];
    size_t req_len;
    struct plugin_response_t resp;
    size_t sent_len;
};

/* Handler for a completed request (req_buf holds it up to \r\n\r\n).
 * Fills resp: head is the full HTTP response with headers, iov/iov_count
 * are the writev parts. 0 = ok, otherwise the connection is closed. */
typedef int (*core_handler_fn)(struct core_conn *conn);

/* Socket + SO_REUSEADDR + bind(INADDR_ANY) + listen; -1 on error */
int core_listen(uint16_t port);

/* Zero-heap epoll loop: blocks forever. Level-triggered, Connection: close
 * semantics. Partial writes resume on EPOLLOUT. Returns -1 only on a
 * fatal initialization failure. */
int core_loop_run(int listen_fd, struct core_conn *conns, size_t max_conns,
                  core_handler_fn handler);
