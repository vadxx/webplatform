#pragma once
#include <stddef.h>
#include <sys/uio.h>

/* Export despite the global -fvisibility=hidden */
#if defined(__GNUC__)
#define CORE_PLUGIN_API __attribute__((visibility("default")))
#else
#define CORE_PLUGIN_API
#endif

enum {
    CORE_RESP_HEAD_SIZE = 4096,
};

/* Host functions available to the plugin via pointers (zero-heap).
 * The server repository extends this struct in its own copy (writev, sendfile, ...).
 */
struct host_api_t {
    int (*snprintf)(char *buf, size_t sz, const char *fmt, ...);
    void *(*memcpy)(void *dst, const void *src, size_t n);
};

/* Plugin response: written into a CALLER-owned struct (concurrency-safe:
 * each connection has its own). iov[0] is the dynamic head, iov[1] is the
 * plugin's read-only static asset. No allocations. */
struct plugin_response_t {
    char head[CORE_RESP_HEAD_SIZE];
    size_t head_len;
    struct iovec iov[2];
    int iov_count;
    size_t total_len;
};

/* Plugin ABI: unprefixed names form the dlsym contract.
 * Demo request: "<METHOD> <path> HTTP/1.1...". 0 = ok, -1 = error. */
CORE_PLUGIN_API int plugin_init(const struct host_api_t *api);
CORE_PLUGIN_API int plugin_handle(const char *request, size_t request_len,
                                  struct plugin_response_t *out);
