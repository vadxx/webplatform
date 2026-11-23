#include "core/event_loop.h"

#include "core/http.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

enum {
    CORE_LISTEN_BACKLOG = 64,
    CORE_MAX_EVENTS = 64,
};

/* --- Pure helpers: tested directly (test_event_loop.c) --- */

/* First free slot (fd == -1) or NULL */
static struct core_conn *conn_find_free(struct core_conn *conns, size_t max_conns) {
    for (size_t i = 0; i < max_conns; ++i) {
        if (conns[i].fd == -1) {
            return &conns[i];
        }
    }
    return NULL;
}

/* Offset just past the first \r\n\r\n, or 0 while headers are incomplete */
static size_t request_end(const char *buf, size_t len) {
    const size_t term_len = sizeof("\r\n\r\n") - 1;
    for (size_t i = 0; i + term_len <= len; ++i) {
        if (memcmp(buf + i, "\r\n\r\n", term_len) == 0) {
            return i + term_len;
        }
    }
    return 0;
}

/* Remainder of the iov vector after `sent` bytes: shifts bases and lengths */
static int iov_skip(const struct iovec *src, int count, size_t sent, struct iovec *dst) {
    int dst_count = 0;
    size_t pos = 0;
    for (int i = 0; i < count; ++i) {
        const size_t begin = pos;
        const size_t end = pos + src[i].iov_len;
        pos = end;
        if (end <= sent) {
            continue; /* chunk fully sent */
        }
        const size_t inside = sent > begin ? sent - begin : 0;
        dst[dst_count].iov_base = (char *)src[i].iov_base + inside;
        dst[dst_count].iov_len = src[i].iov_len - inside;
        ++dst_count;
    }
    return dst_count;
}

/* Source with this fd, or NULL */
static const struct core_poll_source *source_find(const struct core_poll_source *sources,
                                                  size_t count, int fd) {
    for (size_t i = 0; i < count; ++i) {
        if (sources[i].fd == fd) {
            return &sources[i];
        }
    }
    return NULL;
}

/* --- Socket helpers --- */

static int epoll_update(int ep, int op, int fd, uint32_t events) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(ep, op, fd, &ev);
}

static void conn_release(int ep, struct core_conn *conn) {
    epoll_ctl(ep, EPOLL_CTL_DEL, conn->fd, NULL);
    close(conn->fd);
    conn->fd = -1;
    conn->req_len = 0;
    conn->body_expect = 0;
    conn->body_len = 0;
    conn->sent_len = 0;
}

static struct core_conn *pool_find_by_fd(struct core_conn *conns, size_t max_conns, int fd) {
    for (size_t i = 0; i < max_conns; ++i) {
        if (conns[i].fd == fd) {
            return &conns[i];
        }
    }
    return NULL;
}

static int set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void loop_accept(int ep, int listen_fd, struct core_conn *conns, size_t max_conns) {
    const int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) {
        return;
    }
    if (set_nonblocking(fd) != 0) {
        close(fd);
        return;
    }
    struct core_conn *conn = conn_find_free(conns, max_conns);
    if (conn == NULL) {
        close(fd); /* pool is full: drop the new connection */
        return;
    }
    conn->fd = fd;
    conn->req_len = 0;
    conn->body_expect = 0;
    conn->body_len = 0;
    conn->sent_len = 0;
    epoll_update(ep, EPOLL_CTL_ADD, fd, EPOLLIN);
}

/* Writes out the rest of the response; Connection: close finishes it */
static void loop_write(int ep, struct core_conn *conn) {
    struct iovec rest[2];
    const int count = iov_skip(conn->resp.iov, conn->resp.iov_count, conn->sent_len, rest);
    if (count == 0) {
        conn_release(ep, conn);
        return;
    }
    const ssize_t written = writev(conn->fd, rest, count);
    if (written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            epoll_update(ep, EPOLL_CTL_MOD, conn->fd, EPOLLIN | EPOLLOUT);
        } else {
            conn_release(ep, conn);
        }
        return;
    }
    conn->sent_len += (size_t)written;
    if (conn->sent_len >= conn->resp.total_len) {
        conn_release(ep, conn); /* fully sent: Connection: close */
    } else {
        epoll_update(ep, EPOLL_CTL_MOD, conn->fd, EPOLLIN | EPOLLOUT);
    }
}

/* Reads more header bytes; 0 when headers plus body are complete,
 * 1 when more bytes are expected, -1 when the connection is dropped. */
static int headers_arrive(int ep, struct core_conn *conn) {
    const ssize_t got =
        read(conn->fd, conn->req_buf + conn->req_len, sizeof(conn->req_buf) - conn->req_len);
    if (got <= 0) {
        conn_release(ep, conn);
        return -1;
    }
    conn->req_len += (size_t)got;
    const size_t end = request_end(conn->req_buf, conn->req_len);
    if (end == 0) {
        if (conn->req_len >= sizeof(conn->req_buf)) {
            conn_release(ep, conn); /* header limit exceeded: drop */
            return -1;
        }
        return 1; /* waiting for more header bytes */
    }
    size_t content_len = 0;
    if (core_http_content_length(CORE_SV_BUF(conn->req_buf, conn->req_len), &content_len) == 0 &&
        content_len > CORE_BODY_CAP) {
        conn_release(ep, conn); /* body limit exceeded: drop */
        return -1;
    }
    conn->body_expect = content_len; /* 0 when the header is absent */
    const size_t available = conn->req_len - end;
    const size_t take = available < content_len ? available : content_len;
    memcpy(conn->body, conn->req_buf + end, take);
    conn->body_len = take;
    return conn->body_len < conn->body_expect ? 1 : 0;
}

