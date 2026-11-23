#include "../src/db.c"
#include "../src/metrics.c"
#include "utest.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Allocation counter (--wrap=malloc): rendering must stay zero-heap */
extern void *__real_malloc(size_t size);

static int g_malloc_calls;

__attribute__((visibility("default"))) void *__wrap_malloc(size_t size) {
    ++g_malloc_calls;
    return __real_malloc(size);
}

enum {
    SEED_ROWS_FEW = 3,
    SEED_ROWS_MANY = 5000,
    SEED_STEP_SEC = 5,
    EXPECTED_MAX_POINTS = 720, /* mirrors MAX_POINTS from metrics.c */
    LIVE_CONN_FD = 7,          /* "some occupied descriptor": only != -1 matters */
};

/* Conns mirror the server: they live in static storage (the module
 * reads conn->fd later to reap slots of released connections), and a
 * test releases them at the end the way the loop does after a reply. */
static struct core_conn g_conn;
static struct core_conn g_owners[4];
static struct core_conn g_extra;

static void release(struct core_conn *conn) { conn->fd = -1; }

/* Seeds n rows ending at now, STEP apart, deterministic values, in one
 * transaction (test scaffolding; the collector writes per tick).
 * ASSERT macros only work inside UTEST bodies, so this returns a code. */
static int seed_rows(struct db *db, int n, long long now) {
    char sql[96];
    if (db_exec(db, "BEGIN") != 0) {
        return -1;
    }
    for (int i = 0; i < n; ++i) {
        const long long ts = now - (long long)(n - 1 - i) * SEED_STEP_SEC;
        snprintf(sql, sizeof(sql), "INSERT INTO metrics VALUES (%lld, %d.5, %d, %d.25)", ts,
                 i % 100, 100000 + i, i % 50);
        if (db_exec(db, sql) != 0) {
            return -1;
        }
    }
    return db_exec(db, "COMMIT");
}

static const char *serve(struct db *db, const char *path) {
    memset(&g_conn, 0, sizeof(g_conn));
    g_conn.fd = LIVE_CONN_FD;
    metrics_serve(db, &g_conn, CORE_SV_BUF(path, strlen(path)));
    /* the body slot is NUL-terminated for tests; the wire uses body_len */
    return (const char *)g_conn.resp.iov[1].iov_base;
}

/* Count integers inside the first JSON array ("ts":[a,b,c]) */
static int count_ts_points(const char *body) {
    const char *arr = strstr(body, "\"ts\":[");
    if (arr == NULL) {
        return -1;
    }
    int count = 0;
    const char *p = arr + sizeof("\"ts\":[") - 1;
    while (*p != '\0' && *p != ']') {
        if (*p == ',') {
            ++count;
        }
        ++p;
    }
    return *p == ']' ? count + 1 : -1;
}

UTEST(metrics, render_returns_json_arrays) {
    struct db db;
    ASSERT_EQ(0, db_open(&db, ":memory:"));
    const long long now = (long long)time(NULL);
    ASSERT_EQ(0, seed_rows(&db, SEED_ROWS_FEW, now));
    char newest_txt[32];
    snprintf(newest_txt, sizeof(newest_txt), "%lld", now);

    const char *body = serve(&db, "/api/metrics");
    ASSERT_NE(NULL, strstr(g_conn.resp.head, core_http_status_ok));
    ASSERT_NE(NULL, strstr(g_conn.resp.head, "application/json"));
    ASSERT_NE(NULL, strstr(body, "{\"ts\":["));
    ASSERT_NE(NULL, strstr(body, newest_txt));
    ASSERT_NE(NULL, strstr(body, "\"cpu\":["));
    ASSERT_NE(NULL, strstr(body, "\"mem\":["));
    ASSERT_NE(NULL, strstr(body, "\"load\":["));
    ASSERT_EQ(g_conn.resp.head_len + g_conn.resp.iov[1].iov_len, g_conn.resp.total_len);
    release(&g_conn);
    ASSERT_EQ(0, db_close(&db));
}

