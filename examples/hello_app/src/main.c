#include "app_logs.h"
#include "collect.h"
#include "core/event_loop.h"
#include "core/http.h"
#include "core/log.h"
#include "core/session.h"
#include "db.h"
#include "hello_app_htmx_assets.h"
#include "hello_app_index_assets.h"
#include "hello_app_login_assets.h"
#include "hello_app_style_assets.h"
#include "hello_app_uplot_css_assets.h"
#include "hello_app_uplot_js_assets.h"
#include "login.h"
#include "metrics.h"
#include "sessions.h"

#include <stdlib.h>
#include <string.h>

/* Connection pool: static storage, zero heap */
enum {
    CONN_POOL_SIZE = 64,        /* one slot per connection */
    SERVER_PORT_DEFAULT = 8080, /* documented HTTP port convention */
    COLLECT_INTERVAL_SEC = 5,   /* one metrics row per tick */
};

static struct core_conn g_conns[CONN_POOL_SIZE];

static struct db g_db; /* sqlite handle + static arena live inside db.c */
static struct collect_ctx g_collect;

static const char ENV_DB_PATH[] = "WEBPLATFORM_DB";
static const char ENV_PASSWORD[] = "HELLO_APP_PASSWORD";
static const char DB_PATH_DEFAULT[] = "/data/metrics.db";
static const char METRICS_ROUTE[] = "/api/metrics";
static const char LOGS_ROUTE[] = "/api/logs";
static const char LOGIN_ROUTE[] = "/login";
static const char LOGOUT_ROUTE[] = "/logout";
static const char ROOT_ROUTE[] = "/";
static const char API_PREFIX[] = "/api/";
static const char METHOD_GET[] = "GET";
static const char METHOD_POST[] = "POST";
static const char LOG_TAG[] = "app";
static const char BODY_METHOD_NOT_ALLOWED[] = "method not allowed";
static const char BODY_UNAUTHORIZED[] = "unauthorized";

/* Public: no session needed - the login page, its form target and the
 * static assets the login page and dashboard shell load. */
static const struct core_http_asset_route g_public_routes[] = {
    {"/style.css", core_content_type_css, hello_app_style_gz, sizeof(hello_app_style_gz)},
    {"/htmx.min.js", core_content_type_js, hello_app_htmx_gz, sizeof(hello_app_htmx_gz)},
    {"/uplot.iife.min.js", core_content_type_js, hello_app_uplot_js_gz,
     sizeof(hello_app_uplot_js_gz)},
    {"/uplot.min.css", core_content_type_css, hello_app_uplot_css_gz,
     sizeof(hello_app_uplot_css_gz)},
};
static const struct core_http_routes g_public = {
    .assets = g_public_routes,
    .asset_count = CORE_ARRAY_LEN(g_public_routes),
};

/* Behind the gate: the dashboard shell only. */
static const struct core_http_asset_route g_gated_routes[] = {
    {"/", core_content_type_html, hello_app_index_gz, sizeof(hello_app_index_gz)},
};
static const struct core_http_routes g_gated = {
    .assets = g_gated_routes,
    .asset_count = CORE_ARRAY_LEN(g_gated_routes),
};

static const struct core_http_asset_route g_login_route[] = {
    {"/login", core_content_type_html, hello_app_login_gz, sizeof(hello_app_login_gz)},
};
static const struct core_http_routes g_login_asset = {
    .assets = g_login_route,
    .asset_count = CORE_ARRAY_LEN(g_login_route),
};

static int method_is(core_str_view_t req, const char *want) {
    core_str_view_t method = {0};
    if (core_http_request_method(req, &method) == NULL) {
        return 0;
    }
    return core_http_match_exact(method, want);
}

/* The gate: every request past the public routes needs a live session
 * cookie. Pages redirect to /login; API endpoints answer 401 so fetch
 * callers do not chase a redirect they cannot follow. */
static int gate_check(struct core_conn *conn, core_str_view_t req, core_str_view_t clean_path,
                      core_str_view_t *token) {
    const int has_cookie = core_http_cookie(req, app_cookie_name, token) == 1;
    if (has_cookie && core_session_valid(token->data, token->len) == 1) {
        return 1;
    }
    if (core_http_match_prefix(clean_path, API_PREFIX, &(core_str_view_t){0}) == 1) {
        core_http_serve_status(conn, core_http_status_unauthorized, BODY_UNAUTHORIZED);
    } else {
        core_http_serve_redirect(conn, LOGIN_ROUTE);
    }
    return 0;
}

static int handle_login(core_str_view_t req, struct core_conn *conn) {
    if (method_is(req, METHOD_POST) == 1) {
        return app_login_serve(conn);
    }
    if (method_is(req, METHOD_GET) == 1) {
        return core_http_serve_asset_route(conn, CORE_SV(LOGIN_ROUTE), &g_login_asset);
    }
    return core_http_serve_status(conn, core_http_status_method_not_allowed,
                                  BODY_METHOD_NOT_ALLOWED);
}