/* Reads body bytes straight into conn->body; same return contract. */
static int body_arrive(int ep, struct core_conn *conn) {
    const ssize_t got =
        read(conn->fd, conn->body + conn->body_len, conn->body_expect - conn->body_len);
    if (got <= 0) {
        conn_release(ep, conn);
        return -1;
    }
    conn->body_len += (size_t)got;
    return conn->body_len < conn->body_expect ? 1 : 0;
}

/* Reads available bytes; on a complete request (headers plus
 * Content-Length bytes of body) runs the handler and the first write */
static void loop_read(int ep, struct core_conn *conn, core_handler_fn handler) {
    int pending = 0;
    if (request_end(conn->req_buf, conn->req_len) == 0) {
        pending = headers_arrive(ep, conn);
    } else if (conn->body_len < conn->body_expect) {
        pending = body_arrive(ep, conn);
    }
    if (pending != 0) {
        return; /* dropped, or waiting for more bytes */
    }
    if (handler(conn) != 0) {
        conn_release(ep, conn);
        return;
    }
    conn->sent_len = 0;
    loop_write(ep, conn);
}

/* Loop context: everything the dispatcher needs, grouped. */
struct core_loop {
    int ep;
    int listen_fd;
    struct core_conn *conns;
    size_t max_conns;
    core_handler_fn handler;
    const struct core_poll_source *sources;
    size_t source_count;
};

/* Handles one epoll event batch */
static void loop_dispatch(const struct core_loop *loop, struct epoll_event *events, int ready) {
    for (int idx = 0; idx < ready; ++idx) {
        const int fd = events[idx].data.fd;
        const struct core_poll_source *source = source_find(loop->sources, loop->source_count, fd);
        if (source != NULL) {
            source->on_readable(source->ctx);
            continue;
        }
        if (fd == loop->listen_fd) {
            loop_accept(loop->ep, loop->listen_fd, loop->conns, loop->max_conns);
            continue;
        }
        struct core_conn *conn = pool_find_by_fd(loop->conns, loop->max_conns, fd);
        if (conn == NULL) {
            continue;
        }
        if (events[idx].events & (EPOLLERR | EPOLLHUP)) {
            conn_release(loop->ep, conn);
            continue;
        }
        if (events[idx].events & EPOLLIN) {
            loop_read(loop->ep, conn, loop->handler); /* includes the first write */
        } else if (events[idx].events & EPOLLOUT) {
            loop_write(loop->ep, conn); /* resume a partial write */
        }
    }
}

int core_timer_fd(int interval_sec) {
    if (interval_sec < 0) {
        return -1;
    }
    const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    struct itimerspec spec;
    memset(&spec, 0, sizeof(spec));
    if (interval_sec == 0) {
        spec.it_value.tv_nsec = 1; /* one-shot: fires immediately */
    } else {
        spec.it_interval.tv_sec = interval_sec; /* periodic tick */
        spec.it_value.tv_sec = interval_sec;    /* first fire after one interval */
    }
    if (timerfd_settime(fd, 0, &spec, NULL) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int core_listen(uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    const int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        close(fd);
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, CORE_LISTEN_BACKLOG) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Validates loop arguments and resets every pool slot to free. */
static int loop_args_valid(const struct core_conn_pool *pool, core_handler_fn handler) {
    const size_t max_conns = pool != NULL ? pool->count : 0;
    if (pool == NULL || pool->conns == NULL || max_conns == 0 || handler == NULL) {
        return 0;
    }
    for (size_t i = 0; i < max_conns; ++i) { /* pool: all slots free */
        pool->conns[i].fd = -1;
        pool->conns[i].req_len = 0;
        pool->conns[i].body_expect = 0;
        pool->conns[i].body_len = 0;
        pool->conns[i].sent_len = 0;
    }
    return 1;
}

int core_loop_run(int listen_fd, const struct core_conn_pool *pool, core_handler_fn handler,
                  const struct core_poll_set *sources) {
    if (listen_fd < 0 || !loop_args_valid(pool, handler)) {
        return -1;
    }
    const struct core_poll_source *source_items = sources != NULL ? sources->items : NULL;
    const size_t source_count = sources != NULL ? sources->count : 0;
    for (size_t i = 0; i < source_count; ++i) {
        if (source_items[i].fd < 0 || source_items[i].on_readable == NULL) {
            return -1;
        }
    }
    const int ep = epoll_create1(0);
    if (ep < 0) {
        return -1;
    }
    if (epoll_update(ep, EPOLL_CTL_ADD, listen_fd, EPOLLIN) != 0) {
        close(ep);
        return -1;
    }
    for (size_t i = 0; i < source_count; ++i) {
        if (epoll_update(ep, EPOLL_CTL_ADD, source_items[i].fd, EPOLLIN) != 0) {
            close(ep);
            return -1;
        }
    }
    struct core_loop loop = {
        .ep = ep,
        .listen_fd = listen_fd,
        .conns = pool->conns,
        .max_conns = pool->count,
        .handler = handler,
        .sources = source_items,
        .source_count = source_count,
    };
    struct epoll_event events[CORE_MAX_EVENTS];
    for (;;) {
        const int ready = epoll_wait(ep, events, CORE_MAX_EVENTS, -1);
        if (ready < 0) {
            continue; /* EINTR */
        }
        loop_dispatch(&loop, events, ready);
    }
}
