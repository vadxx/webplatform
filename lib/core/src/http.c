#include "core/http.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/uri.h"

enum {
    WRAP_HEADERS_CAP = 256,     /* max header block for wrap_body's fixed fields */
    PLUGIN_REQUEST_CAP = 192,   /* "GET /" + 128-max segment + " HTTP/1.1" + slack */
    QUERY_LONG_DIGITS_MAX = 15, /* long long never overflows from 15 digits */
    SET_COOKIE_CAP = 192,
    REDIRECT_LINE_CAP = 192,
};

/* Named content-type constants (declarations in core/http.h) */
const char core_content_type_html[] = "text/html";
const char core_content_type_css[] = "text/css";
const char core_content_type_js[] = "application/javascript";
const char core_content_type_json[] = "application/json";
const char core_content_type_plain[] = "text/plain";

/* Named HTTP status lines (declarations in core/http.h) */
const char core_http_status_ok[] = "200 OK";
const char core_http_status_redirect[] = "302 Found";
const char core_http_status_unauthorized[] = "401 Unauthorized";
const char core_http_status_not_found[] = "404 Not Found";
const char core_http_status_method_not_allowed[] = "405 Method Not Allowed";
const char core_http_status_unavailable[] = "503 Service Unavailable";
static const char ENCODING_GZIP[] = "Content-Encoding: gzip\r\n";
static const char NOT_FOUND_BODY[] = "not found\n";
static const char SYM_PLUGIN_INIT[] = "plugin_init";
static const char SYM_PLUGIN_HANDLE[] = "plugin_handle";
static const char PLUGIN_REQUEST_FMT[] = "GET /%.*s HTTP/1.1";
static const char HEAD_FMT[] =
    "HTTP/1.1 %s\r\nContent-Type: %s\r\n%sContent-Length: %zu\r\nConnection: close\r\n\r\n";

/* --- Request line and headers --- */

const char *core_http_request_path(core_str_view_t req, core_str_view_t *path) {
    if (req.data == NULL || path == NULL) {
        return NULL;
    }
    const char *first = memchr(req.data, ' ', req.len);
    if (first == NULL) {
        return NULL;
    }
    const size_t begin = (size_t)(first - req.data) + 1;
    const char *second = memchr(req.data + begin, ' ', req.len - begin);
    const char *stop = second != NULL ? second : req.data + req.len;
    for (const char *p = req.data + begin; p < stop; ++p) {
        if (*p == '\r') {
            stop = p;
            break;
        }
    }
    path->data = req.data + begin;
    path->len = (size_t)(stop - path->data);
    return path->data;
}

const char *core_http_request_method(core_str_view_t req, core_str_view_t *method) {
    if (req.data == NULL || method == NULL) {
        return NULL;
    }
    const char *space = memchr(req.data, ' ', req.len);
    if (space == NULL || space == req.data) {
        return NULL;
    }
    method->data = req.data;
    method->len = (size_t)(space - req.data);
    return method->data;
}

/* Case-insensitive "name:" match at the start of [line, line_end);
 * returns the value start (past the colon) or NULL. */
static const char *header_line_value(const char *line, const char *line_end, const char *name) {
    const size_t name_len = strlen(name);
    if ((size_t)(line_end - line) <= name_len || strncasecmp(line, name, name_len) != 0 ||
        line[name_len] != ':') {
        return NULL;
    }
    const char *value = line + name_len + 1;
    while (value < line_end && (*value == ' ' || *value == '\t')) {
        ++value;
    }
    return value;
}

/* Trims one trailing CR of a header value */
static size_t strip_cr(const char *value, size_t len) {
    while (len > 0 && value[len - 1] == '\r') {
        --len;
    }
    return len;
}

/* Headers end at the first blank line: lines after it are body and
 * must never be parsed as headers (a smuggled Content-Length or Cookie
 * line in the body would be honoured otherwise). */
