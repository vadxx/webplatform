#include "../../hello_server/src/db.c"
#include "../src/login.c"
#include "../src/sessions.c"
#include "utest.h"
#include <stdio.h>
#include <string.h>

/* Allocation counter (--wrap=malloc): login must stay zero-heap */
extern void *__real_malloc(size_t size);

static int g_malloc_calls;

__attribute__((visibility("default"))) void *__wrap_malloc(size_t size) {
    ++g_malloc_calls;
    return __real_malloc(size);
}

static const char DB_PATH[] = "/tmp/webplatform-test-login.db";
static const char PASSWORD[] = "s3cret";
static const char COOKIE_PREFIX[] = "Set-Cookie: app_session=";

/* Conns mirror the server: static storage, fd marks liveness */
static struct core_conn g_conn;

static const char *g_body;

static void conn_reset(void) {
    memset(&g_conn, 0, sizeof(g_conn));
    g_conn.fd = 7; /* any occupied descriptor */
    if (g_body != NULL) {
        memcpy(g_conn.body, g_body, strlen(g_body));
        g_conn.body_len = strlen(g_body);
    }
}

/* A request that already carries the app_session cookie (re-login) */
static void conn_reset_with_cookie(const char *token) {
    static const char REQ_HEAD[] = "POST /login HTTP/1.1\r\nCookie: app_session=";
    conn_reset();
    size_t pos = sizeof(REQ_HEAD) - 1;
    memcpy(g_conn.req_buf, REQ_HEAD, pos);
    memcpy(g_conn.req_buf + pos, token, strlen(token));
    pos += strlen(token);
    memcpy(g_conn.req_buf + pos, "\r\n\r\n", 4);
    g_conn.req_len = pos + 4;
}

/* Extracts the 32-hex session token from a Set-Cookie line in head; 1 found */
static int cookie_token(const char *head, char out[CORE_SESSION_TOKEN_LEN + 1]) {
    const char *at = strstr(head, COOKIE_PREFIX);
    if (at == NULL) {
        return 0;
    }
    at += sizeof(COOKIE_PREFIX) - 1;
    memcpy(out, at, CORE_SESSION_TOKEN_LEN);
    out[CORE_SESSION_TOKEN_LEN] = '\0';
    return 1;
}

struct count_sink_login {
    int count;
};

static int count_row_for_login(void *ctx, int cols, const char *values[]) {
    (void)cols;
    (void)values;
    ++((struct count_sink_login *)ctx)->count;
    return 0;
}

static int db_reset(struct db *db) {
    remove(DB_PATH);
    return db_open(db, DB_PATH);
}

UTEST(login, check_compares_configured_password) {
    app_login_config(PASSWORD);
    ASSERT_EQ(1, app_login_check(CORE_SV(PASSWORD)));
    ASSERT_EQ(0, app_login_check(CORE_SV("s3cretX"))); /* same length, other bytes */
    ASSERT_EQ(0, app_login_check(CORE_SV("wrong")));   /* other length */
    ASSERT_EQ(0, app_login_check(CORE_SV("")));        /* empty */
}

UTEST(login, valid_password_opens_session_and_redirects) {
    struct db db;
    char token[CORE_SESSION_TOKEN_LEN + 1];
    app_login_config(PASSWORD);
    app_login_set_db(&db);
    ASSERT_EQ(0, db_reset(&db));
    g_body = "password=s3cret";
    conn_reset();
    ASSERT_EQ(0, app_login_serve(&g_conn));
    ASSERT_NE(NULL, strstr(g_conn.resp.head, core_http_status_redirect));
    ASSERT_NE(NULL, strstr(g_conn.resp.head, "Location: /\r\n"));
    ASSERT_EQ(1, cookie_token(g_conn.resp.head, token));
    ASSERT_EQ(1, core_session_valid(token, strlen(token)));
    /* cookie flags live in the app layer: SameSite rides along */
    ASSERT_NE(NULL, strstr(g_conn.resp.head, "SameSite=Lax"));
    /* the store holds the session (write-through) */
    struct count_sink_login sink = {0};
    ASSERT_EQ(0, db_query(&db, "SELECT token FROM sessions", count_row_for_login, &sink));
    ASSERT_EQ(1, sink.count);
    /* Set-Cookie insertion is reflected in lengths (Review Focus 2) */
    ASSERT_EQ(strlen(g_conn.resp.head), g_conn.resp.head_len);
    ASSERT_EQ(g_conn.resp.head_len, g_conn.resp.iov[0].iov_len);
    /* the loop cuts the connection by total_len: it must count the line */
    ASSERT_EQ(g_conn.resp.head_len, g_conn.resp.total_len);
    ASSERT_EQ(0, db_close(&db));
}

UTEST(login, wrong_password_redirects_without_cookie) {
    app_login_config(PASSWORD);
    g_body = "password=nope";
    conn_reset();
    ASSERT_EQ(0, app_login_serve(&g_conn));
    ASSERT_NE(NULL, strstr(g_conn.resp.head, "Location: /login"));
    ASSERT_EQ(NULL, strstr(g_conn.resp.head, COOKIE_PREFIX));
}

