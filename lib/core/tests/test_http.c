#include "../src/http.c"
#include "utest.h"
#include <string.h>

/* Named capacities and gzip magic instead of bare numbers */
enum {
    HEAD_CAP = 256,    /* plenty for the fixed header block */
    HEAD_CAP_TINY = 8, /* deliberately too small */
    GZIP_MAGIC_HI = 0x1f,
    GZIP_MAGIC_LO = 0x8b,
};

#define TAIL_TAG ((void *)0x99)
#define TAIL_LEN 3

/* --- Request line --- */

UTEST(path, simple) {
    const core_str_view_t req = CORE_SV("GET /a/b HTTP/1.1");
    core_str_view_t path = {0};
    ASSERT_NE(NULL, (void *)core_http_request_path(req, &path));
    ASSERT_EQ((size_t)4, path.len);
    ASSERT_EQ(0, strncmp("/a/b", path.data, path.len));
}

UTEST(path, stops_at_cr) {
    const core_str_view_t req = CORE_SV("GET /x\r\nHost: a");
    core_str_view_t path = {0};
    ASSERT_NE(NULL, (void *)core_http_request_path(req, &path));
    ASSERT_EQ((size_t)2, path.len);
    ASSERT_EQ(0, strncmp("/x", path.data, path.len));
}

UTEST(path, no_space) {
    const core_str_view_t req = CORE_SV("GETX");
    ASSERT_EQ(NULL, (void *)core_http_request_path(req, NULL));
}
UTEST(path, null_req) {
    const core_str_view_t req = {0};
    ASSERT_EQ(NULL, (void *)core_http_request_path(req, NULL));
}

/* --- Response composition --- */

UTEST(head, ok_with_gzip) {
    char buf[HEAD_CAP];
    const struct core_http_head_spec spec = {
        .status = core_http_status_ok,
        .content_type = "text/html",
        .content_len = 42,
        .gzip_encoded = 1,
    };
    const size_t head_len = core_http_head(buf, sizeof(buf), &spec);
    ASSERT_TRUE(head_len > 0);
    ASSERT_EQ(0, strncmp(buf, "HTTP/1.1 200 OK\r\n", sizeof("HTTP/1.1 200 OK\r\n") - 1));
    ASSERT_NE(NULL, strstr(buf, "Content-Encoding: gzip\r\n"));
    ASSERT_NE(NULL, strstr(buf, "Content-Length: 42\r\n"));
    ASSERT_NE(NULL, strstr(buf, "Connection: close\r\n\r\n"));
}

UTEST(head, no_gzip_line_without_flag) {
    char buf[HEAD_CAP];
    const struct core_http_head_spec spec = {
        .status = core_http_status_not_found,
        .content_type = "text/plain",
        .content_len = 10,
    };
    const size_t head_len = core_http_head(buf, sizeof(buf), &spec);
    ASSERT_TRUE(head_len > 0);
    ASSERT_EQ(NULL, strstr(buf, "Content-Encoding"));
    ASSERT_NE(NULL, strstr(buf, core_http_status_not_found));
}

UTEST(head, too_small_cap_is_zero) {
    char buf[HEAD_CAP_TINY];
    const struct core_http_head_spec spec = {
        .status = core_http_status_ok,
        .content_type = "text/html",
        .content_len = 1,
    };
    ASSERT_EQ((size_t)0, core_http_head(buf, sizeof(buf), &spec));
}
UTEST(head, null_buf) {
    const struct core_http_head_spec spec = {
        .status = core_http_status_ok,
        .content_type = "t",
        .content_len = 1,
    };
    ASSERT_EQ((size_t)0, core_http_head(NULL, HEAD_CAP, &spec));
}

UTEST(asset, serve_sets_head_and_iov) {
    static const unsigned char gz[] = {GZIP_MAGIC_HI, GZIP_MAGIC_LO, 0x00, 0x01};
    struct core_conn conn;
    memset(&conn, 0, sizeof(conn));
    conn.fd = -1;
    ASSERT_EQ(0, core_http_serve_asset(&conn, "text/html", gz, sizeof(gz)));
    ASSERT_NE(NULL, strstr(conn.resp.head, "Content-Encoding: gzip\r\n"));
    ASSERT_NE(NULL, strstr(conn.resp.head, "Content-Length: 4\r\n"));
    ASSERT_EQ((void *)gz, conn.resp.iov[1].iov_base);
    ASSERT_EQ(sizeof(gz), conn.resp.iov[1].iov_len);
    ASSERT_EQ(2, conn.resp.iov_count);
    ASSERT_EQ(conn.resp.head_len + sizeof(gz), conn.resp.total_len);
}

