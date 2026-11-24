#include "../../hello_server/src/db.c"
#include "../src/sessions.c"
#include "utest.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Allocation counter (--wrap=malloc): the store must stay zero-heap */
extern void *__real_malloc(size_t size);

static int g_malloc_calls;

__attribute__((visibility("default"))) void *__wrap_malloc(size_t size) {
    ++g_malloc_calls;
    return __real_malloc(size);
}

static const char DB_PATH[] = "/tmp/webplatform-test-sessions-store.db";

static const char TOKEN_ALIVE[] = "0123456789abcdef0123456789abcdef";
static const char TOKEN_DEAD[] = "fedcba9876543210fedcba9876543210";
static const char TOKEN_GARBAGE[] = "x'; DROP TABLE metrics;--";

/* Row sink: counts rows (values are not inspected) */
struct count_sink {
    int count;
};

static int count_row(void *ctx, int cols, const char *values[]) {
    (void)cols;
    (void)values;
    ++((struct count_sink *)ctx)->count;
    return 0;
}

static int table_exists(struct db *db, const char *name) {
    char sql[96];
    struct count_sink sink = {0};
    snprintf(sql, sizeof(sql), "SELECT name FROM sqlite_master WHERE type='table' AND name='%s'",
             name);
    if (db_query(db, sql, count_row, &sink) != 0) {
        return -1;
    }
    return sink.count;
}

static int db_reset(struct db *db) {
    remove(DB_PATH);
    return db_open(db, DB_PATH);
}

UTEST(sessions_store, save_writes_row) {
    struct db db;
    struct count_sink sink = {0};
    ASSERT_EQ(0, db_reset(&db));
    ASSERT_EQ(0, app_sessions_save(&db, TOKEN_ALIVE, time(NULL) + 3600));
    ASSERT_EQ(0, db_query(&db, "SELECT token FROM sessions", count_row, &sink));
    ASSERT_EQ(1, sink.count);
    ASSERT_EQ(0, db_close(&db));
}

UTEST(sessions_store, delete_removes_row) {
    struct db db;
    struct count_sink sink = {0};
    ASSERT_EQ(0, db_reset(&db));
    ASSERT_EQ(0, app_sessions_save(&db, TOKEN_ALIVE, time(NULL) + 3600));
    ASSERT_EQ(0, app_sessions_delete(&db, TOKEN_ALIVE));
    ASSERT_EQ(0, db_query(&db, "SELECT token FROM sessions", count_row, &sink));
    ASSERT_EQ(0, sink.count);
    /* deleting an unknown token is not an error */
    ASSERT_EQ(0, app_sessions_delete(&db, TOKEN_DEAD));
    ASSERT_EQ(0, db_close(&db));
}

UTEST(sessions_store, garbage_token_rejected_and_db_untouched) {
    struct db db;
    struct count_sink sink = {0};
    ASSERT_EQ(0, db_reset(&db));
    ASSERT_EQ(-1, app_sessions_save(&db, TOKEN_GARBAGE, time(NULL) + 3600));
    ASSERT_EQ(-1, app_sessions_delete(&db, TOKEN_GARBAGE));
    ASSERT_EQ(1, table_exists(&db, "metrics"));
    ASSERT_EQ(1, table_exists(&db, "sessions"));
    ASSERT_EQ(0, db_query(&db, "SELECT token FROM sessions", count_row, &sink));
    ASSERT_EQ(0, sink.count);
    ASSERT_EQ(0, db_close(&db));
}

UTEST(sessions_store, hydrate_restores_live_and_sweeps_dead) {
    struct db db;
    ASSERT_EQ(0, db_reset(&db));
    /* dead row: expired in the past */
    ASSERT_EQ(0, app_sessions_save(&db, TOKEN_DEAD, time(NULL) - 1));
    /* live row */
    ASSERT_EQ(0, app_sessions_save(&db, TOKEN_ALIVE, time(NULL) + 3600));

    ASSERT_EQ(0, app_sessions_hydrate(&db));

    ASSERT_EQ(1, core_session_valid(TOKEN_ALIVE, strlen(TOKEN_ALIVE)));
    ASSERT_EQ(0, core_session_valid(TOKEN_DEAD, strlen(TOKEN_DEAD)));
    /* the dead row was swept */
    struct count_sink sink = {0};
    ASSERT_EQ(0, db_query(&db, "SELECT token FROM sessions", count_row, &sink));
    ASSERT_EQ(1, sink.count);
    ASSERT_EQ(0, db_close(&db));
}

UTEST(sessions_store, null_db_is_safe_noop) {
    ASSERT_EQ(-1, app_sessions_save(NULL, TOKEN_ALIVE, time(NULL) + 3600));
    ASSERT_EQ(-1, app_sessions_delete(NULL, TOKEN_ALIVE));
    ASSERT_EQ(-1, app_sessions_hydrate(NULL));
}

UTEST(sessions_store, module_is_zero_heap) {
    struct db db;
    ASSERT_EQ(0, db_reset(&db));
    g_malloc_calls = 0;
    ASSERT_EQ(0, app_sessions_save(&db, TOKEN_ALIVE, time(NULL) + 3600));
    ASSERT_EQ(0, app_sessions_delete(&db, TOKEN_ALIVE));
    ASSERT_EQ(0, app_sessions_hydrate(&db));
    ASSERT_EQ(0, g_malloc_calls);
    ASSERT_EQ(0, db_close(&db));
    remove(DB_PATH);
}

UTEST_MAIN();