static const char *head_block_end(core_str_view_t req) {
    for (size_t i = 0; i + 4 <= req.len; ++i) {
        if (memcmp(req.data + i, "\r\n\r\n", 4) == 0) {
            return req.data + i + 4;
        }
    }
    return req.data + req.len; /* no blank line yet: scan everything */
}

/* Finds a header line "name:" (case-insensitive); returns the value
 * start with spaces skipped and writes its length (CR stripped). */
static const char *header_value(core_str_view_t req, const char *name, size_t *value_len) {
    const char *line = req.data;
    const char *const end = head_block_end(req);
    while (line < end) {
        const char *eol = memchr(line, '\n', (size_t)(end - line));
        if (eol == NULL) {
            break;
        }
        const char *value = header_line_value(line, eol, name);
        if (value != NULL) {
            *value_len = strip_cr(value, (size_t)(eol - value));
            return value;
        }
        line = eol + 1;
    }
    return NULL;
}

int core_http_content_length(core_str_view_t req, size_t *out_len) {
    if (req.data == NULL || out_len == NULL) {
        return -1;
    }
    size_t value_len = 0;
    const char *value = header_value(req, "Content-Length", &value_len);
    if (value == NULL || value_len == 0) {
        return -1;
    }
    size_t parsed = 0;
    for (size_t i = 0; i < value_len; ++i) {
        if (value[i] < '0' || value[i] > '9') {
            return -1;
        }
        if (parsed > (SIZE_MAX - 9) / 10) {
            return -1; /* the multiply would wrap: no meaningful length */
        }
        parsed = parsed * 10 + (size_t)(value[i] - '0');
    }
    *out_len = parsed;
    return 0;
}

/* --- Response composition --- */

size_t core_http_head(char *buf, size_t cap, const struct core_http_head_spec *spec) {
    if (buf == NULL || cap == 0 || spec == NULL || spec->status == NULL ||
        spec->content_type == NULL) {
        return 0;
    }
    const char *const encoding = spec->gzip_encoded != 0 ? ENCODING_GZIP : "";
    const int written =
        snprintf(buf, cap, HEAD_FMT, spec->status, spec->content_type, encoding, spec->content_len);
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
    const struct core_http_head_spec spec = {
        .status = core_http_status_ok,
        .content_type = content_type,
        .content_len = gz_len,
        .gzip_encoded = 1,
    };
    const size_t head_len = core_http_head(conn->resp.head, sizeof(conn->resp.head), &spec);
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
    return core_http_serve_status(conn, core_http_status_not_found, NOT_FOUND_BODY);
}

/* --- Bounded composition --- */

int core_http_append(char *buf, size_t cap, size_t *pos, const char *fmt, ...) {
    if (buf == NULL || pos == NULL || fmt == NULL || *pos >= cap) {
        return -1;
    }
    va_list args;
    va_start(args, fmt);
    const int written = vsnprintf(buf + *pos, cap - *pos, fmt, args);
    va_end(args);
    if (written < 0 || (size_t)written >= cap - *pos) {
        return -1;
    }
    *pos += (size_t)written;
    return 0;
}

/* --- Pooled response bodies --- */

static char g_body_pool[CORE_BODY_SLOTS][CORE_BODY_SLOT_CAP];
static struct core_conn *g_body_owner[CORE_BODY_SLOTS];

/* Connection: close semantics: a conn is released (fd == -1) right
 * after its response, and a re-serve of the same conn replaces its
 * body. Both free the slot lazily, on the next checkout. */
static void body_reap(struct core_conn *conn) {
    for (int i = 0; i < CORE_BODY_SLOTS; ++i) {
        struct core_conn *owner = g_body_owner[i];
        if (owner != NULL && (owner == conn || owner->fd == -1)) {
            g_body_owner[i] = NULL;
        }
    }
}

char *core_http_body_checkout(struct core_conn *conn) {
    if (conn == NULL) {
        return NULL;
    }
    body_reap(conn);
    for (int i = 0; i < CORE_BODY_SLOTS; ++i) {
        if (g_body_owner[i] == NULL) {
            g_body_owner[i] = conn;
            return g_body_pool[i];
        }
    }
    return NULL;
}