UTEST(asset, null_args) { ASSERT_EQ(-1, core_http_serve_asset(NULL, "text/plain", "x", 1)); }

UTEST(notfound, sets_404) {
    struct core_conn conn;
    memset(&conn, 0, sizeof(conn));
    ASSERT_EQ(0, core_http_not_found(&conn));
    ASSERT_NE(NULL, strstr(conn.resp.head, core_http_status_not_found));
    ASSERT_EQ(2, conn.resp.iov_count);
    ASSERT_EQ(0, strncmp("not found\n", (const char *)conn.resp.iov[1].iov_base,
                         sizeof("not found\n") - 1));
}

UTEST(wrap, prepends_headers_and_keeps_tail) {
    struct plugin_response_t resp;
    memset(&resp, 0, sizeof(resp));
    memcpy(resp.head, "hello", sizeof("hello") - 1);
    resp.head_len = sizeof("hello") - 1;
    resp.iov[0].iov_base = resp.head;
    resp.iov[0].iov_len = resp.head_len;
    resp.iov[1].iov_base = TAIL_TAG; /* fake asset part */
    resp.iov[1].iov_len = TAIL_LEN;
    resp.iov_count = 2;
    resp.total_len = resp.head_len + TAIL_LEN;

    ASSERT_EQ(0, core_http_wrap_body(&resp));
    ASSERT_EQ(0, strncmp(resp.head, "HTTP/1.1 200 OK\r\n", sizeof("HTTP/1.1 200 OK\r\n") - 1));
    /* Content-Length covers body + tail */
    ASSERT_NE(NULL, strstr(resp.head, "Content-Length: 8\r\n"));
    /* body shifted intact right after the header block */
    const size_t headers_len = resp.head_len - (sizeof("hello") - 1);
    ASSERT_EQ(0, memcmp(resp.head + headers_len, "hello", sizeof("hello") - 1));
    ASSERT_EQ(resp.head, resp.iov[0].iov_base);
    ASSERT_EQ(resp.head_len, resp.iov[0].iov_len);
    ASSERT_EQ(TAIL_TAG, resp.iov[1].iov_base); /* tail preserved */
    ASSERT_EQ(resp.head_len + TAIL_LEN, resp.total_len);
}

UTEST(wrap, overflow_rejected) {
    struct plugin_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.head_len = sizeof(resp.head); /* no room for headers */
    resp.iov_count = 1;
    resp.iov[0].iov_base = resp.head;
    resp.iov[0].iov_len = resp.head_len;
    ASSERT_EQ(-1, core_http_wrap_body(&resp));
}

#undef TAIL_TAG
#undef TAIL_LEN

UTEST(hostapi, default_fills_pointers) {
    struct host_api_t api;
    memset(&api, 0, sizeof(api));
    core_host_api_default(&api);
    ASSERT_TRUE(api.snprintf != NULL);
    ASSERT_TRUE(api.memcpy != NULL);
}
UTEST(hostapi, null_is_noop) { core_host_api_default(NULL); }

UTEST(plugin, open_missing_file_fails) {
    struct core_plugin plugin;
    struct host_api_t api;
    memset(&plugin, 0, sizeof(plugin));
    memset(&api, 0, sizeof(api));
    ASSERT_EQ(-1, core_plugin_open(&plugin, "/nonexistent.so", &api));
}
UTEST(plugin, open_null_args_fail) { ASSERT_EQ(-1, core_plugin_open(NULL, "x", NULL)); }

/* --- Routing primitives (match_exact / match_prefix / serve_plugin / table) --- */

UTEST(match, exact_hit) {
    ASSERT_EQ(1, core_http_match_exact(CORE_SV("/htmx.min.js"), "/htmx.min.js"));
}
UTEST(match, exact_longer_path) {
    ASSERT_EQ(0, core_http_match_exact(CORE_SV("/htmx.min.jsx"), "/htmx.min.js"));
}
UTEST(match, exact_shorter_path) {
    ASSERT_EQ(0, core_http_match_exact(CORE_SV("/htmx"), "/htmx.min.js"));
}
UTEST(match, exact_different) {
    ASSERT_EQ(0, core_http_match_exact(CORE_SV("/other"), "/htmx.min.js"));
}

