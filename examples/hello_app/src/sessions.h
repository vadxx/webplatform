#pragma once
#include <stddef.h>
#include <time.h>

#include "db.h"

/* Write-through persistence for the core session registry: login saves
 * the minted token, startup hydrates live tokens back into the registry,
 * logout deletes the row. The registry stays the single check path.
 * Zero heap: SQL is composed on the stack. Tokens are validated (32 hex
 * chars) before they ever reach SQL. */

/* INSERT OR REPLACE. 0 ok, -1 on bad args/DB failure. */
int app_sessions_save(struct db *db, const char *token, time_t expires_at);

/* DELETE by token; unknown tokens are not an error. 0 ok, -1 bad args. */
int app_sessions_delete(struct db *db, const char *token);

/* Registers every live row in the core registry (core_session_restore),
 * sweeps expired rows from the table. 0 ok, -1 on bad args/DB failure. */
int app_sessions_hydrate(struct db *db);