static int body_belongs_to_pool(const char *body) {
    for (int i = 0; i < CORE_BODY_SLOTS; ++i) {
        if (body == g_body_pool[i]) {
            return 1;
        }
    }
    return 0;
}

int core_http_serve_body(struct core_conn *conn, const char *status, const char *content_type,
                         core_str_view_t body) {
    if (conn == NULL || body.data == NULL || !body_belongs_to_pool(body.data)) {
        return -1; /* a stack buffer can die mid-send: never serve it */
    }
    const struct core_http_head_spec spec = {
        .status = status,
        .content_type = content_type,
        .content_len = body.len,
    };
    const size_t head_len = core_http_head(conn->resp.head, sizeof(conn->resp.head), &spec);
    if (head_len == 0) {
        return -1;
    }
    conn->resp.head_len = head_len;
    conn->resp.iov[0].iov_base = conn->resp.head;
    conn->resp.iov[0].iov_len = head_len;
    conn->resp.iov[1].iov_base = (void *)body.data;
    conn->resp.iov[1].iov_len = body.len;
    conn->resp.iov_count = 2;
    conn->resp.total_len = head_len + body.len;
    return 0;
}

int core_http_serve_status(struct core_conn *conn, const char *status, const char *body) {
    if (conn == NULL || status == NULL || body == NULL) {
        return -1;
    }
    const struct core_http_head_spec spec = {
        .status = status,
        .content_type = core_content_type_plain,
        .content_len = strlen(body),
    };
    const size_t head_len = core_http_head(conn->resp.head, sizeof(conn->resp.head), &spec);
    if (head_len == 0) {
        return -1;
    }
    conn->resp.head_len = head_len;
    conn->resp.iov[0].iov_base = conn->resp.head;
    conn->resp.iov[0].iov_len = head_len;
    conn->resp.iov[1].iov_base = (void *)body;
    conn->resp.iov[1].iov_len = strlen(body);
    conn->resp.iov_count = 2;
    conn->resp.total_len = head_len + strlen(body);
    return 0;
}

/* Inserts "line\r\n" right before the final blank line of a composed
 * head. 0 ok, -1 when it does not fit (the head is left untouched). */
static int head_insert_header(char *head, size_t cap, const char *line) {
    if (head == NULL || line == NULL) {
        return -1;
    }
    const size_t head_len = strlen(head);
    if (head_len < 4 || memcmp(head + head_len - 4, "\r\n\r\n", 4) != 0) {
        return -1;
    }
    const size_t line_len = strlen(line);
    if (head_len + line_len + 2 >= cap) {
        return -1; /* keep room for the NUL */
    }
    memmove(head + head_len - 2 + line_len + 2, head + head_len - 2, 2); /* final "\r\n" */
    memcpy(head + head_len - 2, line, line_len);
    memcpy(head + head_len - 2 + line_len, "\r\n", 2);
    head[head_len + line_len + 2] = '\0';
    return 0;
}

int core_http_set_cookie(char *head, size_t cap, const struct core_http_cookie_spec *spec) {
    if (head == NULL || spec == NULL || spec->name == NULL || spec->value.data == NULL) {
        return -1;
    }
    char line[SET_COOKIE_CAP];
    const int written =
        snprintf(line, sizeof(line), "Set-Cookie: %s=%.*s; Max-Age=%d; HttpOnly; Path=/%s%s",
                 spec->name, (int)spec->value.len, spec->value.data, spec->max_age_sec,
                 spec->extra != NULL ? "; " : "", spec->extra != NULL ? spec->extra : "");
    if (written <= 0 || (size_t)written >= sizeof(line)) {
        return -1;
    }
    return head_insert_header(head, cap, line);
}

