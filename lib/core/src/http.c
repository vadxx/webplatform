#include "core/http.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/uri.h"

enum {
    WRAP_HEADERS_CAP = 256,   /* max header block for wrap_body's fixed fields */
    PLUGIN_REQUEST_CAP = 192, /* "GET /" + 128-max segment + " HTTP/1.1" + slack */
};

/* Named content-type constants (declarations in core/http.h) */
const char core_content_type_html[] = "text/html";
const char core_content_type_css[] = "text/css";
const char core_content_type_js[] = "application/javascript";
const char core_content_type_json[] = "application/json";
const char core_content_type_plain[] = "text/plain";

/* HTTP status lines: single source for the helpers below */
static const char STATUS_OK[] = "200 OK";
static const char STATUS_NOT_FOUND[] = "404 Not Found";
static const char ENCODING_GZIP[] = "Content-Encoding: gzip\r\n";
static const char NOT_FOUND_BODY[] = "not found\n";
static const char SYM_PLUGIN_INIT[] = "plugin_init";
static const char SYM_PLUGIN_HANDLE[] = "plugin_handle";
static const char PLUGIN_REQUEST_FMT[] = "GET /%.*s HTTP/1.1";

/* --- Request line --- */

const char *core_http_request_path(const char *req, size_t req_len, size_t *path_len) {
    if (req == NULL || path_len == NULL) {
        return NULL;
    }
    size_t i = 0;
    while (i < req_len && req[i] != ' ') {
        ++i;
    }
    if (i == req_len) {
        return NULL;
    }
    const size_t begin = ++i;
    while (i < req_len && req[i] != ' ' && req[i] != '\r') {
        ++i;
    }
    *path_len = i - begin;
    return req + begin;
}

/* --- Response composition --- */

