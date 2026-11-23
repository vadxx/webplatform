#include "collect.h"
#include "core/event_loop.h"
#include "core/http.h"
#include "core/log.h"
#include "core/plugin_api.h"
#include "db.h"
#include "hello_server_htmx_assets.h"
#include "hello_server_index_assets.h"
#include "hello_server_uplot_css_assets.h"
#include "hello_server_uplot_js_assets.h"
#include "metrics.h"

#include <stdlib.h>

/* Connection pool: static storage, zero heap */
enum {
    CONN_POOL_SIZE = 64,        /* one slot per connection */
    SERVER_PORT_DEFAULT = 8080, /* documented HTTP port convention */
    COLLECT_INTERVAL_SEC = 5,   /* one metrics row per tick */
};

static struct core_conn g_conns[CONN_POOL_SIZE];

static struct core_plugin g_plugin;
static struct db g_db; /* sqlite handle + static arena live inside db.c */
static struct collect_ctx g_collect;

static const char ENV_DB_PATH[] = "WEBPLATFORM_DB";
static const char DB_PATH_DEFAULT[] = "/data/metrics.db";
static const char METRICS_ROUTE[] = "/api/metrics";
static const char LOG_TAG[] = "server";

/* Static routes: exact path -> embedded gzip asset */
static const struct core_http_asset_route g_asset_routes[] = {
    {"/", core_content_type_html, hello_server_index_gz, sizeof(hello_server_index_gz)},
    {"/htmx.min.js", core_content_type_js, hello_server_htmx_gz, sizeof(hello_server_htmx_gz)},
    {"/uplot.iife.min.js", core_content_type_js, hello_server_uplot_js_gz,
     sizeof(hello_server_uplot_js_gz)},
    {"/uplot.min.css", core_content_type_css, hello_server_uplot_css_gz,
     sizeof(hello_server_uplot_css_gz)},
};

/* Plugin routes: prefix -> plugin (the tail segment becomes the plugin request) */
static const struct core_http_plugin_route g_plugin_routes[] = {
    {"/plugin/", &g_plugin},
};

/* Both tables for the router */
static const struct core_http_routes g_routes = {
    .assets = g_asset_routes,
    .asset_count = CORE_ARRAY_LEN(g_asset_routes),
    .plugins = g_plugin_routes,
    .plugin_count = CORE_ARRAY_LEN(g_plugin_routes),
};

static int handle_request(struct core_conn *conn) {
    core_str_view_t path = {0};
    const char *path_data =
        core_http_request_path(CORE_SV_BUF(conn->req_buf, conn->req_len), &path);
    if (path_data != NULL) {
        const core_str_view_t clean = {.data = path.data, .len = core_http_path_only(path)};
        if (core_http_match_exact(clean, METRICS_ROUTE) == 1) {
            return metrics_serve(&g_db, conn, path);
        }
    }
    return core_http_route(conn, &g_routes);
}

int main(void) {
    struct host_api_t api;
    core_host_api_default(&api);
    if (core_plugin_open(&g_plugin, PLUGIN_PATH, &api) != 0) {
        core_log('E', LOG_TAG, "cannot load plugin %s", PLUGIN_PATH);
        return 1;
    }

    const char *db_path = getenv(ENV_DB_PATH);
    if (db_path == NULL || db_path[0] == '\0') {
        db_path = DB_PATH_DEFAULT;
    }
    g_collect.db = &g_db;
    if (db_open(&g_db, db_path) != 0) {
        /* keep serving: the site stays alive, /api/metrics answers 503 */
        core_log('E', LOG_TAG, "db_open %s failed, metrics degraded", db_path);
    }

    const int timer_fd = core_timer_fd(COLLECT_INTERVAL_SEC);
    if (timer_fd < 0) {
        core_log('E', LOG_TAG, "collect timer failed");
        return 1;
    }
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
    core_log('I', LOG_TAG, "listening on %u, metrics into %s", port, db_path);
    return core_loop_run(listen_fd, &conn_pool, handle_request, &poll_set);
}
