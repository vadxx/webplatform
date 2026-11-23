#include <stdio.h>
#include <stdlib.h>

#include "core/event_loop.h"
#include "core/http.h"
#include "core/plugin_api.h"
#include "hello_server_htmx_assets.h"
#include "hello_server_index_assets.h"

/* Connection pool: static storage, zero heap */
enum {
    CONN_POOL_SIZE = 64,        /* one slot per connection */
    SERVER_PORT_DEFAULT = 8080, /* documented HTTP port convention */
};

static struct core_conn g_conns[CONN_POOL_SIZE];

static struct core_plugin g_plugin;

/* Static routes: exact path -> embedded gzip asset */
static const struct core_http_asset_route g_asset_routes[] = {
    {"/", core_content_type_html, hello_server_index_gz, sizeof(hello_server_index_gz)},
    {"/htmx.min.js", core_content_type_js, hello_server_htmx_gz, sizeof(hello_server_htmx_gz)},
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
    return core_http_route(conn, &g_routes);
}

int main(void) {
    struct host_api_t api;
    core_host_api_default(&api);
    if (core_plugin_open(&g_plugin, PLUGIN_PATH, &api) != 0) {
        fprintf(stderr, "hello_server: cannot load plugin %s\n", PLUGIN_PATH);
        return 1;
    }
    const int listen_fd = core_listen(core_http_port_env(SERVER_PORT_DEFAULT));
    if (listen_fd < 0) {
        fprintf(stderr, "hello_server: core_listen failed\n");
        return 1;
    }
    const struct core_conn_pool pool = {
        .conns = g_conns,
        .count = CONN_POOL_SIZE,
    };
    return core_loop_run(listen_fd, &pool, handle_request, NULL);
}
