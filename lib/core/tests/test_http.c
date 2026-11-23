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

/* iov tail fixture: a constant expression is required in the initializer */
#define TAIL_TAG ((void *)0x99)
#define TAIL_LEN 3

UTEST(path, simple) {
    size_t len = 0;
    const char request[] = "GET /a/b HTTP/1.1";
    const char *path = core_http_request_path(request, strlen(request), &len);
    ASSERT_NE(NULL, (void *)path);
    ASSERT_EQ(0, strncmp(path, "/a/b", sizeof("/a/b") - 1));
    ASSERT_EQ((size_t)(sizeof("/a/b") - 1), len);
}

UTEST(path, stops_at_cr) {
    size_t len = 0;
    const char request[] = "GET /x\r\nHost: a";
    const char *path = core_http_request_path(request, strlen(request), &len);
    ASSERT_NE(NULL, (void *)path);
    ASSERT_EQ((size_t)(sizeof("/x") - 1), len);
}

UTEST(path, no_space) {
    ASSERT_EQ(NULL, (void *)core_http_request_path("GETX", strlen("GETX"), NULL));
}
UTEST(path, null_req) { ASSERT_EQ(NULL, (void *)core_http_request_path(NULL, 0, NULL)); }

UTEST(head, ok_with_gzip) {
    char buf[HEAD_CAP];
    const size_t head_len = core_http_head(buf, sizeof(buf), "200 OK", "text/html", 42, 1);
    ASSERT_TRUE(head_len > 0);
    ASSERT_EQ(0, strncmp(buf, "HTTP/1.1 200 OK\r\n", sizeof("HTTP/1.1 200 OK\r\n") - 1));
    ASSERT_NE(NULL, strstr(buf, "Content-Encoding: gzip\r\n"));
    ASSERT_NE(NULL, strstr(buf, "Content-Length: 42\r\n"));
    ASSERT_NE(NULL, strstr(buf, "Connection: close\r\n\r\n"));
}

UTEST(head, no_gzip_line_without_flag) {
    char buf[HEAD_CAP];
    const size_t head_len = core_http_head(buf, sizeof(buf), "404 Not Found", "text/plain", 10, 0);
    ASSERT_TRUE(head_len > 0);
    ASSERT_EQ(NULL, strstr(buf, "Content-Encoding"));
    ASSERT_NE(NULL, strstr(buf, "404 Not Found"));
}

UTEST(head, too_small_cap_is_zero) {
    char buf[HEAD_CAP_TINY];
    ASSERT_EQ((size_t)0, core_http_head(buf, sizeof(buf), "200 OK", "text/html", 1, 0));
}
UTEST(head, null_buf) { ASSERT_EQ((size_t)0, core_http_head(NULL, HEAD_CAP, "200 OK", "t", 1, 0)); }

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
    ASSERT_NE(NULL, strstr(conn.resp.head, "404 Not Found"));
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
    ASSERT_EQ(1, core_http_match_exact("/htmx.min.js", strlen("/htmx.min.js"), "/htmx.min.js"));
}
UTEST(match, exact_longer_path) {
    ASSERT_EQ(0, core_http_match_exact("/htmx.min.jsx", strlen("/htmx.min.jsx"), "/htmx.min.js"));
}
UTEST(match, exact_shorter_path) {
    ASSERT_EQ(0, core_http_match_exact("/htmx", strlen("/htmx"), "/htmx.min.js"));
}
UTEST(match, exact_different) {
    ASSERT_EQ(0, core_http_match_exact("/other", strlen("/other"), "/htmx.min.js"));
}