size_t core_http_head(char *buf, size_t cap, const char *status, const char *content_type,
                      size_t content_len, int gzip_encoded) {
    if (buf == NULL || cap == 0 || status == NULL || content_type == NULL) {
        return 0;
    }
    const char *const encoding = gzip_encoded != 0 ? ENCODING_GZIP : "";
    const int written = snprintf(buf, cap,
                                 "HTTP/1.1 %s\r\nContent-Type: %s\r\n%s"
                                 "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                                 status, content_type, encoding, content_len);
    if (written <= 0 || (size_t)written >= cap) {
        return 0;
    }
    return (size_t)written;
}

int core_http_serve_asset(struct core_conn *conn, const char *content_type, const void *gz,
                          size_t gz_len) {
    if (conn == NULL || gz == NULL) {
        return -1;
    }
    const size_t head_len = core_http_head(conn->resp.head, sizeof(conn->resp.head), STATUS_OK,
                                           content_type, gz_len, 1);
    if (head_len == 0) {
        return -1;
    }
    conn->resp.head_len = head_len;
    conn->resp.iov[0].iov_base = conn->resp.head;
    conn->resp.iov[0].iov_len = head_len;
    conn->resp.iov[1].iov_base = (void *)gz;
    conn->resp.iov[1].iov_len = gz_len;
    conn->resp.iov_count = 2;
    conn->resp.total_len = head_len + gz_len;
    return 0;
}

int core_http_not_found(struct core_conn *conn) {
    if (conn == NULL) {
        return -1;
    }
    const size_t head_len =
        core_http_head(conn->resp.head, sizeof(conn->resp.head), STATUS_NOT_FOUND,
                       core_content_type_plain, sizeof(NOT_FOUND_BODY) - 1, 0);
    if (head_len == 0) {
        return -1;
    }
    conn->resp.head_len = head_len;
    conn->resp.iov[0].iov_base = conn->resp.head;
    conn->resp.iov[0].iov_len = head_len;
    conn->resp.iov[1].iov_base = (void *)NOT_FOUND_BODY;
    conn->resp.iov[1].iov_len = sizeof(NOT_FOUND_BODY) - 1;
    conn->resp.iov_count = 2;
    conn->resp.total_len = head_len + sizeof(NOT_FOUND_BODY) - 1;
    return 0;
}

int core_http_wrap_body(struct plugin_response_t *resp) {
    if (resp == NULL) {
        return -1;
    }
    const size_t body_len = resp->head_len;
    size_t tail_len = 0;
    for (int i = 1; i < resp->iov_count; ++i) {
        tail_len += resp->iov[i].iov_len;
    }
    char headers[WRAP_HEADERS_CAP];
    const size_t head_len = core_http_head(headers, sizeof(headers), STATUS_OK,
                                           core_content_type_html, body_len + tail_len, 0);
    if (head_len == 0 || head_len + body_len > sizeof(resp->head)) {
        return -1;
    }
    memmove(resp->head + head_len, resp->head, body_len);
    memcpy(resp->head, headers, head_len);
    resp->head_len = head_len + body_len;
    resp->iov[0].iov_base = resp->head;
    resp->iov[0].iov_len = resp->head_len;
    resp->total_len = resp->head_len + tail_len;
    return 0;
}

/* --- Host & plugin loading (executable side) --- */

void core_host_api_default(struct host_api_t *api) {
    if (api == NULL) {
        return;
    }
    api->snprintf = snprintf;
    api->memcpy = memcpy;
}

int core_plugin_open(struct core_plugin *p, const char *path, const struct host_api_t *api) {
    if (p == NULL || path == NULL || api == NULL) {
        return -1;
    }
    void *lib = dlopen(path, RTLD_NOW);
    if (lib == NULL) {
        return -1;
    }
    void *init_sym = dlsym(lib, SYM_PLUGIN_INIT);
    void *handle_sym = dlsym(lib, SYM_PLUGIN_HANDLE);
    if (init_sym == NULL || handle_sym == NULL) {
        dlclose(lib);
        return -1;
    }
    /* ISO C forbids direct void* -> fn-ptr (-Wpedantic): copy representations */
    memcpy((void *)&p->init, (const void *)&init_sym, sizeof(p->init));
    memcpy((void *)&p->handle, (const void *)&handle_sym, sizeof(p->handle));
    p->lib = lib;
    if (p->init(api) != 0) {
        dlclose(lib);
        return -1;
    }
    return 0;
}

/* --- Routing --- */

int core_http_match_exact(const char *path, size_t path_len, const char *route_path) {
    const size_t route_len = strlen(route_path);
    if (path == NULL || path_len != route_len) {
        return 0;
    }
    return memcmp(path, route_path, route_len) == 0 ? 1 : 0;
}

int core_http_match_prefix(const char *path, size_t path_len, const char *prefix, const char **seg,
                           size_t *seg_len) {
    const size_t prefix_len = strlen(prefix);
    if (path == NULL || seg == NULL || seg_len == NULL || path_len <= prefix_len) {
        return 0;
    }
    if (memcmp(path, prefix, prefix_len) != 0) {
        return 0;
    }
    *seg = path + prefix_len;
    *seg_len = path_len - prefix_len;
    return 1;
}

int core_http_serve_asset_route(struct core_conn *conn, const char *path, size_t path_len,
                                const struct core_http_asset_route *routes, size_t route_count) {
    if (conn == NULL || path == NULL) {
        return -1;
    }
    for (size_t i = 0; i < route_count; ++i) {
        if (core_http_match_exact(path, path_len, routes[i].path)) {
            return core_http_serve_asset(conn, routes[i].content_type, routes[i].gz,
                                         routes[i].gz_len);
        }
    }
    return 1;
}

int core_http_serve_plugin(struct core_conn *conn, const struct core_plugin *plugin,
                           const char *seg, size_t seg_len) {
    if (conn == NULL || plugin == NULL) {
        return -1;
    }
    if (!core_uri_segment_valid(seg, seg_len)) {
        return core_http_not_found(conn);
    }
    char request[PLUGIN_REQUEST_CAP];
    const int req_len = snprintf(request, sizeof(request), PLUGIN_REQUEST_FMT, (int)seg_len, seg);
    if (req_len <= 0) {
        return -1;
    }
    if (plugin->handle(request, (size_t)req_len, &conn->resp) != 0) {
        return core_http_not_found(conn);
    }
    /* fragment pattern: the dynamic head only, without the plugin asset part */
    conn->resp.iov_count = 1;
    conn->resp.total_len = conn->resp.head_len;
    return core_http_wrap_body(&conn->resp);
}

int core_http_route(struct core_conn *conn, const struct core_http_asset_route *assets,
                    size_t asset_count, const struct core_http_plugin_route *plugins,
                    size_t plugin_count) {
    if (conn == NULL) {
        return -1;
    }
    size_t path_len = 0;
    const char *path = core_http_request_path(conn->req_buf, conn->req_len, &path_len);
    if (path == NULL) {
        return core_http_not_found(conn);
    }
    if (core_http_serve_asset_route(conn, path, path_len, assets, asset_count) == 0) {
        return 0;
    }
    for (size_t i = 0; i < plugin_count; ++i) {
        const char *seg = NULL;
        size_t seg_len = 0;
        if (core_http_match_prefix(path, path_len, plugins[i].prefix, &seg, &seg_len)) {
            return core_http_serve_plugin(conn, plugins[i].plugin, seg, seg_len);
        }
    }
    return core_http_not_found(conn);
}

/* --- Port from the platform convention --- */

enum {
    PORT_MIN = 1,
    PORT_MAX = 65535,
};

static const char ENV_HTTP_PORT[] = "WEBPLATFORM_HTTP_PORT";

uint16_t core_http_port_env(uint16_t default_port) {
    const char *env = getenv(ENV_HTTP_PORT);
    if (env == NULL) {
        return default_port;
    }
    const int port = atoi(env);
    if (port < PORT_MIN || port > PORT_MAX) {
        fprintf(stderr, "core: bad %s='%s', using %u\n", ENV_HTTP_PORT, env, default_port);
        return default_port;
    }
    return (uint16_t)port;
}
