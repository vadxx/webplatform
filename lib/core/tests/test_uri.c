#include "../src/uri.c"
#include "utest.h"
#include <string.h>

/* Contract boundaries mirrored from the implementation (max segment) */
enum {
    URI_MAX_LEN = 128,
    URI_TOO_LONG = 129,
    URI_BOUND_BUF = URI_TOO_LONG + 1, /* fits the too-long segment */
};

/* Allocation counter (--wrap=malloc): stdio and the framework allocate
 * outside the module and an abort would break everything, so attribute
 * precisely: a dedicated test resets the counter and asserts the module
 * did not allocate. */
extern void *__real_malloc(size_t size); /* provided by ld --wrap */

static int g_malloc_calls;

__attribute__((visibility("default"))) void *__wrap_malloc(size_t size) {
    ++g_malloc_calls;
    return __real_malloc(size);
}

/* Short helpers: length always follows the literal, no hand-counted numbers */
static int valid(const char *segment) { return core_uri_segment_valid(segment, strlen(segment)); }

static int valid_n(const char *segment, size_t len) { return core_uri_segment_valid(segment, len); }

static void fill_a(char *buf, size_t n) {
    memset(buf, 'a', n);
    buf[n] = '\0';
}

UTEST(uri, valid_simple) { ASSERT_TRUE(valid("hello")); }
UTEST(uri, valid_charset) { ASSERT_TRUE(valid("a-b_c.9")); }
UTEST(uri, valid_max_len) {
    char segment[URI_BOUND_BUF];
    fill_a(segment, URI_MAX_LEN);
    ASSERT_TRUE(valid_n(segment, URI_MAX_LEN));
}
UTEST(uri, invalid_empty) { ASSERT_FALSE(valid_n("", 0)); }
UTEST(uri, invalid_null) { ASSERT_FALSE(core_uri_segment_valid(NULL, 0)); }
UTEST(uri, invalid_dotdot) { ASSERT_FALSE(valid("..")); }
UTEST(uri, invalid_inner_dotdot) { ASSERT_FALSE(valid("a..b")); }
UTEST(uri, invalid_slash) { ASSERT_FALSE(valid("a/b")); }
UTEST(uri, invalid_upper) { ASSERT_FALSE(valid("A")); }
UTEST(uri, invalid_control) { ASSERT_FALSE(valid_n("a\nb", strlen("a\nb"))); }
UTEST(uri, invalid_too_long) {
    char segment[URI_BOUND_BUF];
    fill_a(segment, URI_TOO_LONG);
    ASSERT_FALSE(valid_n(segment, URI_TOO_LONG));
}

UTEST(uri, module_is_zero_heap) {
    char segment[URI_BOUND_BUF];
    fill_a(segment, URI_MAX_LEN);
    g_malloc_calls = 0;
    ASSERT_TRUE(valid_n(segment, URI_MAX_LEN));
    ASSERT_FALSE(valid(".."));
    ASSERT_EQ(0, g_malloc_calls);
}

UTEST_MAIN();
