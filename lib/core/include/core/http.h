#pragma once
#include <stddef.h>

#include "core/event_loop.h"
#include "core/plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Request line --- */

/* Path of the request line: between the first and second space.
 * Returns a pointer into req and writes the length; NULL when absent. */
const char *core_http_request_path(const char *req, size_t req_len, size_t *path_len);

/* --- Response composition --- */

/* Writes HTTP response headers into buf; returns the head length,
 * 0 on bad args or when the headers do not fit. */
size_t core_http_head(char *buf, size_t cap, const char *status, const char *content_type,
                      size_t content_len, int gzip_encoded);

/* Response from an embedded gzip asset: iov[0]=head, iov[1]=asset. 0 ok. */
int core_http_serve_asset(struct core_conn *conn, const char *content_type, const void *gz,
                          size_t gz_len);

/* 404 with a plain-text body. 0 ok. */
int core_http_not_found(struct core_conn *conn);

/* --- Routing --- */

/* Common content types: named constants usable in static route-table initializers */
extern const char core_content_type_html[];
extern const char core_content_type_css[];
extern const char core_content_type_js[];
extern const char core_content_type_json[];
extern const char core_content_type_plain[];

/* Exact path match against a literal route. 1 match, 0 no match. */
int core_http_match_exact(const char *path, size_t path_len, const char *route_path);

/* Prefix match: on match writes the tail (after prefix) into seg/seg_len.
 * The prefix alone (empty tail) does not match. 1 match, 0 no match. */
int core_http_match_prefix(const char *path, size_t path_len, const char *prefix, const char **seg,
                           size_t *seg_len);

/* Static route: exact path -> embedded gzip asset */
struct core_http_asset_route {
    const char *path;
    const char *content_type;
    const void *gz;
    size_t gz_len;
};

/* Plugin route: prefix -> plugin (the tail segment becomes the plugin request) */
struct core_http_plugin_route {
    const char *prefix;
    const struct core_plugin *plugin;
};

/* Serves the first matching asset route. 0 served, 1 no match (fall through). */
int core_http_serve_asset_route(struct core_conn *conn, const char *path, size_t path_len,
                                const struct core_http_asset_route *routes, size_t route_count);

/* Fragment pattern: validate the segment, compose "GET /<seg> HTTP/1.1",
 * call the plugin, keep the dynamic head only, wrap with HTTP headers.
 * Invalid segment or plugin decline -> 404. 0 ok, -1 error. */
int core_http_serve_plugin(struct core_conn *conn, const struct core_plugin *plugin,
                           const char *seg, size_t seg_len);

/* Full router: parse the path from conn, walk the asset table, then the
 * plugin table, fall through to 404. 0 ok, -1 error. */
int core_http_route(struct core_conn *conn, const struct core_http_asset_route *assets,
                    size_t asset_count, const struct core_http_plugin_route *plugins,
                    size_t plugin_count);

/* Reads WEBPLATFORM_HTTP_PORT (the platform convention, passed in by
 * compose). Returns default_port when absent or unparsable (a warning is
 * printed for garbage). */
uint16_t core_http_port_env(uint16_t default_port);

/* Element count of a static array */
#define CORE_ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Wraps the plugin body (resp->head plus resp->iov[1..)) with HTTP headers:
 * shifts the body right and prepends the head. iov[1..) are preserved. */
int core_http_wrap_body(struct plugin_response_t *resp);

/* --- Host & plugin loading (executable side) --- */

/* Fills host_api with the libc functions of the executable itself. */
void core_host_api_default(struct host_api_t *api);

/* Loaded plugin handle */
struct core_plugin {
    void *lib;
    int (*init)(const struct host_api_t *);
    int (*handle)(const char *, size_t, struct plugin_response_t *);
};

/* dlopen + dlsym + plugin_init in one call; -1 on any failure. */
int core_plugin_open(struct core_plugin *p, const char *path, const struct host_api_t *api);

#ifdef __cplusplus
}
#endif
