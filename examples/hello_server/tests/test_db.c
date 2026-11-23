#include "../src/db.c"
#include "utest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Allocation counter (--wrap=malloc): the seam must stay zero-heap.
 * Every SQLite allocation goes to the static arena, never to libc. */
extern void *__real_malloc(size_t size); /* provided by ld --wrap */

static int g_malloc_calls;

__attribute__((visibility("default"))) void *__wrap_malloc(size_t size) {
    ++g_malloc_calls;
    return __real_malloc(size);
}

/* Contract boundaries for the fixtures */
enum {
    ROW_CAP = 4,
    ROW_VALUE_CAP = 31,
    SOAK_ROWS = 20000,
    SOAK_DELETE_STEP = 2000,
    SOAK_DELETE_KEEP = 500,
};

/* Row sink: captures the first column of up to ROW_CAP rows */
struct row_sink {
    char values[ROW_CAP][ROW_VALUE_CAP + 1];
    int count;
};

/* Row sink: counts every row, stores the first column of the first
 * ROW_CAP rows. Always returns 0: a nonzero return aborts sqlite3_exec,
 * which is the abort path, not an error path. */
static int capture_row(void *ctx, int cols, const char *values[]) {
    struct row_sink *sink = ctx;
    if (cols >= 1 && sink->count < ROW_CAP) {
        snprintf(sink->values[sink->count], sizeof(sink->values[0]), "%s", values[0]);
    }
    ++sink->count;
    return 0;
}

UTEST(db, open_exec_query_roundtrip) {
    struct db db;
    struct row_sink sink;
    memset(&sink, 0, sizeof(sink));
    ASSERT_EQ(0, db_open(&db, ":memory:"));
    ASSERT_EQ(0, db_exec(&db, "INSERT INTO metrics VALUES (1000, 1.5, 200, 0.1)"));
    ASSERT_EQ(0, db_exec(&db, "INSERT INTO metrics VALUES (2000, 2.5, 300, 0.2)"));
    ASSERT_EQ(0, db_query(&db, "SELECT ts FROM metrics ORDER BY ts", capture_row, &sink));
    ASSERT_EQ(2, sink.count);
    ASSERT_EQ(0, strcmp("1000", sink.values[0]));
    ASSERT_EQ(0, strcmp("2000", sink.values[1]));
    ASSERT_EQ(0, db_close(&db));
}

UTEST(db, schema_created_by_open) {
    struct db db;
    struct row_sink sink;
    memset(&sink, 0, sizeof(sink));
    ASSERT_EQ(0, db_open(&db, ":memory:"));
    ASSERT_EQ(0, db_query(&db, "SELECT COUNT(*) FROM metrics", capture_row, &sink));
    ASSERT_EQ(1, sink.count);
    ASSERT_EQ(0, strcmp("0", sink.values[0]));
    ASSERT_EQ(0, db_close(&db));
}

UTEST(db, exec_bad_sql_fails) {
    struct db db;
    ASSERT_EQ(0, db_open(&db, ":memory:"));
    ASSERT_NE(0, db_exec(&db, "THIS IS NOT SQL"));
    ASSERT_EQ(0, db_close(&db));
}

UTEST(db, reopen_and_parallel_are_safe) {
    struct db first;
    struct db second;
    ASSERT_EQ(0, db_open(&first, ":memory:"));
    ASSERT_EQ(0, db_open(&second, ":memory:")); /* arena configured once */
    ASSERT_EQ(0, db_exec(&second, "INSERT INTO metrics VALUES (5, 1, 1, 1)"));
    ASSERT_EQ(0, db_close(&second));
    ASSERT_EQ(0, db_close(&first));
    ASSERT_EQ(0, db_open(&first, ":memory:")); /* sequential reopen */
    ASSERT_EQ(0, db_close(&first));
}

