#include "../src/collect.c"
#include "../src/db.c"
#include "utest.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

/* Allocation counter (--wrap=malloc): the tick must stay zero-heap */
extern void *__real_malloc(size_t size);

static int g_malloc_calls;

__attribute__((visibility("default"))) void *__wrap_malloc(size_t size) {
    ++g_malloc_calls;
    return __real_malloc(size);
}

/* Literal /proc fixtures, mirrored from a real Linux procfs */
enum {
    STAT_USER = 74608,
    STAT_NICE = 2520,
    STAT_SYS = 24433,
    STAT_IDLE = 1117073,
    STAT_IOWAIT = 6176,
    STAT_IRQ = 4054,
    STAT_SIRQ = 0,
    STAT_STEAL = 24699,
    STAT_BUSY = STAT_USER + STAT_NICE + STAT_SYS + STAT_IRQ + STAT_SIRQ + STAT_STEAL,
    STAT_TOTAL = STAT_BUSY + STAT_IDLE + STAT_IOWAIT,
    MEM_TOTAL_KB = 16384256,
    MEM_AVAIL_KB = 11321876,
    TICK_SLEEP_MS = 100, /* let /proc/stat jiffies advance between ticks */
};

static const char PROC_STAT_FIXTURE[] = "cpu  74608 2520 24433 1117073 6176 4054 0 24699 0 0\n"
                                        "cpu0 37304 1260 12216 558536 3088 2027 0 12349 0 0\n";

static const char PROC_MEMINFO_FIXTURE[] = "MemTotal:       16384256 kB\n"
                                           "MemFree:          2345648 kB\n"
                                           "MemAvailable:    11321876 kB\n"
                                           "Buffers:           267248 kB\n";

static const char PROC_LOADAVG_FIXTURE[] = "0.42 0.58 0.61 1/495 12345\n";

/* Counting row callback: every row increments, stores nothing */
static int count_row(void *ctx, int cols, const char *values[]) {
    (void)cols;
    (void)values;
    ++*(int *)ctx;
    return 0;
}

static int metrics_rows(struct db *db) {
    int count = 0;
    if (db_query(db, "SELECT ts FROM metrics", count_row, &count) != 0) {
        return -1;
    }
    return count;
}

UTEST(parse, proc_stat_totals) {
    struct collect_stat_sample sample = {0};
    ASSERT_EQ(0, collect_parse_proc_stat(CORE_SV_BUF(PROC_STAT_FIXTURE, sizeof(PROC_STAT_FIXTURE)),
                                         &sample));
    ASSERT_EQ((long long)STAT_BUSY, sample.busy);
    ASSERT_EQ((long long)STAT_TOTAL, sample.total);
}

UTEST(parse, proc_stat_minimal_four_fields) {
    static const char line[] = "cpu 1 2 3 4\n";
    struct collect_stat_sample sample = {0};
    ASSERT_EQ(0, collect_parse_proc_stat(CORE_SV_BUF(line, sizeof(line)), &sample));
    ASSERT_EQ((long long)6, sample.busy);   /* user + nice + sys */
    ASSERT_EQ((long long)10, sample.total); /* busy + idle */
}

UTEST(parse, proc_stat_garbage) {
    static const char garbage[] = "garbage\n";
    static const char short_stat[] = "cpu 1 2\n";
    struct collect_stat_sample sample = {0};
    ASSERT_EQ(-1, collect_parse_proc_stat(CORE_SV_BUF(garbage, sizeof(garbage)), &sample));
    ASSERT_EQ(-1, collect_parse_proc_stat(CORE_SV_BUF(short_stat, sizeof(short_stat)), &sample));
}

UTEST(parse, stat_view_must_carry_terminator) {
    /* the view len must include the NUL: a terminator-free view would
     * let sscanf wander into stale bytes beyond the read length */
    char buf[64];
    memset(buf, '9', sizeof(buf));
    memcpy(buf, "cpu  1 2 3 4 5 6 7 8", 20);
    struct collect_stat_sample sample = {0};
    ASSERT_EQ(-1, collect_parse_proc_stat(CORE_SV_BUF(buf, 20), &sample));
}