UTEST(metrics, range_default_and_cap) {
    struct db db;
    ASSERT_EQ(0, db_open(&db, ":memory:"));
    char sql[96];
    const long long now = (long long)time(NULL);
    const long long far_ts = now - 90000; /* beyond RANGE_MAX_SEC */
    const long long near_ts = now - 10;
    snprintf(sql, sizeof(sql), "INSERT INTO metrics VALUES (%lld, 1.5, 100, 0.5)", far_ts);
    ASSERT_EQ(0, db_exec(&db, sql));
    snprintf(sql, sizeof(sql), "INSERT INTO metrics VALUES (%lld, 2.5, 200, 0.6)", near_ts);
    ASSERT_EQ(0, db_exec(&db, sql));
    char far_txt[32];
    char near_txt[32];
    snprintf(far_txt, sizeof(far_txt), "%lld", far_ts);
    snprintf(near_txt, sizeof(near_txt), "%lld", near_ts);

    /* garbage range falls back to the default window */
    const char *body = serve(&db, "/api/metrics?range=abc");
    ASSERT_NE(NULL, strstr(body, near_txt));
    ASSERT_EQ(NULL, strstr(body, far_txt));

    /* huge range is capped at 86400 */
    body = serve(&db, "/api/metrics?range=999999999");
    ASSERT_NE(NULL, strstr(body, near_txt));
    ASSERT_EQ(NULL, strstr(body, far_txt));
    release(&g_conn);
    ASSERT_EQ(0, db_close(&db));
}

UTEST(metrics, range_overflow_caps_and_downsamples) {
    /* a file db mirrors production: the arena only holds the page cache,
     * a :memory: monster this size would fragment it for later tests */
    const char db_path[] = "/tmp/webplatform_metrics_grow.db";
    struct db db;
    remove(db_path);
    ASSERT_EQ(0, db_open(&db, db_path));
    const long long now = (long long)time(NULL);
    ASSERT_EQ(0, seed_rows(&db, SEED_ROWS_MANY, now));
    const long long oldest = now - (long long)(SEED_ROWS_MANY - 1) * SEED_STEP_SEC;
    char oldest_txt[32];
    char newest_txt[32];
    snprintf(oldest_txt, sizeof(oldest_txt), "%lld", oldest);
    snprintf(newest_txt, sizeof(newest_txt), "%lld", now);

    const char *body = serve(&db, "/api/metrics?range=86400");
    ASSERT_NE(NULL, strstr(g_conn.resp.head, core_http_status_ok));
    /* downsampling keeps every point under the cap and covers both ends:
     * dropping the newest rows would hide the last half hour */
    ASSERT_TRUE(count_ts_points(body) <= EXPECTED_MAX_POINTS);
    ASSERT_NE(NULL, strstr(body, oldest_txt));
    ASSERT_NE(NULL, strstr(body, newest_txt));
    release(&g_conn);
    ASSERT_EQ(0, db_close(&db));
    remove(db_path);
}

UTEST(metrics, range_cap_includes_half_day) {
    struct db db;
    ASSERT_EQ(0, db_open(&db, ":memory:"));
    char sql[96];
    const long long now = (long long)time(NULL);
    const long long half_day_ts = now - 43200; /* inside the 86400 cap */
    const long long near_ts = now - 10;
    snprintf(sql, sizeof(sql), "INSERT INTO metrics VALUES (%lld, 1.5, 100, 0.5)", half_day_ts);
    ASSERT_EQ(0, db_exec(&db, sql));
    snprintf(sql, sizeof(sql), "INSERT INTO metrics VALUES (%lld, 2.5, 200, 0.6)", near_ts);
    ASSERT_EQ(0, db_exec(&db, sql));
    char half_day_txt[32];
    snprintf(half_day_txt, sizeof(half_day_txt), "%lld", half_day_ts);

    /* a 9+ digit range is clamped to 86400, not fallen back to default */
    const char *body = serve(&db, "/api/metrics?range=999999999");
    ASSERT_NE(NULL, strstr(body, half_day_txt));
    release(&g_conn);
    ASSERT_EQ(0, db_close(&db));
}