UTEST(match, prefix_extracts_tail) {
    core_str_view_t seg = {0};
    ASSERT_EQ(1, core_http_match_prefix(CORE_SV("/plugin/hello"), "/plugin/", &seg));
    ASSERT_EQ((size_t)5, seg.len);
    ASSERT_EQ(0, strncmp("hello", seg.data, seg.len));
}
UTEST(match, prefix_no_match) {
    core_str_view_t seg = {.len = 9}; /* sentinel: no match must not touch */
    ASSERT_EQ(0, core_http_match_prefix(CORE_SV("/other/x"), "/plugin/", &seg));
    ASSERT_EQ((size_t)9, seg.len);
}
UTEST(match, prefix_equal_length_no_tail) {
    /* the prefix itself is not a route: an empty segment is not allowed */
    core_str_view_t seg = {.len = 9};
    ASSERT_EQ(0, core_http_match_prefix(CORE_SV("/plugin/"), "/plugin/", &seg));
    ASSERT_EQ((size_t)9, seg.len);
}

/* Fake plugin tail tag: a constant expression is required in the initializer */
#define TAIL_TAG_REDEF ((void *)0x77)
#define TAIL_LEN_REDEF 3

/* Fake plugin for serve_plugin: captures the request, answers with a body + tail */
static char g_captured_request[64];

static int capture_handle(const char *request, size_t request_len, struct plugin_response_t *resp) {
    memcpy(g_captured_request, request, request_len);
    g_captured_request[request_len] = '\0';
    memcpy(resp->head, "hi", sizeof("hi") - 1);
    resp->head_len = sizeof("hi") - 1;
    resp->iov[0].iov_base = resp->head;
    resp->iov[0].iov_len = resp->head_len;
    resp->iov[1].iov_base = TAIL_TAG_REDEF;
    resp->iov[1].iov_len = TAIL_LEN_REDEF;
    resp->iov_count = 2;
    resp->total_len = resp->head_len + TAIL_LEN_REDEF;
    return 0;
}

static int decline_handle(const char *request, size_t request_len, struct plugin_response_t *resp) {
    (void)request;
    (void)request_len;
    (void)resp;
    return -1;
}

UTEST(serve_plugin, composes_request_and_wraps_fragment) {
    struct core_conn conn;
    struct core_plugin plugin = {.handle = capture_handle};
    memset(&conn, 0, sizeof(conn));
    ASSERT_EQ(0, core_http_serve_plugin(&conn, &plugin, CORE_SV("hello")));
    /* the plugin received a composed single-segment request */
    ASSERT_EQ(0, strcmp("GET /hello HTTP/1.1", g_captured_request));
    /* the client got headers + body, the tail iov is dropped (fragment pattern) */
    ASSERT_EQ(0, strncmp("HTTP/1.1 200 OK\r\n", conn.resp.head, sizeof("HTTP/1.1 200 OK\r\n") - 1));
    ASSERT_NE(NULL, strstr(conn.resp.head, "Content-Length: 2\r\n"));
    ASSERT_EQ(1, conn.resp.iov_count);
    ASSERT_EQ(conn.resp.head_len, conn.resp.total_len);
}

UTEST(serve_plugin, invalid_segment_is_404) {
    struct core_conn conn;
    struct core_plugin plugin = {.handle = capture_handle};
    memset(&conn, 0, sizeof(conn));
    ASSERT_EQ(0, core_http_serve_plugin(&conn, &plugin, CORE_SV("..")));
    ASSERT_NE(NULL, strstr(conn.resp.head, core_http_status_not_found));
}

UTEST(serve_plugin, plugin_decline_is_404) {
    struct core_conn conn;
    struct core_plugin plugin = {.handle = decline_handle};
    memset(&conn, 0, sizeof(conn));
    ASSERT_EQ(0, core_http_serve_plugin(&conn, &plugin, CORE_SV("x")));
    ASSERT_NE(NULL, strstr(conn.resp.head, core_http_status_not_found));
}

UTEST(serve_plugin, null_args) { ASSERT_EQ(-1, core_http_serve_plugin(NULL, NULL, CORE_SV("x"))); }

UTEST(route_table, serves_matching_route) {
    static const unsigned char gz[] = {0x1f, 0x8b};
    static const struct core_http_asset_route assets[] = {
        {"/style.css", core_content_type_css, gz, sizeof(gz)},
    };
    const struct core_http_routes routes = {
        .assets = assets,
        .asset_count = CORE_ARRAY_LEN(assets),
    };
    struct core_conn conn;
    memset(&conn, 0, sizeof(conn));
    ASSERT_EQ(0, core_http_serve_asset_route(&conn, CORE_SV("/style.css"), &routes));
    ASSERT_NE(NULL, strstr(conn.resp.head, "Content-Type: "));
    ASSERT_NE(NULL, strstr(conn.resp.head, core_content_type_css));
}

