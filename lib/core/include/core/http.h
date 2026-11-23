#pragma once
#include <stddef.h>

#include "core/event_loop.h"
#include "core/plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Zero-copy string slice: a pointer and a length, never a copy.
 * CORE_SV for string literals, CORE_SV_BUF for runtime buffers. */
typedef struct {
    const char *data;
    size_t len;
} core_str_view_t;

#define CORE_SV(literal) ((core_str_view_t){(literal), sizeof(literal) - 1})
#define CORE_SV_BUF(buf, n) ((core_str_view_t){(buf), (n)})

/* --- Request line and headers --- */

/* Path of the request line: between the first and second space.
 * Returns a view into req; NULL when absent. */
const char *core_http_request_path(core_str_view_t req, core_str_view_t *path);

/* Request method: the first word of the request line. Returns a view
 * into req; NULL when there is no space. */
const char *core_http_request_method(core_str_view_t req, core_str_view_t *method);

/* Content-Length header: 0 ok, -1 absent or unparsable. Header names
 * match case-insensitively; the value must be digits only. */
int core_http_content_length(core_str_view_t req, size_t *out_len);

/* --- Response composition --- */

/* Everything core_http_head needs besides the buffer. */
struct core_http_head_spec {
    const char *status;
    const char *content_type;
    size_t content_len;
    int gzip_encoded;
};

/* Writes HTTP response headers into buf; returns the head length,
 * 0 on bad args or when the headers do not fit. */
size_t core_http_head(char *buf, size_t cap, const struct core_http_head_spec *spec);

/* Response from an embedded gzip asset: iov[0]=head, iov[1]=asset. 0 ok. */
int core_http_serve_asset(struct core_conn *conn, const char *content_type, const void *gz,
                          size_t gz_len);

/* 404 with a plain-text body. 0 ok. */
int core_http_not_found(struct core_conn *conn);

/* Wires the response for a pooled body: head plus one iov for the
 * slot. body must be a pool slot. 0 ok, -1 error. */
int core_http_serve_body(struct core_conn *conn, const char *status, const char *content_type,
                         core_str_view_t body);

/* Generic status response with a short plain-text body (404, 503, ...). */
int core_http_serve_status(struct core_conn *conn, const char *status, const char *body);

/* 302 redirect with an empty body. */
int core_http_serve_redirect(struct core_conn *conn, const char *location);

/* Set-Cookie attributes; value is a view. */
struct core_http_cookie_spec {
    const char *name;
    core_str_view_t value;
    int max_age_sec;
};

/* Appends "Set-Cookie: name=value; Max-Age=N; HttpOnly; Path=/\r\n"
 * before the final blank line of an already composed head. 0 ok,
 * -1 when it does not fit (the head is left untouched). */
int core_http_set_cookie(char *head, size_t cap, const struct core_http_cookie_spec *spec);

/* --- Routing --- */

/* Common content types: named constants usable in static route-table initializers */
extern const char core_content_type_html[];
extern const char core_content_type_css[];
extern const char core_content_type_js[];
extern const char core_content_type_json[];
extern const char core_content_type_plain[];

/* Common HTTP status lines: single source for serve_status/serve_body callers */
extern const char core_http_status_ok[];                 /* "200 OK" */
extern const char core_http_status_redirect[];           /* "302 Found" */
extern const char core_http_status_unauthorized[];       /* "401 Unauthorized" */
extern const char core_http_status_not_found[];          /* "404 Not Found" */
extern const char core_http_status_method_not_allowed[]; /* "405 Method Not Allowed" */
extern const char core_http_status_unavailable[];        /* "503 Service Unavailable" */

/* Exact path match against a literal route. 1 match, 0 no match. */
int core_http_match_exact(core_str_view_t path, const char *route_path);

/* Prefix match: on match writes the tail (after prefix) into seg.
 * The prefix alone (empty tail) does not match. 1 match, 0 no match. */
int core_http_match_prefix(core_str_view_t path, const char *prefix, core_str_view_t *seg);

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

/* Both route tables with their counts. */
struct core_http_routes {
    const struct core_http_asset_route *assets;
    size_t asset_count;
    const struct core_http_plugin_route *plugins;
    size_t plugin_count;
};

/* Serves the first matching asset route. 0 served, 1 no match (fall through). */
int core_http_serve_asset_route(struct core_conn *conn, core_str_view_t path,
                                const struct core_http_routes *routes);

/* Fragment pattern: validate the segment, compose "GET /<seg> HTTP/1.1",
 * call the plugin, keep the dynamic head only, wrap with HTTP headers.
 * Invalid segment or plugin decline -> 404. 0 ok, -1 error. */
int core_http_serve_plugin(struct core_conn *conn, const struct core_plugin *plugin,
                           core_str_view_t seg);

/* Full router: parse the path from conn, walk the asset table, then the
 * plugin table, fall through to 404. 0 ok, -1 error. */
int core_http_route(struct core_conn *conn, const struct core_http_routes *routes);

/* --- Query string and forms --- */

/* Length of the path without the "?query" part (equal to path.len when
 * there is no '?'). Use for route matching. */
size_t core_http_path_only(core_str_view_t path);

/* Value of ?name=... inside the path, as a zero-copy view. An empty
 * value ("?name=") is found with len == 0. 1 found, 0 absent. */
int core_http_query_param(core_str_view_t path, const char *name, core_str_view_t *val);

/* Bounded composition: appends formatted text at *pos inside
 * [buf, buf+cap). 0 ok, -1 when it would not fit (pos is unchanged). */
int core_http_append(char *buf, size_t cap, size_t *pos, const char *fmt, ...);

/* Large dynamic response bodies: pooled static slots, because the
 * response head (4 KiB by ABI) does not fit chart-sized JSON.
 * Contract: conn must live in the static connection pool - slots are
 * reaped lazily by re-serving the same conn or by a released conn
 * (fd == -1), which Connection: close guarantees. Returns NULL when
 * all slots are busy. */
enum {
    CORE_BODY_SLOTS = 4,
    CORE_BODY_SLOT_CAP = 32 * 1024,
};
char *core_http_body_checkout(struct core_conn *conn);

/* Inclusive bounds for core_http_query_long. */
struct core_http_range {
    long long min;
    long long max;
};

/* Numeric query parameter: digits only. Absent, non-numeric, empty or
 * zero values return def; otherwise the value is clamped into range. */
long long core_http_query_long(core_str_view_t path, const char *name, long long def,
                               struct core_http_range range);

/* Cookie: a name=value pair from the Cookie header, as a zero-copy
 * view. Pairs are ';'-separated, spaces around '=' are trimmed. 1/0. */
int core_http_cookie(core_str_view_t req, const char *name, core_str_view_t *val);

/* x-www-form-urlencoded body field, as a zero-copy view (no URL
 * decoding). Pairs are '&'-separated. 1 found, 0 absent. */
int core_http_form_field(core_str_view_t body, const char *name, core_str_view_t *val);

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

/* Reads WEBPLATFORM_HTTP_PORT (the platform convention, passed in by
 * compose). Returns default_port when absent or unparsable (a warning is
 * printed for garbage). */
uint16_t core_http_port_env(uint16_t default_port);

/* Element count of a static array */
#define CORE_ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Wraps the plugin body (resp->head plus resp->iov[1..)) with HTTP headers:
 * shifts the body right and prepends the head. iov[1..) are preserved. */
int core_http_wrap_body(struct plugin_response_t *resp);

#ifdef __cplusplus
}
#endif
