#pragma once
#include <stddef.h>

#include "core/http.h"

/* GET /api/metrics?range=N: renders stored metric rows as uPlot-shaped
 * JSON ({"ts":[...],"cpu":[...],"mem":[...],"load":[...]}) into a pooled
 * static body buffer and fills conn->resp (head + one body iov). */

struct core_conn;
struct db;

/* Serves the endpoint. range defaults to 3600 s and is capped at
 * 86400 s; ranges beyond 720 points are downsampled. Returns 0 when a
 * response is filled (including 503 when the pool is exhausted), -1 on
 * a fatal error (the caller closes the connection).
 *
 * Contract: conn must live in the static connection pool (the module
 * reads conn->fd on later serves to reap slots of released
 * connections). Stack conns are not supported. */
int metrics_serve(struct db *db, struct core_conn *conn, core_str_view_t path);