UTEST(route_table, no_match_falls_through) {
    static const struct core_http_asset_route assets[] = {
        {"/style.css", core_content_type_css, "x", 1},
    };
    const struct core_http_routes routes = {
        .assets = assets,
        .asset_count = CORE_ARRAY_LEN(assets),
    };
    struct core_conn conn;
    memset(&conn, 0, sizeof(conn));
    ASSERT_EQ(1, core_http_serve_asset_route(&conn, CORE_SV("/other"), &routes));
}

UTEST(route_table, empty_table_falls_through) {
    struct core_conn conn;
    memset(&conn, 0, sizeof(conn));
    ASSERT_EQ(1, core_http_serve_asset_route(&conn, CORE_SV("/"), NULL));
}

#undef TAIL_TAG_REDEF
#undef TAIL_LEN_REDEF

/* --- Full router (core_http_route) --- */

/* Route a request string through the router into a fresh conn */
static int route_request(const char *request, const struct core_http_routes *routes,
                         struct core_conn *conn) {
    memset(conn, 0, sizeof(*conn));
    memcpy(conn->req_buf, request, strlen(request));
    conn->req_len = strlen(request);
    return core_http_route(conn, routes);
}

UTEST(router, asset_hit) {
    static const unsigned char gz[] = {0x1f, 0x8b};
    static const struct core_http_asset_route assets[] = {
        {"/", "text/html", gz, sizeof(gz)},
    };
    const struct core_http_routes routes = {
        .assets = assets,
        .asset_count = CORE_ARRAY_LEN(assets),
    };
    struct core_conn conn;
    ASSERT_EQ(0, route_request("GET / HTTP/1.1\r\n\r\n", &routes, &conn));
    ASSERT_NE(NULL, strstr(conn.resp.head, "Content-Type: text/html"));
}

UTEST(router, plugin_hit) {
    static const struct core_plugin plugin = {.handle = capture_handle};
    static const struct core_http_plugin_route plugins[] = {
        {"/plugin/", &plugin},
    };
    const struct core_http_routes routes = {
        .plugins = plugins,
        .plugin_count = CORE_ARRAY_LEN(plugins),
    };
    struct core_conn conn;
    ASSERT_EQ(0, route_request("GET /plugin/hello HTTP/1.1\r\n\r\n", &routes, &conn));
    ASSERT_EQ(0, strcmp("GET /hello HTTP/1.1", g_captured_request));
    ASSERT_EQ(0, strncmp("HTTP/1.1 200 OK\r\n", conn.resp.head, sizeof("HTTP/1.1 200 OK\r\n") - 1));
}

UTEST(router, unknown_path_is_404) {
    struct core_conn conn;
    ASSERT_EQ(0, route_request("GET /nope HTTP/1.1\r\n\r\n", NULL, &conn));
    ASSERT_NE(NULL, strstr(conn.resp.head, core_http_status_not_found));
}

UTEST(router, request_without_path_is_404) {
    struct core_conn conn;
    ASSERT_EQ(0, route_request("GARBAGE\r\n\r\n", NULL, &conn));
    ASSERT_NE(NULL, strstr(conn.resp.head, core_http_status_not_found));
}

UTEST(router, null_conn) { ASSERT_EQ(-1, core_http_route(NULL, NULL)); }

UTEST(router, route_exact_with_query) {
    static const unsigned char gz[] = {0x1f, 0x8b};
    static const struct core_http_asset_route assets[] = {
        {"/", "text/html", gz, sizeof(gz)},
    };
    const struct core_http_routes routes = {
        .assets = assets,
        .asset_count = CORE_ARRAY_LEN(assets),
    };
    struct core_conn conn;
    ASSERT_EQ(0, route_request("GET /?x=1 HTTP/1.1\r\n\r\n", &routes, &conn));
    ASSERT_NE(NULL, strstr(conn.resp.head, core_http_status_ok));
}

/* --- Query string (path_only / query_param / router on clean path) --- */

UTEST(query, path_only_strips_query) {
    ASSERT_EQ((size_t)(sizeof("/api/metrics") - 1),
              core_http_path_only(CORE_SV("/api/metrics?range=3600")));
}
UTEST(query, path_only_without_query) {
    ASSERT_EQ((size_t)(sizeof("/api/metrics") - 1), core_http_path_only(CORE_SV("/api/metrics")));
}
UTEST(query, path_only_question_only) {
    ASSERT_EQ((size_t)0, core_http_path_only(CORE_SV("?")));
    ASSERT_EQ((size_t)1, core_http_path_only(CORE_SV("/?")));
}
UTEST(query, path_only_empty) { ASSERT_EQ((size_t)0, core_http_path_only(CORE_SV(""))); }

