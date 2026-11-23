#include "hello_plugin.h"
#include "core/plugin_api.h"
#include "core/uri.h"
#include "hello_plugin_page_assets.h"

/* Host API copy received in plugin_init */
static struct host_api_t g_host_api;

CORE_PLUGIN_API int plugin_init(const struct host_api_t *api) {
    if (api == NULL) {
        return -1;
    }
    g_host_api = *api;
    return 0;
}

/* Path segment: chars after the first '/' up to the next '/', space or end.
 * Returns the segment length, or 0 when no '/' is found. */
static size_t path_segment(const char *request, size_t request_len, const char **out) {
    size_t i = 0;
    while (i < request_len && request[i] != '/') {
        ++i;
    }
    if (i == request_len) {
        return 0;
    }
    const size_t begin = ++i;
    while (i < request_len && request[i] != '/' && request[i] != ' ') {
        ++i;
    }
    *out = request + begin;
    return i - begin;
}

CORE_PLUGIN_API int plugin_handle(const char *request, size_t request_len,
                                  struct plugin_response_t *out) {
    if (out == NULL || request == NULL || request_len == 0) {
        return -1;
    }
    if (g_host_api.snprintf == NULL) {
        return -1;
    }

    const char *segment = NULL;
    const size_t seg_len = path_segment(request, request_len, &segment);
    if (seg_len == 0) {
        return -1;
    }
    if (!core_uri_segment_valid(segment, seg_len)) {
        return -1;
    }

    const int written = g_host_api.snprintf(out->head, sizeof(out->head), HELLO_RESPONSE_FMT,
                                            (int)seg_len, segment);
    size_t head_len = 0;
    if (written > 0) {
        head_len = (size_t)written;
        if (head_len >= sizeof(out->head)) {
            head_len = sizeof(out->head) - 1;
        }
    }

    out->head_len = head_len;
    out->iov[0].iov_base = out->head;
    out->iov[0].iov_len = head_len;
    out->iov[1].iov_base = (void *)hello_plugin_page_gz; /* read-only static asset */
    out->iov[1].iov_len = hello_plugin_page_gz_len;
    out->iov_count = 2;
    out->total_len = head_len + hello_plugin_page_gz_len;
    return 0;
}
