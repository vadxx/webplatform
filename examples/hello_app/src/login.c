#include "login.h"

#include "core/log.h"
#include "core/session.h"
#include "db.h"
#include "sessions.h"

#include <string.h>
#include <time.h>

static const char LOGIN_LOG_TAG[] = "login";
const char app_cookie_name[] = "app_session";
static const char LOGIN_PATH[] = "/login";
static const char ROOT_PATH[] = "/";
static const char PASSWORD_FIELD[] = "password";
static const char PASSWORD_DEFAULT[] = "admin";

static const char *g_password = PASSWORD_DEFAULT;
static struct db *g_db; /* NULL: no write-through (memory-only sessions) */

int app_login_config(const char *password) {
    if (password != NULL && password[0] == '\0') {
        return -1;
    }
    if (password == NULL) {
        core_log('W', LOGIN_LOG_TAG, "demo default password active");
        g_password = PASSWORD_DEFAULT;
        return 0;
    }
    g_password = password;
    return 0;
}

void app_login_set_db(struct db *db) { g_db = db; }

int app_login_check(core_str_view_t password) {
    const size_t expected_len = strlen(g_password);
    return password.len == expected_len && memcmp(password.data, g_password, expected_len) == 0 ? 1
                                                                                                : 0;
}

/* cookie flags stay in the app layer: core_http_cookie_spec.extra */
static const char COOKIE_ATTRS[] = "SameSite=Lax";

/* core_http_set_cookie edits the head buffer via strlen and knows nothing
 * of resp bookkeeping: after inserting, the lengths must be recomputed
 * or the client receives a head cut short of the Set-Cookie line, and
 * the loop cuts the connection at total_len. */
static void refresh_head_lengths(struct core_conn *conn) {
    const size_t old_head_len = conn->resp.head_len;
    conn->resp.head_len = strlen(conn->resp.head);
    conn->resp.iov[0].iov_len = conn->resp.head_len;
    conn->resp.total_len += conn->resp.head_len - old_head_len;
}

static int set_session_cookie(struct core_conn *conn, core_str_view_t value, int max_age_sec) {
    const struct core_http_cookie_spec spec = {
        .name = app_cookie_name,
        .value = value,
        .max_age_sec = max_age_sec,
        .extra = COOKIE_ATTRS,
    };
    return core_http_set_cookie(conn->resp.head, sizeof(conn->resp.head), &spec);
}

/* Closes the session behind a raw cookie token and deletes its row.
 * SQL only sees a validated hex token (app_sessions_delete guards). */
static void session_revoke(core_str_view_t token) {
    if (token.len != CORE_SESSION_TOKEN_LEN) {
        return;
    }
    char token_z[CORE_SESSION_TOKEN_LEN + 1];
    memcpy(token_z, token.data, token.len);
    token_z[token.len] = '\0';
    core_session_close(token_z, token.len);
    if (g_db != NULL && app_sessions_delete(g_db, token_z) != 0) {
        core_log('W', LOGIN_LOG_TAG, "session store delete failed");
    }
}

/* Re-login replaces the session: the cookie the request already carries
 * would otherwise keep its slot and row until the TTL (32 logins
 * without logout lock the table, and hydrate restores the lockout). */
static void close_existing_session(struct core_conn *conn) {
    const core_str_view_t req = CORE_SV_BUF(conn->req_buf, conn->req_len);
    core_str_view_t token = {0};
    if (core_http_cookie(req, app_cookie_name, &token) == 1) {
        session_revoke(token);
    }
}

int app_login_serve(struct core_conn *conn) {
    if (conn == NULL) {
        return -1;
    }
    core_str_view_t password = {0};
    const int found =
        core_http_form_field(CORE_SV_BUF(conn->body, conn->body_len), PASSWORD_FIELD, &password);
    if (found != 1 || app_login_check(password) != 1) {
        return core_http_serve_redirect(conn, LOGIN_PATH);
    }
    close_existing_session(conn);
    char token[CORE_SESSION_TOKEN_LEN + 1];
    if (core_session_open(APP_SESSION_TTL_SEC, token) == NULL) {
        core_log('E', LOGIN_LOG_TAG, "session table full");
        return core_http_serve_status(conn, core_http_status_unavailable, "session table full");
    }
    if (g_db != NULL && app_sessions_save(g_db, token, time(NULL) + APP_SESSION_TTL_SEC) != 0) {
        /* the session lives in the registry; only persistence degrades */
        core_log('W', LOGIN_LOG_TAG, "session store write failed");
    }
    if (core_http_serve_redirect(conn, ROOT_PATH) != 0) {
        return -1;
    }
    if (set_session_cookie(conn, CORE_SV_BUF(token, CORE_SESSION_TOKEN_LEN), APP_SESSION_TTL_SEC) !=
        0) {
        return -1;
    }
    refresh_head_lengths(conn);
    return 0;
}

int app_logout_serve(struct core_conn *conn, core_str_view_t token) {
    if (conn == NULL) {
        return -1;
    }
    session_revoke(token);
    if (core_http_serve_redirect(conn, LOGIN_PATH) != 0) {
        return -1;
    }
    if (set_session_cookie(conn, CORE_SV(""), 0) != 0) {
        return -1;
    }
    refresh_head_lengths(conn);
    return 0;
}
