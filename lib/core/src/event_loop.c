#include "core/event_loop.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
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

/* The request is complete once the buffer holds \r\n\r\n (end of headers) */
static int request_complete(const char *buf, size_t len) {
    const size_t term_len = sizeof("\r\n\r\n") - 1;
    for (size_t i = 0; i + term_len <= len; ++i) {
        if (memcmp(buf + i, "\r\n\r\n", term_len) == 0) {
            return 1;
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

/* Reads available bytes; on a complete request runs the handler and first write */
static void loop_read(int ep, struct core_conn *conn, core_handler_fn handler) {
    const ssize_t got =
        read(conn->fd, conn->req_buf + conn->req_len, sizeof(conn->req_buf) - conn->req_len);
    if (got <= 0) {
        conn_release(ep, conn);
        return;
    }
    conn->req_len += (size_t)got;
    if (!request_complete(conn->req_buf, conn->req_len)) {
        if (conn->req_len >= sizeof(conn->req_buf)) {
            conn_release(ep, conn); /* header limit exceeded: drop */
        }
        return; /* waiting for more request bytes */
    }
    if (handler(conn) != 0) {
        conn_release(ep, conn);
        return;
    }
    conn->sent_len = 0;
    loop_write(ep, conn);
}

/* Handles one epoll event batch */
static void loop_dispatch(int ep, struct epoll_event *events, int ready, int listen_fd,
                          struct core_conn *conns, size_t max_conns, core_handler_fn handler) {
    for (int idx = 0; idx < ready; ++idx) {
        const int fd = events[idx].data.fd;
        if (fd == listen_fd) {
            loop_accept(ep, listen_fd, conns, max_conns);
            continue;
        }
        struct core_conn *conn = pool_find_by_fd(conns, max_conns, fd);
        if (conn == NULL) {
            continue;
        }
        if (events[idx].events & (EPOLLERR | EPOLLHUP)) {
            conn_release(ep, conn);
            continue;
        }
        if (events[idx].events & EPOLLIN) {
            loop_read(ep, conn, handler); /* includes the first response write */
        } else if (events[idx].events & EPOLLOUT) {
            loop_write(ep, conn); /* resume a partial write */
        }
    }
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

int core_loop_run(int listen_fd, struct core_conn *conns, size_t max_conns,
                  core_handler_fn handler) {
    if (listen_fd < 0 || conns == NULL || max_conns == 0 || handler == NULL) {
        return -1;
    }
    for (size_t i = 0; i < max_conns; ++i) { /* pool: all slots free */
        conns[i].fd = -1;
        conns[i].req_len = 0;
        conns[i].sent_len = 0;
    }
    const int ep = epoll_create1(0);
    if (ep < 0) {
        return -1;
    }
    if (epoll_update(ep, EPOLL_CTL_ADD, listen_fd, EPOLLIN) != 0) {
        close(ep);
        return -1;
    }
    struct epoll_event events[CORE_MAX_EVENTS];
    for (;;) {
        const int ready = epoll_wait(ep, events, CORE_MAX_EVENTS, -1);
        if (ready < 0) {
            continue; /* EINTR */
        }
        loop_dispatch(ep, events, ready, listen_fd, conns, max_conns, handler);
    }
}