UTEST(query, param_found) {
    core_str_view_t val = {0};
    ASSERT_EQ(1, core_http_query_param(CORE_SV("/api/metrics?range=3600"), "range", &val));
    ASSERT_EQ((size_t)(sizeof("3600") - 1), val.len);
    ASSERT_EQ(0, strncmp("3600", val.data, val.len));
}
UTEST(query, param_absent) {
    core_str_view_t val = {.len = 9}; /* sentinel: absent must not touch */
    ASSERT_EQ(0, core_http_query_param(CORE_SV("/api/metrics?other=1"), "range", &val));
    ASSERT_EQ((size_t)9, val.len);
}
UTEST(query, param_no_query_string) {
    core_str_view_t val = {.len = 9};
    ASSERT_EQ(0, core_http_query_param(CORE_SV("/api/metrics"), "range", &val));
    ASSERT_EQ((size_t)9, val.len);
}
UTEST(query, param_empty_value) {
    core_str_view_t val = {.len = 1}; /* deliberately wrong: overwritten with 0 */
    ASSERT_EQ(1, core_http_query_param(CORE_SV("/api/metrics?range="), "range", &val));
    ASSERT_EQ((size_t)0, val.len);
}
UTEST(query, param_second_of_two) {
    core_str_view_t val = {0};
    ASSERT_EQ(1, core_http_query_param(CORE_SV("/api/metrics?a=1&range=7"), "range", &val));
    ASSERT_EQ((size_t)1, val.len);
    ASSERT_EQ('7', val.data[0]);
}

UTEST(querylong, found) {
    const struct core_http_range range = {.min = 1, .max = 86400};
    ASSERT_EQ(60, core_http_query_long(CORE_SV("/api/metrics?range=60"), "range", 3600, range));
}
UTEST(querylong, absent_garbage_empty_default) {
    const struct core_http_range range = {.min = 1, .max = 86400};
    ASSERT_EQ(3600, core_http_query_long(CORE_SV("/x"), "range", 3600, range));
    ASSERT_EQ(3600, core_http_query_long(CORE_SV("/x?range=abc"), "range", 3600, range));
    ASSERT_EQ(3600, core_http_query_long(CORE_SV("/x?range="), "range", 3600, range));
}
UTEST(querylong, zero_means_default) {
    const struct core_http_range range = {.min = 1, .max = 86400};
    ASSERT_EQ(3600, core_http_query_long(CORE_SV("/x?range=0"), "range", 3600, range));
}
UTEST(querylong, clamped_to_max) {
    const struct core_http_range range = {.min = 1, .max = 86400};
    ASSERT_EQ(86400, core_http_query_long(CORE_SV("/x?range=999999999"), "range", 3600, range));
    ASSERT_EQ(86400,
              core_http_query_long(CORE_SV("/x?range=123456789012345"), "range", 3600, range));
}

/* --- Bounded composition (core_http_append) --- */

UTEST(append, writes_and_advances) {
    char buf[32];
    size_t pos = 0;
    ASSERT_EQ(0, core_http_append(buf, sizeof(buf), &pos, "{\"ts\":["));
    ASSERT_EQ((size_t)(sizeof("{\"ts\":[") - 1), pos);
    ASSERT_EQ(0, core_http_append(buf, sizeof(buf), &pos, "%d", 42));
    ASSERT_EQ(0, strncmp(buf, "{\"ts\":[42", pos));
}
UTEST(append, overflow_rejected_pos_unchanged) {
    char buf[8];
    size_t pos = 0;
    ASSERT_EQ(0, core_http_append(buf, sizeof(buf), &pos, "1234"));
    ASSERT_EQ(-1, core_http_append(buf, sizeof(buf), &pos, "%s", "5678"));
    ASSERT_EQ((size_t)4, pos); /* the failed append moves nothing */
    ASSERT_EQ(0, strncmp(buf, "1234", pos));
}
UTEST(append, null_args) {
    char buf[8];
    size_t pos = 0;
    ASSERT_EQ(-1, core_http_append(NULL, sizeof(buf), &pos, "x"));
    ASSERT_EQ(-1, core_http_append(buf, sizeof(buf), NULL, "x"));
}

/* --- Pooled response bodies (contract: conns live in static storage) --- */

static struct core_conn g_body_conn;
static struct core_conn g_body_extra;
static struct core_conn g_body_owners[CORE_BODY_SLOTS];

