#include "../src/app_logs.c"
#include "utest.h"
#include <string.h>

/* Allocation counter (--wrap=malloc): the endpoint must stay zero-heap */
extern void *__real_malloc(size_t size);

static int g_malloc_calls;

__attribute__((visibility("default"))) void *__wrap_malloc(size_t size) {
    ++g_malloc_calls;
    return __real_malloc(size);
}

/* Conns mirror the server: static storage, fd marks liveness */
static struct core_conn g_conn;

static const char *serve(void) {
    memset(&g_conn, 0, sizeof(g_conn));
    g_conn.fd = 7; /* any occupied descriptor */
    app_logs_serve(&g_conn);
    return (const char *)g_conn.resp.iov[1].iov_base;
}

/* The ring is process-global: the empty-ring case must run before any
 * core_log call. UTEST registration order is top-to-bottom, so this
 * case stays first in the file. */
UTEST(app_logs, empty_ring_renders_empty_array) {
    core_log_stderr(0);
    const char *body = serve();
    ASSERT_NE(NULL, body);
    ASSERT_NE(NULL, strstr(body, "{\"lines\":[]}"));
}

UTEST(app_logs, renders_lines_oldest_first) {
    core_log_stderr(0);
    core_log('I', "test", "first line");
    core_log('I', "test", "second line");
    core_log('I', "test", "third line");
    const char *body = serve();
    ASSERT_NE(NULL, body);
    const char *first = strstr(body, "first line");
    const char *second = strstr(body, "second line");
    const char *third = strstr(body, "third line");
    ASSERT_NE(NULL, first);
    ASSERT_NE(NULL, second);
    ASSERT_NE(NULL, third);
    ASSERT_TRUE(first < second);
    ASSERT_TRUE(second < third);
}

UTEST(app_logs, escapes_quotes_and_backslashes) {
    core_log_stderr(0);
    core_log('I', "test", "bad \"quoted\" \\ path");
    const char *body = serve();
    ASSERT_NE(NULL, body);
    ASSERT_NE(NULL, strstr(body, "bad \\\"quoted\\\" \\\\ path"));
}

UTEST(app_logs, json_content_type_and_status) {
    core_log_stderr(0);
    memset(&g_conn, 0, sizeof(g_conn));
    g_conn.fd = 7;
    app_logs_serve(&g_conn);
    ASSERT_NE(NULL, strstr(g_conn.resp.head, core_http_status_ok));
    ASSERT_NE(NULL, strstr(g_conn.resp.head, "application/json"));
}

UTEST(app_logs, module_is_zero_heap) {
    core_log_stderr(0);
    core_log('I', "test", "heap probe");
    g_malloc_calls = 0;
    const char *body = serve();
    ASSERT_NE(NULL, body);
    ASSERT_EQ(0, g_malloc_calls);
}

UTEST_MAIN();
