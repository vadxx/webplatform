#include "metrics.h"

#include "core/event_loop.h"
#include "core/http.h"
#include "core/log.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    MAX_POINTS = 720,
    RANGE_DEFAULT_SEC = 3600,
    RANGE_MAX_SEC = 86400,
    RANGE_MIN_SEC = 1,
};

static const char BUSY_BODY[] = "metrics busy\n";
static const char RANGE_PARAM[] = "range";
static const char LOG_TAG[] = "metrics";
static const char SELECT_COUNT_FMT[] = "SELECT COUNT(*) FROM metrics WHERE ts >= %lld";
static const char SELECT_ROWS_FMT[] =
    "SELECT ts, cpu_busy, mem_kb, load1 FROM metrics WHERE ts >= %lld ORDER BY ts";

/* Single-threaded render state: row cache filled by the data callback */
struct row_cache {
    long long ts[MAX_POINTS];
    double cpu[MAX_POINTS];
    long mem[MAX_POINTS];
    double load[MAX_POINTS];
    int n;
};
static struct row_cache g_cache;
static long long g_seen_rows;
static long long g_total_rows;
static int g_stride;

/* One warn line per degraded streak, not one per request */
static int g_busy_streak;

static int count_row(void *ctx, int cols, const char *values[]) {
    (void)ctx;
    (void)cols;
    g_total_rows = atoll(values[0]);
    return 0;
}

static int data_row(void *ctx, int cols, const char *values[]) {
    (void)ctx;
    const long long index = g_seen_rows++;
    /* stride sampling plus the last row: downsampling must keep both ends */
    if (cols < 4 || g_cache.n >= MAX_POINTS) {
        return 0;
    }
    if ((index % g_stride) != 0 && index != g_total_rows - 1) {
        return 0;
    }
    g_cache.ts[g_cache.n] = atoll(values[0]);
    g_cache.cpu[g_cache.n] = atof(values[1]);
    g_cache.mem[g_cache.n] = atol(values[2]);
    g_cache.load[g_cache.n] = atof(values[3]);
    ++g_cache.n;
    return 0;
}

static int compose_json(char *dst, size_t cap) {
    size_t pos = 0;
    if (core_http_append(dst, cap, &pos, "{\"ts\":[") != 0) {
        return -1;
    }
    for (int i = 0; i < g_cache.n; ++i) {
        if (core_http_append(dst, cap, &pos, "%s%lld", i > 0 ? "," : "", g_cache.ts[i]) != 0) {
            return -1;
        }
    }
    if (core_http_append(dst, cap, &pos, "],\"cpu\":[") != 0) {
        return -1;
    }
    for (int i = 0; i < g_cache.n; ++i) {
        if (core_http_append(dst, cap, &pos, "%s%.2f", i > 0 ? "," : "", g_cache.cpu[i]) != 0) {
            return -1;
        }
    }
    if (core_http_append(dst, cap, &pos, "],\"mem\":[") != 0) {
        return -1;
    }
    for (int i = 0; i < g_cache.n; ++i) {
        if (core_http_append(dst, cap, &pos, "%s%ld", i > 0 ? "," : "", g_cache.mem[i]) != 0) {
            return -1;
        }
    }
    if (core_http_append(dst, cap, &pos, "],\"load\":[") != 0) {
        return -1;
    }
    for (int i = 0; i < g_cache.n; ++i) {
        if (core_http_append(dst, cap, &pos, "%s%.3f", i > 0 ? "," : "", g_cache.load[i]) != 0) {
            return -1;
        }
    }
    return core_http_append(dst, cap, &pos, "]}");
}

/* Range parse + count + stride + row query + JSON composition into the
 * checked-out body slot. 0 ok, -1 store or composition failure. */
static int render_json(struct db *db, core_str_view_t path, char *body, size_t *body_len) {
    const struct core_http_range range = {
        .min = RANGE_MIN_SEC,
        .max = RANGE_MAX_SEC,
    };
    const long long range_sec = core_http_query_long(path, RANGE_PARAM, RANGE_DEFAULT_SEC, range);
    const long long from_ts = (long long)time(NULL) - range_sec;
    char sql[128];
    g_total_rows = 0;
    snprintf(sql, sizeof(sql), SELECT_COUNT_FMT, from_ts);
    if (db_query(db, sql, count_row, NULL) != 0) {
        return -1;
    }
    /* ceil: even coverage, the newest rows must survive downsampling */
    g_stride = g_total_rows > MAX_POINTS ? (int)((g_total_rows + MAX_POINTS - 1) / MAX_POINTS) : 1;
    g_seen_rows = 0;
    g_cache.n = 0;
    snprintf(sql, sizeof(sql), SELECT_ROWS_FMT, from_ts);
    if (db_query(db, sql, data_row, NULL) != 0) {
        return -1;
    }
    if (compose_json(body, CORE_BODY_SLOT_CAP) != 0) {
        return -1; /* overflow must never reach the wire truncated */
    }
    *body_len = strlen(body);
    return 0;
}

static int serve_busy(struct core_conn *conn, const char *why) {
    if (g_busy_streak == 0) {
        core_log('W', LOG_TAG, "%s", why);
    }
    ++g_busy_streak;
    return core_http_serve_status(conn, core_http_status_unavailable, BUSY_BODY);
}

int metrics_serve(struct db *db, struct core_conn *conn, core_str_view_t path) {
    if (db == NULL || conn == NULL || path.data == NULL) {
        return -1;
    }
    char *body = core_http_body_checkout(conn);
    if (body == NULL) {
        return serve_busy(conn, "response pool exhausted");
    }
    size_t body_len = 0;
    if (render_json(db, path, body, &body_len) != 0) {
        return serve_busy(conn, "store query failed");
    }
    g_busy_streak = 0; /* a full answer ends the degraded streak */
    return core_http_serve_body(conn, core_http_status_ok, core_content_type_json,
                                CORE_SV_BUF(body, body_len));
}
