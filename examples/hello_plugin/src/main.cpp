#include <cstdio>
#include <cstring>

#include "../lib/hello_plugin.h"

#include "core/http.h"
#include "core/plugin_api.h"

int main(int argc, char **argv) {
    const char *plugin_path = argc > 1 ? argv[1] : HELLO_DEFAULT_PATH;

    /* host_api from the core's own libc (the core links musl normally) */
    struct host_api_t api = {};
    core_host_api_default(&api);

    core_plugin plugin = {};
    if (core_plugin_open(&plugin, plugin_path, &api) != 0) {
        fprintf(stderr, "plugin_host: cannot load plugin %s\n", plugin_path);
        return 1;
    }

    static const char request[] = "GET /demo HTTP/1.1";
    struct plugin_response_t resp = {};
    if (plugin.handle(request, sizeof(request) - 1, &resp) != 0) {
        fprintf(stderr, "plugin_host: plugin_handle failed\n");
        return 1;
    }

    if (strncmp(resp.head, HELLO_RESPONSE_MARKER, sizeof(HELLO_RESPONSE_MARKER) - 1) != 0) {
        fprintf(stderr, "plugin_host: unexpected response head\n");
        return 1;
    }

    for (int i = 0; i < resp.iov_count; ++i) {
        fwrite(resp.iov[i].iov_base, 1, resp.iov[i].iov_len, stdout);
    }
    fputc('\n', stdout);
    return 0;
}