UTEST(metrics, degraded_db_answers_503) {
    struct db db;
    memset(&db, 0, sizeof(db)); /* broken/unopened store: handle NULL */
    const char *body = serve(&db, "/api/metrics?range=60");
    (void)body;
    ASSERT_NE(NULL, strstr(g_conn.resp.head, core_http_status_unavailable));
    release(&g_conn);
}

UTEST(metrics, pool_exhaustion_answers_503) {
    struct db db;
    ASSERT_EQ(0, db_open(&db, ":memory:"));
    ASSERT_EQ(0, seed_rows(&db, SEED_ROWS_FEW, (long long)time(NULL)));

    /* four distinct live conns hold all slots */
    for (int i = 0; i < 4; ++i) {
        memset(&g_owners[i], 0, sizeof(g_owners[i]));
        g_owners[i].fd = 10 + i;
        ASSERT_EQ(0, metrics_serve(&db, &g_owners[i], CORE_SV("/api/metrics")));
        ASSERT_NE(NULL, strstr(g_owners[i].resp.head, core_http_status_ok));
    }

    /* the fifth gets 503, not a hang and not a crash */
    memset(&g_extra, 0, sizeof(g_extra));
    g_extra.fd = 50;
    ASSERT_EQ(0, metrics_serve(&db, &g_extra, CORE_SV("/api/metrics")));
    ASSERT_NE(NULL, strstr(g_extra.resp.head, core_http_status_unavailable));
    for (int i = 0; i < 4; ++i) {
        release(&g_owners[i]); /* the loop releases conns after replies */
    }
    release(&g_extra);
    ASSERT_EQ(0, db_close(&db));
}

UTEST(metrics, pool_checkout_reaps_released_conns) {
    struct db db;
    ASSERT_EQ(0, db_open(&db, ":memory:"));
    ASSERT_EQ(0, seed_rows(&db, SEED_ROWS_FEW, (long long)time(NULL)));

    for (int i = 0; i < 4; ++i) {
        memset(&g_owners[i], 0, sizeof(g_owners[i]));
        g_owners[i].fd = 20 + i;
        ASSERT_EQ(0, metrics_serve(&db, &g_owners[i], CORE_SV("/api/metrics")));
    }
    memset(&g_extra, 0, sizeof(g_extra));
    g_extra.fd = 60;
    ASSERT_EQ(0, metrics_serve(&db, &g_extra, CORE_SV("/api/metrics")));
    ASSERT_NE(NULL, strstr(g_extra.resp.head, core_http_status_unavailable));

    /* a released conn (fd == -1) frees its slot on the next serve */
    release(&g_owners[0]);
    ASSERT_EQ(0, metrics_serve(&db, &g_extra, CORE_SV("/api/metrics")));
    ASSERT_NE(NULL, strstr(g_extra.resp.head, core_http_status_ok));
    for (int i = 0; i < 4; ++i) {
        release(&g_owners[i]);
    }
    release(&g_extra);
    ASSERT_EQ(0, db_close(&db));
}

UTEST(metrics, module_is_zero_heap) {
    struct db db;
    g_malloc_calls = 0;
    ASSERT_EQ(0, db_open(&db, ":memory:"));
    ASSERT_EQ(0, seed_rows(&db, SEED_ROWS_FEW, (long long)time(NULL)));
    (void)serve(&db, "/api/metrics?range=3600");
    release(&g_conn);
    ASSERT_EQ(0, db_close(&db));
    ASSERT_EQ(0, g_malloc_calls);
}

UTEST_MAIN();