UTEST(parse, meminfo_view_must_carry_terminator) {
    char buf[64];
    memset(buf, 'x', sizeof(buf));
    const size_t len = sizeof("MemTotal: 100 kB\nMemAvailable: 50 kB\n") - 1;
    memcpy(buf, "MemTotal: 100 kB\nMemAvailable: 50 kB\n", len);
    struct collect_mem_sample sample = {0};
    ASSERT_EQ(-1, collect_parse_meminfo(CORE_SV_BUF(buf, len), &sample));
}

UTEST(parse, meminfo_pair) {
    struct collect_mem_sample sample = {0};
    ASSERT_EQ(0, collect_parse_meminfo(
                     CORE_SV_BUF(PROC_MEMINFO_FIXTURE, sizeof(PROC_MEMINFO_FIXTURE)), &sample));
    ASSERT_EQ((long)MEM_TOTAL_KB, sample.total_kb);
    ASSERT_EQ((long)MEM_AVAIL_KB, sample.avail_kb);
}

UTEST(parse, meminfo_missing_available) {
    static const char no_avail[] = "MemTotal: 100 kB\n";
    struct collect_mem_sample sample = {0};
    ASSERT_EQ(-1, collect_parse_meminfo(CORE_SV_BUF(no_avail, sizeof(no_avail)), &sample));
}

UTEST(parse, loadavg_first) {
    double load1 = 0;
    ASSERT_EQ(0, collect_parse_loadavg(
                     CORE_SV_BUF(PROC_LOADAVG_FIXTURE, sizeof(PROC_LOADAVG_FIXTURE)), &load1));
    ASSERT_NEAR(0.42, load1, 0.0001);
}

UTEST(delta, cpu_busy_percent) {
    const struct collect_stat_sample prev = {.busy = 6, .total = 10};
    const struct collect_stat_sample cur = {.busy = 14, .total = 30};
    double percent = 0;
    /* busy grew by 8, total by 20 -> 40% */
    ASSERT_EQ(0, collect_cpu_busy_percent(&prev, &cur, &percent));
    ASSERT_NEAR(40.0, percent, 0.0001);
    /* no delta yet (same counters) */
    ASSERT_EQ(-1, collect_cpu_busy_percent(&prev, &prev, &percent));
    /* counter reset (total went backwards) */
    const struct collect_stat_sample reset = {.busy = 5, .total = 30};
    ASSERT_EQ(-1, collect_cpu_busy_percent(&reset, &cur, &percent));
}

UTEST(tick, first_tick_writes_nothing) {
    struct db db;
    struct collect_ctx ctx;
    ASSERT_EQ(0, db_open(&db, ":memory:"));
    ctx.db = &db;
    ctx.timer_fd = -1;     /* no timer wired in tests */
    collect_on_tick(&ctx); /* baseline only */
    ASSERT_EQ(0, metrics_rows(&db));
    usleep(TICK_SLEEP_MS * 1000);
    collect_on_tick(&ctx); /* one delta row against the real /proc */
    ASSERT_EQ(1, metrics_rows(&db));
    ASSERT_EQ(0, db_close(&db));
}

UTEST(tick, drains_timer_fd) {
    struct db db;
    struct collect_ctx ctx;
    ASSERT_EQ(0, db_open(&db, ":memory:"));
    ctx.db = &db;
    const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    ASSERT_GE(fd, 0);
    struct itimerspec spec;
    memset(&spec, 0, sizeof(spec));
    spec.it_value.tv_nsec = 1; /* fires immediately */
    ASSERT_EQ(0, timerfd_settime(fd, 0, &spec, NULL));
    ctx.timer_fd = fd;
    collect_on_tick(&ctx);
    uint64_t ticks = 0;
    ASSERT_EQ(-1, (int)read(fd, &ticks, sizeof(ticks)));
    ASSERT_EQ(EAGAIN, errno); /* fully drained: no level-triggered storm */
    close(fd);
    ctx.timer_fd = -1;
    ASSERT_EQ(0, db_close(&db));
}

UTEST(tick, module_is_zero_heap) {
    struct db db;
    struct collect_ctx ctx;
    g_malloc_calls = 0;
    ASSERT_EQ(0, db_open(&db, ":memory:"));
    ctx.db = &db;
    ctx.timer_fd = -1;
    collect_on_tick(&ctx);
    usleep(TICK_SLEEP_MS * 1000);
    collect_on_tick(&ctx);
    ASSERT_EQ(0, db_close(&db));
    ASSERT_EQ(0, g_malloc_calls);
}

UTEST_MAIN();