UTEST(db, module_is_zero_heap) {
    struct db db;
    struct row_sink sink;
    char sql[64];
    memset(&sink, 0, sizeof(sink));
    g_malloc_calls = 0;
    ASSERT_EQ(0, db_open(&db, ":memory:"));
    for (int i = 0; i < 10; ++i) {
        snprintf(sql, sizeof(sql), "INSERT INTO metrics VALUES (%d, 1.5, 100, 0.5)", i);
        ASSERT_EQ(0, db_exec(&db, sql));
    }
    ASSERT_EQ(0, db_query(&db, "SELECT ts, cpu_busy FROM metrics", capture_row, &sink));
    ASSERT_EQ(10, sink.count);
    ASSERT_EQ(0, db_close(&db));
    ASSERT_EQ(0, g_malloc_calls);
}

UTEST(db, soak_day_of_inserts) {
    const char path[] = "/tmp/webplatform_db_soak.db";
    struct db db;
    char sql[96];
    remove(path);
    ASSERT_EQ(0, db_open(&db, path));
    for (int i = 0; i < SOAK_ROWS; ++i) {
        snprintf(sql, sizeof(sql), "INSERT INTO metrics VALUES (%d, 12.5, 812340, 0.42)", 1700000000 + i);
        ASSERT_EQ(0, db_exec(&db, sql));
        if ((i + 1) % SOAK_DELETE_STEP == 0) {
            const int keep_from = 1700000000 + i + 1 - SOAK_DELETE_KEEP;
            snprintf(sql, sizeof(sql), "DELETE FROM metrics WHERE ts < %d", keep_from);
            ASSERT_EQ(0, db_exec(&db, sql));
        }
    }
    ASSERT_EQ(0, db_close(&db));
    remove(path);
}

/* Counters for the growth reproducer */
struct row_counter {
    int n;
};

static int count_every_row(void *ctx, int cols, const char *values[]) {
    (void)cols;
    (void)values;
    ++((struct row_counter *)ctx)->n;
    return 0;
}

static int keep_first_ll(void *ctx, int cols, const char *values[]) {
    (void)cols;
    *(long long *)ctx = atoll(values[0]);
    return 0;
}

/* Reproducer for the arena/cache_size budget: the collector's leaf
 * splits grow the file page by page; once the tree stops fitting in
 * the page cache, every further INSERT and a full-scan SELECT must
 * still succeed instead of exhausting the static arena. */
UTEST(db, soak_grows_without_deletes) {
    const char path[] = "/tmp/webplatform_db_grow.db";
    struct db db;
    struct row_counter scanned = {0};
    long long counted = 0;
    char sql[96];
    remove(path);
    ASSERT_EQ(0, db_open(&db, path));
    for (int i = 0; i < SOAK_ROWS; ++i) {
        snprintf(sql, sizeof(sql), "INSERT INTO metrics VALUES (%d, 12.5, 812340, 0.42)",
                 1700000000 + i);
        if (db_exec(&db, sql) != 0) {
            ASSERT_TRUE(0 && "INSERT failed while the table grows");
            break;
        }
    }
    ASSERT_EQ(0, db_query(&db, "SELECT COUNT(*) FROM metrics", keep_first_ll, &counted));
    ASSERT_EQ((long long)SOAK_ROWS, counted);
    ASSERT_EQ(0, db_query(&db, "SELECT ts, cpu_busy, mem_kb, load1 FROM metrics", count_every_row,
                          &scanned));
    ASSERT_EQ(SOAK_ROWS, scanned.n);
    ASSERT_EQ(0, db_close(&db));
    remove(path);
}

UTEST(db, changes_counts_rows) {
    struct db db;
    char sql[96];
    ASSERT_EQ(0, db_open(&db, ":memory:"));
    ASSERT_EQ(0, db_exec(&db, "INSERT INTO metrics VALUES (1, 1.5, 100, 0.5)"));
    ASSERT_EQ(0, db_exec(&db, "INSERT INTO metrics VALUES (2, 1.5, 100, 0.5)"));
    ASSERT_EQ(0, db_exec(&db, "DELETE FROM metrics WHERE ts < 3"));
    ASSERT_EQ((long)2, db_changes(&db));
    ASSERT_EQ(0, db_close(&db));
}

UTEST_MAIN();