UTEST(login, missing_password_field_redirects_to_login) {
    app_login_config(PASSWORD);
    g_body = "other=1";
    conn_reset();
    ASSERT_EQ(0, app_login_serve(&g_conn));
    ASSERT_NE(NULL, strstr(g_conn.resp.head, "Location: /login"));
    ASSERT_EQ(NULL, strstr(g_conn.resp.head, COOKIE_PREFIX));
}

UTEST(login, empty_body_redirects_to_login) {
    app_login_config(PASSWORD);
    g_body = "";
    conn_reset();
    g_conn.body_len = 0;
    ASSERT_EQ(0, app_login_serve(&g_conn));
    ASSERT_NE(NULL, strstr(g_conn.resp.head, "Location: /login"));
}

UTEST(login, logout_closes_session_and_sweeps_store) {
    struct db db;
    char token[CORE_SESSION_TOKEN_LEN + 1];
    app_login_config(PASSWORD);
    app_login_set_db(&db);
    ASSERT_EQ(0, db_reset(&db));
    g_body = "password=s3cret";
    conn_reset();
    ASSERT_EQ(0, app_login_serve(&g_conn));
    ASSERT_EQ(1, cookie_token(g_conn.resp.head, token));

    conn_reset();
    ASSERT_EQ(0, app_logout_serve(&g_conn, CORE_SV_BUF(token, strlen(token))));
    ASSERT_EQ(0, core_session_valid(token, strlen(token)));
    ASSERT_NE(NULL, strstr(g_conn.resp.head, "Location: /login"));
    ASSERT_NE(NULL, strstr(g_conn.resp.head, "Max-Age=0"));
    ASSERT_EQ(strlen(g_conn.resp.head), g_conn.resp.total_len);
    struct count_sink_login sink = {0};
    ASSERT_EQ(0, db_query(&db, "SELECT token FROM sessions", count_row_for_login, &sink));
    ASSERT_EQ(0, sink.count);
    ASSERT_EQ(0, db_close(&db));
}

/* Re-login must replace the previous session, not stack a second one:
 * leaked tokens keep their registry slot and their SQLite row until the
 * TTL, so 32 logins without logout lock the table - and hydrate
 * restores that lockout on restart. */
UTEST(login, relogin_replaces_previous_session) {
    struct db db;
    char token1[CORE_SESSION_TOKEN_LEN + 1];
    char token2[CORE_SESSION_TOKEN_LEN + 1];
    app_login_config(PASSWORD);
    app_login_set_db(&db);
    ASSERT_EQ(0, db_reset(&db));
    g_body = "password=s3cret";
    conn_reset();
    ASSERT_EQ(0, app_login_serve(&g_conn));
    ASSERT_EQ(1, cookie_token(g_conn.resp.head, token1));

    conn_reset_with_cookie(token1);
    ASSERT_EQ(0, app_login_serve(&g_conn));
    ASSERT_EQ(1, cookie_token(g_conn.resp.head, token2));
    ASSERT_EQ(0, core_session_valid(token1, strlen(token1)));
    ASSERT_EQ(1, core_session_valid(token2, strlen(token2)));
    ASSERT_EQ(strlen(g_conn.resp.head), g_conn.resp.total_len);
    /* the old row is deleted, the new one is stored: one row total */
    struct count_sink_login sink = {0};
    ASSERT_EQ(0, db_query(&db, "SELECT token FROM sessions", count_row_for_login, &sink));
    ASSERT_EQ(1, sink.count);
    ASSERT_EQ(0, db_close(&db));
}

/* An explicitly empty HELLO_APP_PASSWORD must not silently disable the
 * gate: refusal (-1) leaves the previous configuration active. */
UTEST(login, config_refuses_empty_password) {
    app_login_config(PASSWORD);
    ASSERT_EQ(-1, app_login_config(""));
    ASSERT_EQ(1, app_login_check(CORE_SV(PASSWORD)));
}

/* NULL env falls back to the demo default, loudly. */
UTEST(login, config_null_falls_back_to_demo_default) {
    ASSERT_EQ(0, app_login_config(NULL));
    ASSERT_EQ(1, app_login_check(CORE_SV("admin")));
}

UTEST(login, null_db_login_still_works_in_memory) {
    app_login_config(PASSWORD);
    app_login_set_db(NULL);
    g_body = "password=s3cret";
    conn_reset();
    g_malloc_calls = 0; /* delta: only the serve call is counted */
    ASSERT_EQ(0, app_login_serve(&g_conn));
    ASSERT_NE(NULL, strstr(g_conn.resp.head, "Location: /"));
    ASSERT_NE(NULL, strstr(g_conn.resp.head, COOKIE_PREFIX));
    ASSERT_EQ(0, g_malloc_calls);
}

UTEST_MAIN();
