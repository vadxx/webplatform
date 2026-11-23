#include "../lib/hello_plugin.h"

#include "core/plugin_api.h"
#include "utest.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Path via env (manual runs fall back to the sibling library) */
static const char *plugin_path(void) {
    const char *env = getenv("PLUGIN_SO");
    return env != NULL ? env : HELLO_DEFAULT_PATH;
}

static struct host_api_t fake_api = {.snprintf = snprintf, .memcpy = memcpy};

UTEST(load, exports_abi_and_handles) {
    struct plugin_response_t r;
    void *handle = dlopen(plugin_path(), RTLD_NOW);
    if (handle == NULL)
        printf("dlerror: %s\n", dlerror());
    ASSERT_NE(NULL, handle);

    int (*init)(const struct host_api_t *) = dlsym(handle, "plugin_init");
    int (*hdl)(const char *, size_t, struct plugin_response_t *) = dlsym(handle, "plugin_handle");
    ASSERT_NE(NULL, (void *)init);
    ASSERT_NE(NULL, (void *)hdl);

    ASSERT_EQ(0, init(&fake_api));
    ASSERT_EQ(0, hdl("GET /load HTTP/1.1", strlen("GET /load HTTP/1.1"), &r));
    ASSERT_EQ(0, strncmp(r.head, HELLO_RESPONSE_MARKER, sizeof(HELLO_RESPONSE_MARKER) - 1));
    ASSERT_EQ(0, strcmp(r.head + sizeof(HELLO_RESPONSE_MARKER) - 1, "load"));
    dlclose(handle);
}

UTEST(load, missing_file_fails) { ASSERT_EQ(NULL, dlopen("/nonexistent.so", RTLD_NOW)); }

UTEST_MAIN();