int core_http_serve_redirect(struct core_conn *conn, const char *location) {
    if (conn == NULL || location == NULL) {
        return -1;
    }
    const struct core_http_head_spec spec = {
        .status = core_http_status_redirect,
        .content_type = core_content_type_plain,
    };
    const size_t head_len = core_http_head(conn->resp.head, sizeof(conn->resp.head), &spec);
    if (head_len == 0) {
        return -1;
    }
    char line[REDIRECT_LINE_CAP];
    const int written = snprintf(line, sizeof(line), "Location: %s", location);
    if (written <= 0 || (size_t)written >= sizeof(line)) {
        return -1;
    }
    if (head_insert_header(conn->resp.head, sizeof(conn->resp.head), line) != 0) {
        return -1;
    }
    conn->resp.head_len = strlen(conn->resp.head);
    conn->resp.iov[0].iov_base = conn->resp.head;
    conn->resp.iov[0].iov_len = conn->resp.head_len;
    conn->resp.iov_count = 1;
    conn->resp.total_len = conn->resp.head_len;
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

int core_http_match_exact(core_str_view_t path, const char *route_path) {
    const size_t route_len = strlen(route_path);
    if (path.data == NULL || path.len != route_len) {
        return 0;
    }
    return memcmp(path.data, route_path, route_len) == 0 ? 1 : 0;
}

int core_http_match_prefix(core_str_view_t path, const char *prefix, core_str_view_t *seg) {
    const size_t prefix_len = strlen(prefix);
    if (path.data == NULL || seg == NULL || path.len <= prefix_len) {
        return 0;
    }
    if (memcmp(path.data, prefix, prefix_len) != 0) {
        return 0;
    }
    seg->data = path.data + prefix_len;
    seg->len = path.len - prefix_len;
    return 1;
}

int core_http_serve_asset_route(struct core_conn *conn, core_str_view_t path,
                                const struct core_http_routes *routes) {
    if (conn == NULL || path.data == NULL) {
        return -1;
    }
    if (routes == NULL) {
        return 1; /* no table: fall through */
    }
    for (size_t i = 0; i < routes->asset_count; ++i) {
        const struct core_http_asset_route *route = &routes->assets[i];
        if (core_http_match_exact(path, route->path) == 1) {
            return core_http_serve_asset(conn, route->content_type, route->gz, route->gz_len);
        }
    }
    return 1;
}

int core_http_serve_plugin(struct core_conn *conn, const struct core_plugin *plugin,
                           core_str_view_t seg) {
    if (conn == NULL || plugin == NULL) {
        return -1;
    }
    if (!core_uri_segment_valid(seg.data, seg.len)) {
        return core_http_not_found(conn);
    }
    char request[PLUGIN_REQUEST_CAP];
    const int req_len =
        snprintf(request, sizeof(request), PLUGIN_REQUEST_FMT, (int)seg.len, seg.data);
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

int core_http_route(struct core_conn *conn, const struct core_http_routes *routes) {
    if (conn == NULL) {
        return -1;
    }
    core_str_view_t path = {0};
    const char *path_data =
        core_http_request_path(CORE_SV_BUF(conn->req_buf, conn->req_len), &path);
    if (path_data == NULL) {
        return core_http_not_found(conn);
    }
    /* routes and prefix segments match the clean path, without "?query" */
    path.len = core_http_path_only(path);
    if (core_http_serve_asset_route(conn, path, routes) == 0) {
        return 0;
    }
    for (size_t i = 0; routes != NULL && i < routes->plugin_count; ++i) {
        core_str_view_t seg = {0};
        if (core_http_match_prefix(path, routes->plugins[i].prefix, &seg) == 1) {
            return core_http_serve_plugin(conn, routes->plugins[i].plugin, seg);
        }
    }
    return core_http_not_found(conn);
}

/* --- Query string and forms --- */

size_t core_http_path_only(core_str_view_t path) {
    if (path.data == NULL) {
        return 0;
    }
    const char *query = memchr(path.data, '?', path.len);
    return query == NULL ? path.len : (size_t)(query - path.data);
}

/* Bounds of the value in a "name=value" pair inside [pair, stop),
 * with spaces around '=' and at the edges trimmed; 1 on a name hit. */
static int pair_match(core_str_view_t pair, const char *name, core_str_view_t *val) {
    const char *start = pair.data;
    const char *stop = pair.data + pair.len;
    while (start < stop && *start == ' ') {
        ++start;
    }
    const char *eq = memchr(start, '=', (size_t)(stop - start));
    if (eq == NULL) {
        return 0;
    }
    const size_t name_len = strlen(name);
    const char *name_end = eq;
    while (name_end > start && name_end[-1] == ' ') {
        --name_end;
    }
    if ((size_t)(name_end - start) != name_len || memcmp(start, name, name_len) != 0) {
        return 0;
    }
    const char *value = eq + 1;
    while (value < stop && *value == ' ') {
        ++value;
    }
    const char *value_end = stop;
    while (value_end > value && value_end[-1] == ' ') {
        --value_end;
    }
    val->data = value;
    val->len = (size_t)(value_end - value);
    return 1;
}

/* Walks separator-delimited pairs in span; 1 on the first name hit. */
static int pair_walk(core_str_view_t span, char separator, const char *name, core_str_view_t *val) {
    const char *pair = span.data;
    const char *const end = span.data + span.len;
    while (pair < end) {
        const char *sep = memchr(pair, separator, (size_t)(end - pair));
        const char *stop = sep != NULL ? sep : end;
        if (pair_match(CORE_SV_BUF(pair, (size_t)(stop - pair)), name, val) == 1) {
            return 1;
        }
        if (sep == NULL) {
            break;
        }
        pair = sep + 1;
    }
    return 0;
}

int core_http_query_param(core_str_view_t path, const char *name, core_str_view_t *val) {
    if (path.data == NULL || name == NULL || val == NULL) {
        return 0;
    }
    const size_t clean = core_http_path_only(path);
    const char *query = memchr(path.data, '?', path.len);
    if (query == NULL) {
        return 0;
    }
    const core_str_view_t qs = {.data = query + 1, .len = path.len - clean - 1};
    return pair_walk(qs, '&', name, val);
}

int core_http_cookie(core_str_view_t req, const char *name, core_str_view_t *val) {
    if (req.data == NULL || name == NULL || val == NULL) {
        return 0;
    }
    size_t header_len = 0;
    const char *header = header_value(req, "Cookie", &header_len);
    if (header == NULL) {
        return 0;
    }
    return pair_walk(CORE_SV_BUF(header, header_len), ';', name, val);
}

int core_http_form_field(core_str_view_t body, const char *name, core_str_view_t *val) {
    if (body.data == NULL || name == NULL || val == NULL) {
        return 0;
    }
    return pair_walk(body, '&', name, val);
}

long long core_http_query_long(core_str_view_t path, const char *name, long long def,
                               struct core_http_range range) {
    if (range.min > range.max) {
        return def;
    }
    core_str_view_t val = {0};
    if (core_http_query_param(path, name, &val) != 1 || val.len == 0 ||
        val.len > QUERY_LONG_DIGITS_MAX) {
        return def;
    }
    long long parsed = 0;
    for (size_t i = 0; i < val.len; ++i) {
        if (val.data[i] < '0' || val.data[i] > '9') {
            return def;
        }
        parsed = parsed * 10 + (val.data[i] - '0');
    }
    if (parsed <= 0) {
        return def; /* zero reads as "not set" */
    }
    if (parsed < range.min) {
        return range.min;
    }
    return parsed > range.max ? range.max : parsed;
}

/* Wraps the plugin body with HTTP headers */
int core_http_wrap_body(struct plugin_response_t *resp) {
    if (resp == NULL) {
        return -1;
    }
    const size_t body_len = resp->head_len;
    size_t tail_len = 0;
    for (int i = 1; i < resp->iov_count; ++i) {
        tail_len += resp->iov[i].iov_len;
    }
    const struct core_http_head_spec spec = {
        .status = core_http_status_ok,
        .content_type = core_content_type_html,
        .content_len = body_len + tail_len,
    };
    char headers[WRAP_HEADERS_CAP];
    const size_t head_len = core_http_head(headers, sizeof(headers), &spec);
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
