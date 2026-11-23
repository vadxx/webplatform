#include "../../../lib/core/src/uri.c"
#include "../lib/hello_plugin.c"
#include "../lib/hello_plugin.h"

#include "utest.h"
#include <stdio.h>
#include <string.h>

/* Allocation counter (--wrap=malloc): stdio and the framework allocate
 * outside the module; a dedicated test asserts the plugin does not. */
extern void *__real_malloc(size_t size);

static int g_malloc_calls;

__attribute__((visibility("default"))) void *__wrap_malloc(size_t size) {
    ++g_malloc_calls;
    return __real_malloc(size);
}

/* Contract boundary mirrored from the implementation */
enum {
    URI_MAX_LEN = 128,
    REQUEST_CAP = 512, /* fits "GET /" + the max segment + " HTTP/1.1" */
};

/* Fake host_api: libc functions of the test binary */
static struct host_api_t fake_api = {.snprintf = snprintf, .memcpy = memcpy};

static int handle_ok_path(const char *path, struct plugin_response_t *resp) {
    char request[REQUEST_CAP];
    const int len = snprintf(request, sizeof(request), "GET /%s HTTP/1.1", path);
    return plugin_handle(request, (size_t)len, resp);
}

UTEST(plugin, init_null_api) { ASSERT_EQ(-1, plugin_init(NULL)); }
UTEST(plugin, init_ok) { ASSERT_EQ(0, plugin_init(&fake_api)); }

UTEST(plugin, handle_without_init_guard) {
    struct plugin_response_t resp;
    g_host_api = (struct host_api_t){0}; /* same TU as the plugin: reset directly */
    char request[] = "GET /x HTTP/1.1";
    ASSERT_EQ(-1, plugin_handle(request, strlen(request), &resp));
}

UTEST(plugin, handle_null_out) {
    char request[] = "GET /x HTTP/1.1";
    plugin_init(&fake_api);
    ASSERT_EQ(-1, plugin_handle(request, strlen(request), NULL));
}

UTEST(plugin, handle_dotdot) {
    struct plugin_response_t resp;
    char request[] = "GET /../etc HTTP/1.1";
    plugin_init(&fake_api);
    ASSERT_EQ(-1, plugin_handle(request, strlen(request), &resp));
}

UTEST(plugin, handle_ok) {
    struct plugin_response_t resp;
    plugin_init(&fake_api);
    ASSERT_EQ(0, handle_ok_path("hello", &resp));
    ASSERT_EQ(2, resp.iov_count);
    ASSERT_EQ(0, strncmp(resp.head, HELLO_RESPONSE_MARKER, sizeof(HELLO_RESPONSE_MARKER) - 1));
    ASSERT_EQ(0, strcmp(resp.head + sizeof(HELLO_RESPONSE_MARKER) - 1, "hello"));
    ASSERT_EQ(resp.head, resp.iov[0].iov_base);
}

UTEST(plugin, handle_max_len_path_no_overflow) {
    struct plugin_response_t resp;
    char path[URI_MAX_LEN + 1];
    memset(path, 'a', URI_MAX_LEN);
    path[URI_MAX_LEN] = '\0';
    plugin_init(&fake_api);
    ASSERT_EQ(0, handle_ok_path(path, &resp));
    ASSERT_LT(resp.head_len, sizeof(resp.head));
    ASSERT_EQ(0, resp.head[resp.head_len]); /* NUL-terminated */
}

UTEST(plugin, module_is_zero_heap) {
    struct plugin_response_t resp;
    plugin_init(&fake_api);
    g_malloc_calls = 0;
    ASSERT_EQ(0, handle_ok_path("heap", &resp));
    ASSERT_EQ(0, g_malloc_calls);
}

UTEST(plugin, handle_response_has_gzip_asset) {
    struct plugin_response_t resp;
    plugin_init(&fake_api);
    ASSERT_EQ(0, handle_ok_path("hello", &resp));
    /* iov[1] starts with the gzip magic 1f 8b */
    const unsigned char *gz = (const unsigned char *)resp.iov[1].iov_base;
    ASSERT_EQ(0x1f, gz[0]);
    ASSERT_EQ(0x8b, gz[1]);
    ASSERT_TRUE(resp.iov[1].iov_len > 0);
    ASSERT_EQ(resp.head_len + resp.iov[1].iov_len, resp.total_len);
}

UTEST_MAIN();