/* Behind the gate: the dashboard shell, logout and the JSON APIs. */
static int handle_gated(core_str_view_t req, core_str_view_t path, core_str_view_t token,
                        struct core_conn *conn) {
    const core_str_view_t clean = {.data = path.data, .len = core_http_path_only(path)};
    if (core_http_match_exact(clean, ROOT_ROUTE) == 1) {
        if (method_is(req, METHOD_GET) != 1) {
            return core_http_serve_status(conn, core_http_status_method_not_allowed,
                                          BODY_METHOD_NOT_ALLOWED);
        }
        return core_http_serve_asset_route(conn, clean, &g_gated);
    }
    if (core_http_match_exact(clean, LOGOUT_ROUTE) == 1) {
        if (method_is(req, METHOD_POST) != 1) {
            return core_http_serve_status(conn, core_http_status_method_not_allowed,
                                          BODY_METHOD_NOT_ALLOWED);
        }
        return app_logout_serve(conn, token);
    }
    if (core_http_match_exact(clean, METRICS_ROUTE) == 1) {
        if (method_is(req, METHOD_GET) != 1) {
            return core_http_serve_status(conn, core_http_status_method_not_allowed,
                                          BODY_METHOD_NOT_ALLOWED);
        }
        return metrics_serve(&g_db, conn, path);
    }
    if (core_http_match_exact(clean, LOGS_ROUTE) == 1) {
        if (method_is(req, METHOD_GET) != 1) {
            return core_http_serve_status(conn, core_http_status_method_not_allowed,
                                          BODY_METHOD_NOT_ALLOWED);
        }
        return app_logs_serve(conn);
    }
    return core_http_not_found(conn);
}

/* The public asset paths, matched without serving: non-GET requests to
 * them must answer 405 instead of silently reading. */
static int public_asset_requested(core_str_view_t clean) {
    for (size_t i = 0; i < CORE_ARRAY_LEN(g_public_routes); ++i) {
        if (core_http_match_exact(clean, g_public_routes[i].path) == 1) {
            return 1;
        }
    }
    return 0;
}

static int handle_request(struct core_conn *conn) {
    const core_str_view_t req = CORE_SV_BUF(conn->req_buf, conn->req_len);
    core_str_view_t path = {0};
    if (core_http_request_path(req, &path) == NULL) {
        return core_http_not_found(conn);
    }
    const core_str_view_t clean = {.data = path.data, .len = core_http_path_only(path)};

    /* /login is public: the page (GET) and the form target (POST) */
    if (core_http_match_exact(clean, LOGIN_ROUTE) == 1) {
        return handle_login(req, conn);
    }

    /* public static assets: GET serves, anything else is a 405 */
    if (public_asset_requested(clean) == 1) {
        if (method_is(req, METHOD_GET) != 1) {
            return core_http_serve_status(conn, core_http_status_method_not_allowed,
                                          BODY_METHOD_NOT_ALLOWED);
        }
        return core_http_serve_asset_route(conn, clean, &g_public);
    }

    /* everything below is behind the session gate */
    core_str_view_t token = {0};
    if (gate_check(conn, req, clean, &token) != 1) {
        return 0;
    }
    return handle_gated(req, path, token, conn);
}

/* Opens the store and hydrates persisted sessions into the registry.
 * A failed open keeps serving: the site stays alive, sessions and
 * metrics degrade. */
static void open_db_and_hydrate(const char *db_path) {
    if (db_open(&g_db, db_path) != 0) {
        core_log('E', LOG_TAG, "db_open %s failed, sessions and metrics degraded", db_path);
        return;
    }
    app_login_set_db(&g_db);
    if (app_sessions_hydrate(&g_db) != 0) {
        core_log('W', LOG_TAG, "session hydration failed, starting empty");
    }
}

int main(void) {
    const char *db_path = getenv(ENV_DB_PATH);
    if (db_path == NULL || db_path[0] == '\0') {
        db_path = DB_PATH_DEFAULT;
    }
    if (app_login_config(getenv(ENV_PASSWORD)) != 0) {
        /* an explicitly empty password would disable the gate: fail loud */
        core_log('E', LOG_TAG, "%s is set but empty", ENV_PASSWORD);
        return 1;
    }
    open_db_and_hydrate(db_path);

    const int timer_fd = core_timer_fd(COLLECT_INTERVAL_SEC);
    if (timer_fd < 0) {
        core_log('E', LOG_TAG, "collect timer failed");
        return 1;
    }
    g_collect.db = &g_db;
    g_collect.timer_fd = timer_fd;

    struct core_poll_source sources[] = {
        {timer_fd, collect_on_tick, &g_collect}, /* /proc -> INSERT, retention */
    };
    const struct core_poll_set poll_set = {
        .items = sources,
        .count = CORE_ARRAY_LEN(sources),
    };
    const struct core_conn_pool conn_pool = {
        .conns = g_conns,
        .count = CONN_POOL_SIZE,
    };

    const uint16_t port = core_http_port_env(SERVER_PORT_DEFAULT);
    const int listen_fd = core_listen(port);
    if (listen_fd < 0) {
        core_log('E', LOG_TAG, "core_listen failed");
        return 1;
    }
    core_log('I', LOG_TAG, "listening on %u, db %s", port, db_path);
    return core_loop_run(listen_fd, &conn_pool, handle_request, &poll_set);
}
