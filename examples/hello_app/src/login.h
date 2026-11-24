#pragma once
#include <stddef.h>

#include "core/http.h"

struct db;

/* POST /login and POST /logout handlers plus the password check.
 * Password comes from HELLO_APP_PASSWORD (main passes the env value;
 * NULL or empty falls back to the demo default "admin"). A successful
 * login opens a core session, writes it through to SQLite and sets the
 * app_session cookie; logout closes and deletes it. The db pointer is
 * optional: without it login still works, sessions just do not persist. */

enum { APP_SESSION_TTL_SEC = 86400 };

/* Configures the expected password (pointer is remembered, not copied:
 * main passes a getenv result that outlives the process). NULL falls
 * back to the demo default "admin" with a warning; an explicitly empty
 * value is refused (-1, configuration unchanged) - a silently disabled
 * gate is worse than a failed start. 0 ok. */
int app_login_config(const char *password);

/* Sets the store for write-through; NULL disables persistence. */
void app_login_set_db(struct db *db);

/* 1 when the password slice matches the configured one. */
int app_login_check(core_str_view_t password);

/* POST /login: parses the "password" form field out of conn->body.
 * Success: 302 / + Set-Cookie app_session=<token> (Max-Age=APP_SESSION_TTL_SEC).
 * Failure: 302 /login without Set-Cookie. 0 ok, -1 fatal. */
int app_login_serve(struct core_conn *conn);

/* POST /logout: closes the session behind token, deletes the store row,
 * expires the cookie, 302 /login. 0 ok, -1 fatal. */
int app_logout_serve(struct core_conn *conn, core_str_view_t token);