UTEST(body, checkout_and_serve) {
    memset(&g_body_conn, 0, sizeof(g_body_conn));
    g_body_conn.fd = 9;
    char *slot = core_http_body_checkout(&g_body_conn);
    ASSERT_NE(NULL, slot);
    memcpy(slot, "[1,2]", sizeof("[1,2]") - 1);
    ASSERT_EQ(0, core_http_serve_body(&g_body_conn, core_http_status_ok, core_content_type_json,
                                      CORE_SV_BUF(slot, sizeof("[1,2]") - 1)));
    ASSERT_NE(NULL, strstr(g_body_conn.resp.head, core_http_status_ok));
    ASSERT_NE(NULL, strstr(g_body_conn.resp.head, core_content_type_json));
    ASSERT_NE(NULL, strstr(g_body_conn.resp.head, "Content-Length: 5\r\n"));
    ASSERT_EQ((void *)slot, g_body_conn.resp.iov[1].iov_base);
    ASSERT_EQ((size_t)(sizeof("[1,2]") - 1), g_body_conn.resp.iov[1].iov_len);
    ASSERT_EQ(g_body_conn.resp.head_len + sizeof("[1,2]") - 1, g_body_conn.resp.total_len);
    g_body_conn.fd = -1; /* released */
}

UTEST(body, reserve_same_conn_reuses_slot) {
    memset(&g_body_conn, 0, sizeof(g_body_conn));
    g_body_conn.fd = 10;
    char *first = core_http_body_checkout(&g_body_conn);
    ASSERT_NE(NULL, first);
    char *second = core_http_body_checkout(&g_body_conn); /* re-serve frees the old slot */
    ASSERT_EQ(first, second);
    g_body_conn.fd = -1;
}

UTEST(body, exhaustion_returns_null) {
    memset(g_body_owners, 0, sizeof(g_body_owners));
    for (int i = 0; i < CORE_BODY_SLOTS; ++i) {
        g_body_owners[i].fd = 20 + i;
        ASSERT_NE(NULL, core_http_body_checkout(&g_body_owners[i]));
    }
    struct core_conn *extra = &g_body_extra; /* distinct live conn, no slot of its own */
    memset(extra, 0, sizeof(*extra));
    extra->fd = 50;
    ASSERT_EQ(NULL, core_http_body_checkout(extra));
    extra->fd = -1;
    for (int i = 0; i < CORE_BODY_SLOTS; ++i) {
        g_body_owners[i].fd = -1;
    }
}

UTEST(body, released_conn_frees_slot) {
    memset(g_body_owners, 0, sizeof(g_body_owners));
    for (int i = 0; i < CORE_BODY_SLOTS; ++i) {
        g_body_owners[i].fd = 30 + i;
        ASSERT_NE(NULL, core_http_body_checkout(&g_body_owners[i]));
    }
    g_body_owners[3].fd = -1; /* the loop released this conn */
    ASSERT_NE(NULL, core_http_body_checkout(&g_body_owners[0]));
    for (int i = 0; i < CORE_BODY_SLOTS; ++i) {
        g_body_owners[i].fd = -1;
    }
}

UTEST(body, serve_rejects_foreign_pointer) {
    memset(&g_body_conn, 0, sizeof(g_body_conn));
    g_body_conn.fd = 11;
    char stack_body[8] = "nope";
    ASSERT_EQ(-1, core_http_serve_body(&g_body_conn, core_http_status_ok, core_content_type_json,
                                       CORE_SV_BUF(stack_body, sizeof(stack_body))));
    g_body_conn.fd = -1;
}

UTEST(status, serves_error_with_body) {
    struct core_conn conn;
    memset(&conn, 0, sizeof(conn));
    ASSERT_EQ(0, core_http_serve_status(&conn, core_http_status_unavailable, "busy\n"));
    ASSERT_NE(NULL, strstr(conn.resp.head, core_http_status_unavailable));
    ASSERT_NE(NULL, strstr(conn.resp.head, "Content-Length: 5\r\n"));
    ASSERT_EQ(2, conn.resp.iov_count);
    ASSERT_EQ(0, strncmp("busy\n", (const char *)conn.resp.iov[1].iov_base, sizeof("busy\n") - 1));
}

/* --- Request primitives for the app layer --- */

static const char POST_FORM_REQUEST[] = "POST /login HTTP/1.1\r\n"
                                        "Host: x\r\n"
                                        "Content-Type: application/x-www-form-urlencoded\r\n"
                                        "Content-Length: 15\r\n"
                                        "Cookie: theme=dark; app_session=abc123\r\n"
                                        "\r\n"
                                        "password=sekret";

