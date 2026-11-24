#include "sessions.h"

#include "core/log.h"
#include "core/session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char LOG_TAG[] = "sessions";

/* Mints come from the registry (hex) and cookies arrive raw: never let
 * an unchecked token near SQL. */
static int token_is_hex32(const char *token) {
    if (token == NULL || strlen(token) != CORE_SESSION_TOKEN_LEN) {
        return 0;
    }
    for (size_t i = 0; i < CORE_SESSION_TOKEN_LEN; ++i) {
        const char c = token[i];
        const int is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!is_hex) {
            return 0;
        }
    }
    return 1;
}

int app_sessions_save(struct db *db, const char *token, time_t expires_at) {
    if (db == NULL || !token_is_hex32(token)) {
        return -1;
    }
    char sql[128];
    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO sessions VALUES ('%s', %lld)", token,
             (long long)expires_at);
    return db_exec(db, sql);
}

int app_sessions_delete(struct db *db, const char *token) {
    if (db == NULL || !token_is_hex32(token)) {
        return -1;
    }
    char sql[96];
    snprintf(sql, sizeof(sql), "DELETE FROM sessions WHERE token = '%s'", token);
    return db_exec(db, sql);
}

struct hydrate_ctx {
    time_t now;
};

/* Returns 0 so sqlite3_exec continues to the next row (a non-zero
 * return aborts the whole query with SQLITE_ABORT). */
static int hydrate_row(void *raw, int cols, const char *values[]) {
    if (cols < 2 || values[0] == NULL || values[1] == NULL) {
        return 0;
    }
    const struct hydrate_ctx *ctx = raw;
    const char *token = values[0];
    const long long expires_at = atoll(values[1]);
    if (!token_is_hex32(token)) {
        core_log('W', LOG_TAG, "ignoring malformed stored token");
        return 0;
    }
    /* no SQL here: this runs inside sqlite3_exec's callback and the same
     * handle must not be re-entered */
    if (core_session_restore(token, CORE_SESSION_TOKEN_LEN,
                             (int)(expires_at - (long long)ctx->now)) != 1) {
        core_log('W', LOG_TAG, "stored token did not restore");
    }
    return 0;
}

int app_sessions_hydrate(struct db *db) {
    if (db == NULL) {
        return -1;
    }
    char sql[96];
    const long long now = (long long)time(NULL);
    snprintf(sql, sizeof(sql), "DELETE FROM sessions WHERE expires_at <= %lld", now);
    if (db_exec(db, sql) != 0) {
        return -1;
    }
    const struct hydrate_ctx ctx = {.now = (time_t)now};
    return db_query(db, "SELECT token, expires_at FROM sessions", hydrate_row, (void *)&ctx);
}
