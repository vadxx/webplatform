#include "../src/log.c"
#include "utest.h"
#include <string.h>

/* Allocation counter (--wrap=malloc): logging must stay zero-heap */
extern void *__real_malloc(size_t size);

static int g_malloc_calls;

__attribute__((visibility("default"))) void *__wrap_malloc(size_t size) {
    ++g_malloc_calls;
    return __real_malloc(size);
}

UTEST(log, ring_keeps_lines) {
    core_log_stderr(0); /* silence stderr: tests read the ring */
    core_log('I', "tag1", "first %d", 1);
    ASSERT_EQ((size_t)1, core_log_count());
    core_log('W', "tag2", "second");
    ASSERT_EQ((size_t)2, core_log_count());
    ASSERT_NE(NULL, strstr(core_log_line(0), "first 1"));
    ASSERT_NE(NULL, strstr(core_log_line(1), "second"));
}

UTEST(log, line_shape) {
    core_log_stderr(0);
    core_log('W', "collect", "shape check");
    const char *line = core_log_line(core_log_count() - 1);
    ASSERT_NE(NULL, line);
    ASSERT_EQ('-', line[4]);  /* 2026-11-23T... */
    ASSERT_EQ('T', line[10]); /* ISO-like stamp */
    ASSERT_EQ('Z', line[19]);
    ASSERT_NE(NULL, strstr(line, " W collect: ")); /* level + tag */
    ASSERT_NE(NULL, strstr(line, "shape check"));
}

UTEST(log, ring_wraps_keeping_newest) {
    core_log_stderr(0);
    enum {
        FLOOD = CORE_LOG_RING_LINES + 10,
        FIRST_KEPT = 10, /* FLOOD - CORE_LOG_RING_LINES */
    };
    char expect_first[32];
    char expect_last[32];
    for (int i = 0; i < FLOOD; ++i) {
        core_log('I', "flood", "line %d", i);
    }
    snprintf(expect_first, sizeof(expect_first), "line %d", FIRST_KEPT);
    snprintf(expect_last, sizeof(expect_last), "line %d", FLOOD - 1);
    ASSERT_EQ((size_t)CORE_LOG_RING_LINES, core_log_count());
    ASSERT_NE(NULL, strstr(core_log_line(0), expect_first));
    ASSERT_NE(NULL, strstr(core_log_line(CORE_LOG_RING_LINES - 1), expect_last));
}

UTEST(log, out_of_range_is_null) {
    core_log_stderr(0);
    ASSERT_EQ(NULL, (void *)core_log_line(core_log_count()));
}

UTEST(log, module_is_zero_heap) {
    core_log_stderr(0);
    g_malloc_calls = 0;
    core_log('I', "probe", "window %d", 1);
    core_log('E', "probe", "window %d", 2);
    ASSERT_EQ(0, g_malloc_calls);
}

UTEST_MAIN();
