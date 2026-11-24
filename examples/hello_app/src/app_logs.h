#pragma once

/* GET /api/logs: the core_log ring buffer as JSON ({"lines":[...]},
 * oldest first) in a pooled body slot. Zero heap. */

struct core_conn;

/* 0 when a response is filled (including 503 when the pool is
 * exhausted), -1 on a fatal error (the caller closes the connection).
 * Contract: conn must live in the static connection pool (the pooled
 * body slots are reaped by fd), same as metrics_serve. */
int app_logs_serve(struct core_conn *conn);
