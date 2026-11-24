#include "db.h"

#include <stddef.h>

/* Static arena: SQLite's only allocator (SQLITE_CONFIG_HEAP, memsys5).
 * Sized for the metrics workload: page cache 64 x 4 KiB plus working
 * set; the soak test in tests/test_db.c proves it is enough. */
enum {
    DB_ARENA_SIZE = 512 * 1024,
    DB_ARENA_MIN_ALLOC = 8,
};

static _Alignas(16) unsigned char g_arena[DB_ARENA_SIZE];
static int g_sqlite_configured;

static const char DB_SCHEMA[] = "CREATE TABLE IF NOT EXISTS metrics ("
                                "ts INTEGER PRIMARY KEY,"
                                "cpu_busy REAL NOT NULL,"
                                "mem_kb INTEGER NOT NULL,"
                                "load1 REAL NOT NULL);"
                                "CREATE TABLE IF NOT EXISTS sessions ("
                                "token TEXT PRIMARY KEY,"
                                "expires_at INTEGER NOT NULL)";

/* Pragmas: the page cache must fit the static arena with room to spare.
 * Under memsys5 every allocation rounds up to a power of two, so a
 * 4 KiB-ish page costs an 8 KiB chunk (plus a 64 KiB lookaside block):
 * cache_size=24 keeps the cache near 192 KiB inside the 512 KiB arena
 * and lets LRU recycle pages once the cap is reached. synchronous=NORMAL
 * costs one fsync-class sync per commit, which keeps the rollback
 * journal survivable across power loss (OFF can corrupt the file). */
static const char DB_PRAGMAS[] = "PRAGMA cache_size=24;"
                                 "PRAGMA synchronous=NORMAL;";

/* sqlite3_config is process-global and one-shot: before initialize,
 * never after. The guard makes repeated db_open calls safe.
 * SQLITE_THREADSAFE=0 compiles mutexes out entirely, so no
 * SQLITE_CONFIG_SINGLETHREAD call is needed (it would return
 * SQLITE_ERROR: that case is compiled out too). */
static int db_configure_once(void) {
    if (g_sqlite_configured) {
        return 0;
    }
    if (sqlite3_config(SQLITE_CONFIG_HEAP, g_arena, sizeof(g_arena), DB_ARENA_MIN_ALLOC) !=
        SQLITE_OK) {
        return -1;
    }
    if (sqlite3_initialize() != SQLITE_OK) {
        return -1;
    }
    g_sqlite_configured = 1;
    return 0;
}

int db_open(struct db *db, const char *path) {
    if (db == NULL || path == NULL || db_configure_once() != 0) {
        return -1;
    }
    sqlite3 *handle = NULL;
    if (sqlite3_open_v2(path, &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) !=
        SQLITE_OK) {
        sqlite3_close_v2(handle);
        return -1;
    }
    if (sqlite3_exec(handle, DB_PRAGMAS, NULL, NULL, NULL) != SQLITE_OK ||
        sqlite3_exec(handle, DB_SCHEMA, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close_v2(handle);
        return -1;
    }
    db->handle = handle;
    return 0;
}

int db_exec(struct db *db, const char *sql) {
    if (db == NULL || db->handle == NULL || sql == NULL) {
        return -1;
    }
    return sqlite3_exec(db->handle, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

/* Adapts sqlite3_exec's char** callback to the db_row_fn contract */
struct query_trampoline {
    db_row_fn row;
    void *ctx;
};

static int query_trampoline(void *raw, int cols, char **values, char **names) {
    (void)names;
    struct query_trampoline *t = raw;
    return t->row(t->ctx, cols, (const char **)values);
}

int db_query(struct db *db, const char *sql, db_row_fn row, void *ctx) {
    if (db == NULL || db->handle == NULL || sql == NULL || row == NULL) {
        return -1;
    }
    struct query_trampoline trampoline = {row, ctx};
    return sqlite3_exec(db->handle, sql, query_trampoline, &trampoline, NULL) == SQLITE_OK ? 0 : -1;
}

int db_close(struct db *db) {
    if (db == NULL || db->handle == NULL) {
        return -1;
    }
    const int rc = sqlite3_close_v2(db->handle) == SQLITE_OK ? 0 : -1;
    db->handle = NULL;
    return rc;
}

long db_changes(struct db *db) {
    if (db == NULL || db->handle == NULL) {
        return 0;
    }
    return sqlite3_changes64(db->handle);
}
