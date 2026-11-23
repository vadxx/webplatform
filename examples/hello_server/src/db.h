#pragma once
#include <sqlite3.h>

/* Internal kernel-side seam over the vendored SQLite amalgamation.
 * Zero heap: SQLite runs on a static arena configured once per process
 * (see db.c). Callers pass struct db by address and never touch sqlite3
 * directly, so a future storage backend can replace the implementation. */

struct db {
    sqlite3 *handle;
};

/* Opens (creating if needed) the database at path, applies pragmas and
 * the schema. 0 ok, -1 error. Safe to call repeatedly. */
int db_open(struct db *db, const char *path);

/* Runs a statement that returns no rows (INSERT/DELETE/CREATE/PRAGMA).
 * 0 ok, -1 error. */
int db_exec(struct db *db, const char *sql);

/* Runs a statement, calling row() for every result row. Column values
 * arrive as text valid only inside the callback. 0 ok, -1 error. */
typedef int (*db_row_fn)(void *ctx, int cols, const char *values[]);
int db_query(struct db *db, const char *sql, db_row_fn row, void *ctx);

/* Closes the handle. 0 ok, -1 error. */
int db_close(struct db *db);