UTEST(method, slices_first_word) {
    core_str_view_t method = {0};
    const char *method_data = core_http_request_method(
        CORE_SV_BUF(POST_FORM_REQUEST, strlen(POST_FORM_REQUEST)), &method);
    ASSERT_NE(NULL, method_data);
    ASSERT_EQ((size_t)4, method.len);
    ASSERT_EQ(0, strncmp("POST", method.data, method.len));
}
UTEST(method, get_request) {
    core_str_view_t method = {0};
    ASSERT_NE(NULL, (void *)core_http_request_method(CORE_SV("GET / HTTP/1.1\r\n"), &method));
    ASSERT_EQ((size_t)3, method.len);
}
UTEST(method, no_space_is_null) {
    ASSERT_EQ(NULL, (void *)core_http_request_method(CORE_SV("GARBAGE\r\n"), NULL));
}

UTEST(clen, found) {
    size_t len = 0;
    ASSERT_EQ(0, core_http_content_length(CORE_SV_BUF(POST_FORM_REQUEST, strlen(POST_FORM_REQUEST)),
                                          &len));
    ASSERT_EQ((size_t)15, len);
}
UTEST(clen, absent) {
    size_t len = 0;
    ASSERT_EQ(-1, core_http_content_length(CORE_SV("GET / HTTP/1.1\r\nHost: a\r\n\r\n"), &len));
}
UTEST(clen, garbage_value) {
    size_t len = 0;
    ASSERT_EQ(-1, core_http_content_length(
                      CORE_SV("POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n"), &len));
}

UTEST(cookie, finds_pair) {
    core_str_view_t val = {0};
    ASSERT_EQ(1, core_http_cookie(CORE_SV_BUF(POST_FORM_REQUEST, strlen(POST_FORM_REQUEST)),
                                  "app_session", &val));
    ASSERT_EQ((size_t)6, val.len);
    ASSERT_EQ(0, strncmp("abc123", val.data, val.len));
}
UTEST(cookie, finds_first_pair) {
    core_str_view_t val = {0};
    ASSERT_EQ(1, core_http_cookie(CORE_SV_BUF(POST_FORM_REQUEST, strlen(POST_FORM_REQUEST)),
                                  "theme", &val));
    ASSERT_EQ((size_t)4, val.len);
    ASSERT_EQ(0, strncmp("dark", val.data, val.len));
}
UTEST(headers, content_length_overflow_rejected) {
    /* 2^64 - 6 wraps to 14 without an overflow guard */
    const core_str_view_t req =
        CORE_SV("POST /x HTTP/1.1\r\nContent-Length: 18446744073709551630\r\n\r\n");
    size_t len = 0;
    ASSERT_EQ(-1, core_http_content_length(req, &len));
}

UTEST(headers, body_line_is_not_a_header) {
    /* a header-shaped line after the blank line is body, never a header */
    const core_str_view_t req = CORE_SV("POST /x HTTP/1.1\r\n\r\nContent-Length: 5\r\n");
    size_t len = 77;
    ASSERT_EQ(-1, core_http_content_length(req, &len));
}

UTEST(cookie, body_cookie_line_ignored) {
    const core_str_view_t req = CORE_SV("GET / HTTP/1.1\r\n\r\nCookie: app_session=zz\r\n");
    core_str_view_t val = {0};
    ASSERT_EQ(0, core_http_cookie(req, "app_session", &val));
}

UTEST(cookie, absent) {
    core_str_view_t val = {.len = 9}; /* sentinel: absent must not touch */
    ASSERT_EQ(0, core_http_cookie(CORE_SV_BUF(POST_FORM_REQUEST, strlen(POST_FORM_REQUEST)),
                                  "other", &val));
    ASSERT_EQ((size_t)9, val.len);
}
UTEST(cookie, spaces_trimmed) {
    core_str_view_t val = {0};
    const core_str_view_t req = CORE_SV("GET / HTTP/1.1\r\nCookie:  a = b ; c=d\r\n\r\n");
    ASSERT_EQ(1, core_http_cookie(req, "a", &val));
    ASSERT_EQ((size_t)1, val.len);
    ASSERT_EQ('b', val.data[0]);
    ASSERT_EQ(1, core_http_cookie(req, "c", &val));
    ASSERT_EQ((size_t)1, val.len);
    ASSERT_EQ('d', val.data[0]);
}