UTEST(match, prefix_extracts_tail) {
    const char *seg = NULL;
    size_t seg_len = 0;
    ASSERT_EQ(1, core_http_match_prefix("/plugin/hello", strlen("/plugin/hello"), "/plugin/", &seg,
                                        &seg_len));
    ASSERT_EQ(0, strncmp("hello", seg, seg_len));
    ASSERT_EQ((size_t)5, seg_len);
}
UTEST(match, prefix_no_match) {
    ASSERT_EQ(0, core_http_match_prefix("/other/x", strlen("/other/x"), "/plugin/", NULL, NULL));
}
UTEST(match, prefix_equal_length_no_tail) {
    /* the prefix itself is not a route: an empty segment is not allowed */
    ASSERT_EQ(0, core_http_match_prefix("/plugin/", strlen("/plugin/"), "/plugin/", NULL, NULL));
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

#define TAIL_TAG_REDEF ((void *)0x77)

UTEST(serve_plugin, composes_request_and_wraps_fragment) {
    struct core_conn conn;
    struct core_plugin plugin = {.handle = capture_handle};
    memset(&conn, 0, sizeof(conn));
    ASSERT_EQ(0, core_http_serve_plugin(&conn, &plugin, "hello", strlen("hello")));
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
    ASSERT_EQ(0, core_http_serve_plugin(&conn, &plugin, "..", strlen("..")));
    ASSERT_NE(NULL, strstr(conn.resp.head, "404 Not Found"));
}

UTEST(serve_plugin, plugin_decline_is_404) {
    struct core_conn conn;
    struct core_plugin plugin = {.handle = decline_handle};
    memset(&conn, 0, sizeof(conn));
    ASSERT_EQ(0, core_http_serve_plugin(&conn, &plugin, "x", strlen("x")));
    ASSERT_NE(NULL, strstr(conn.resp.head, "404 Not Found"));
}

UTEST(serve_plugin, null_args) { ASSERT_EQ(-1, core_http_serve_plugin(NULL, NULL, "x", 1)); }

UTEST(route_table, serves_matching_route) {
    static const unsigned char gz[] = {0x1f, 0x8b};
    static const struct core_http_asset_route routes[] = {
        {"/style.css", core_content_type_css, gz, sizeof(gz)},
    };
    struct core_conn conn;
    memset(&conn, 0, sizeof(conn));
    ASSERT_EQ(0, core_http_serve_asset_route(&conn, "/style.css", strlen("/style.css"), routes,
                                             CORE_ARRAY_LEN(routes)));
    ASSERT_NE(NULL, strstr(conn.resp.head, "Content-Type: "));
    ASSERT_NE(NULL, strstr(conn.resp.head, core_content_type_css));
}

UTEST(route_table, no_match_falls_through) {
    static const struct core_http_asset_route routes[] = {
        {"/style.css", core_content_type_css, "x", 1},
    };
    struct core_conn conn;
    memset(&conn, 0, sizeof(conn));
    ASSERT_EQ(1, core_http_serve_asset_route(&conn, "/other", strlen("/other"), routes,
                                             CORE_ARRAY_LEN(routes)));
}

UTEST(route_table, empty_table_falls_through) {
    struct core_conn conn;
    memset(&conn, 0, sizeof(conn));
    ASSERT_EQ(1, core_http_serve_asset_route(&conn, "/", 1, NULL, 0));
}

#undef TAIL_TAG_REDEF
#undef TAIL_LEN_REDEF

/* --- Full router (core_http_route) --- */

/* Route a request string through the router into a fresh conn */
static int route_request(const char *request, const struct core_http_asset_route *assets,
                         size_t asset_count, const struct core_http_plugin_route *plugins,
                         size_t plugin_count, struct core_conn *conn) {
    memset(conn, 0, sizeof(*conn));
    memcpy(conn->req_buf, request, strlen(request));
    conn->req_len = strlen(request);
    return core_http_route(conn, assets, asset_count, plugins, plugin_count);
}

UTEST(router, asset_hit) {
    static const unsigned char gz[] = {0x1f, 0x8b};
    static const struct core_http_asset_route assets[] = {
        {"/", "text/html", gz, sizeof(gz)},
    };
    struct core_conn conn;
    ASSERT_EQ(
        0, route_request("GET / HTTP/1.1\r\n\r\n", assets, CORE_ARRAY_LEN(assets), NULL, 0, &conn));
    ASSERT_NE(NULL, strstr(conn.resp.head, "Content-Type: text/html"));
}

UTEST(router, plugin_hit) {
    static const struct core_plugin plugin = {.handle = capture_handle};
    static const struct core_http_plugin_route plugins[] = {
        {"/plugin/", &plugin},
    };
    struct core_conn conn;
    ASSERT_EQ(0, route_request("GET /plugin/hello HTTP/1.1\r\n\r\n", NULL, 0, plugins,
                               CORE_ARRAY_LEN(plugins), &conn));
    ASSERT_EQ(0, strcmp("GET /hello HTTP/1.1", g_captured_request));
    ASSERT_EQ(0, strncmp("HTTP/1.1 200 OK\r\n", conn.resp.head, sizeof("HTTP/1.1 200 OK\r\n") - 1));
}

UTEST(router, unknown_path_is_404) {
    struct core_conn conn;
    ASSERT_EQ(0, route_request("GET /nope HTTP/1.1\r\n\r\n", NULL, 0, NULL, 0, &conn));
    ASSERT_NE(NULL, strstr(conn.resp.head, "404 Not Found"));
}

UTEST(router, request_without_path_is_404) {
    struct core_conn conn;
    ASSERT_EQ(0, route_request("GARBAGE\r\n\r\n", NULL, 0, NULL, 0, &conn));
    ASSERT_NE(NULL, strstr(conn.resp.head, "404 Not Found"));
}

UTEST(router, null_conn) { ASSERT_EQ(-1, core_http_route(NULL, NULL, 0, NULL, 0)); }

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