UTEST(setck, inserts_before_blank_line) {
    char head[HEAD_CAP];
    const struct core_http_head_spec spec = {
        .status = core_http_status_redirect,
        .content_type = core_content_type_plain,
    };
    ASSERT_TRUE(core_http_head(head, sizeof(head), &spec) > 0);
    const struct core_http_cookie_spec cookie = {
        .name = "app_session",
        .value = CORE_SV("tok123"),
        .max_age_sec = 86400,
    };
    ASSERT_EQ(0, core_http_set_cookie(head, sizeof(head), &cookie));
    ASSERT_NE(NULL,
              strstr(head, "Set-Cookie: app_session=tok123; Max-Age=86400; HttpOnly; Path=/"));
    /* the new header must sit before the final blank line */
    const char *inserted = strstr(head, "Set-Cookie:");
    const char *blank = strstr(head, "\r\n\r\n");
    ASSERT_TRUE(inserted != NULL && blank != NULL && inserted < blank);
    /* exactly one blank line, at the very end */
    ASSERT_EQ(0, strcmp(head + strlen(head) - 4, "\r\n\r\n"));
}
UTEST(setck, tight_cap_rejected) {
    char head[128];
    const struct core_http_head_spec spec = {
        .status = core_http_status_redirect,
        .content_type = core_content_type_plain,
    };
    ASSERT_TRUE(core_http_head(head, sizeof(head), &spec) > 0);
    const char before[128];
    memcpy((void *)before, head, sizeof(before));
    const struct core_http_cookie_spec cookie = {
        .name = "app_session",
        .value = CORE_SV("a-very-long-token-value-that-will-not-fit"),
        .max_age_sec = 86400,
    };
    ASSERT_EQ(-1, core_http_set_cookie(head, sizeof(head), &cookie));
    ASSERT_EQ(0, memcmp(before, head, sizeof(before))); /* untouched on failure */
}

UTEST(form, finds_field) {
    core_str_view_t val = {0};
    ASSERT_EQ(1, core_http_form_field(CORE_SV("password=sekret"), "password", &val));
    ASSERT_EQ((size_t)6, val.len);
    ASSERT_EQ(0, strncmp("sekret", val.data, val.len));
}
UTEST(form, second_of_two) {
    core_str_view_t val = {0};
    ASSERT_EQ(1, core_http_form_field(CORE_SV("a=1&password=x"), "password", &val));
    ASSERT_EQ((size_t)1, val.len);
    ASSERT_EQ('x', val.data[0]);
}
UTEST(form, absent_and_empty) {
    core_str_view_t val = {.len = 7}; /* deliberately wrong: overwritten with 0 */
    ASSERT_EQ(1, core_http_form_field(CORE_SV("password="), "password", &val));
    ASSERT_EQ((size_t)0, val.len);
    ASSERT_EQ(0, core_http_form_field(CORE_SV("password="), "other", NULL));
    core_str_view_t none = {.len = 5}; /* sentinel: absent must not touch */
    ASSERT_EQ(0, core_http_form_field(CORE_SV("password="), "other", &none));
    ASSERT_EQ((size_t)5, none.len);
}
UTEST(form, encoded_left_as_is) {
    core_str_view_t val = {0};
    ASSERT_EQ(1, core_http_form_field(CORE_SV("p=a%20b"), "p", &val));
    ASSERT_EQ((size_t)5, val.len);
    ASSERT_EQ(0, strncmp("a%20b", val.data, val.len));
}

UTEST(redirect, serves_302_with_location) {
    struct core_conn conn;
    memset(&conn, 0, sizeof(conn));
    ASSERT_EQ(0, core_http_serve_redirect(&conn, "/login"));
    ASSERT_NE(NULL, strstr(conn.resp.head, core_http_status_redirect));
    ASSERT_NE(NULL, strstr(conn.resp.head, "Location: /login\r\n"));
    ASSERT_EQ(1, conn.resp.iov_count); /* empty body: the head is the response */
    ASSERT_EQ(conn.resp.head_len, conn.resp.total_len);
}

/* --- Port from the WEBPLATFORM_HTTP_PORT convention --- */

UTEST(port_env, override_used) {
    setenv("WEBPLATFORM_HTTP_PORT", "9090", 1);
    ASSERT_EQ(9090, core_http_port_env(8080));
    unsetenv("WEBPLATFORM_HTTP_PORT");
}

UTEST(port_env, absent_falls_back_to_default) {
    unsetenv("WEBPLATFORM_HTTP_PORT");
    ASSERT_EQ(8080, core_http_port_env(8080));
}

UTEST(port_env, garbage_falls_back_to_default) {
    setenv("WEBPLATFORM_HTTP_PORT", "abc", 1);
    ASSERT_EQ(8080, core_http_port_env(8080));
    unsetenv("WEBPLATFORM_HTTP_PORT");
}

UTEST(port_env, out_of_range_falls_back) {
    setenv("WEBPLATFORM_HTTP_PORT", "70000", 1);
    ASSERT_EQ(8080, core_http_port_env(8080));
    setenv("WEBPLATFORM_HTTP_PORT", "0", 1);
    ASSERT_EQ(8080, core_http_port_env(8080));
    unsetenv("WEBPLATFORM_HTTP_PORT");
}

UTEST_MAIN();
