# webplatform platform guide

How to develop the webplatform server and frontends on top of this
repository: the container, the core library, the check pipeline and the
reference examples.

## Overview

The platform consists of four parts:

1. **Container** - `ghcr.io/vadxx/webplatform:main`: Alpine 3.21, musl,
   Clang/LLVM toolchain, CMake + Ninja, `xxd`/`gzip`, curl. Access via
   `docker exec`, persistent data in the `/data` volume.
2. **`lib/core`** - the reusable foundation: plugin ABI, URI whitelist,
   zero-heap epoll event loop, CMake helpers and the vendored `utest.h`
   (in `third-party/`).
3. **`scripts/check.sh`** - the universal verification pipeline
   (format -> build -> tidy -> tests -> ELF audit).
4. **`examples/`** - living reference: the plugin pattern (`plugin`) and the
   HTTP+HTMX vertical slice (`server`).

## Daily workflow

```bash
# once: start the container
docker compose up -d --build

# sync your repo into the container (includes .git - check.sh needs it)
docker cp . webplatform:/data/work/<repo>

# run the full check inside the container
docker exec -w /data/work/<repo> webplatform ./scripts/check.sh

# interactive shell
docker exec -it webplatform bash
```

## Data

- `/data` - named volume: survives restarts, container recreation and
  image updates. `docker compose down -v` is the only way to lose it.
  - `/data/work` - checkouts; `/data/tmp` - scratch; `/data/logs` - logs
- `/share` <-> `~/webplatform-share` - host <-> container file exchange
- Port convention: `WEBPLATFORM_HTTP_PORT` (default 8080) - passed into the
  container by the base compose, read by `examples/server`; the server repo's
  compose publishes the same value.

## Scaffolding a server repository

```
server/
  CMakeLists.txt          # same hard flags; add_subdirectory(lib), src
  lib/
    core/                 # copied from this repo - you own the copy
    frontend/             # your HTMX frontend plugin (.so)
    backend/              # your JSON API plugin (.so)
  src/
    server.cpp            # epoll core: dlopens the plugins
  assets/                 # style.css, images
  third-party/            # vendored: htmx/htmx.min.js, utest/utest.h
  scripts/check.sh        # copied verbatim - no project paths inside
  tests/
```

Copy `lib/core/`, `third-party/` and `scripts/check.sh`; the copy model means you evolve
them freely (e.g. extend `host_api_t` with `writev`, `sendfile`).
Alternative: `FetchContent` this repository to pull updates.

Your own compose, based on the same image:

```yaml
services:
  server:
    image: ghcr.io/vadxx/webplatform:main
    container_name: server
    volumes:
      - server-data:/data
    ports:
      - "${WEBPLATFORM_HTTP_PORT:-8080}:8080"
    restart: unless-stopped
```

## Writing a plugin

```cmake
add_library(frontend SHARED frontend.c)
target_link_libraries(frontend PRIVATE core_headers core_lib)
include(plugin)
core_plugin(frontend)                    # -nostdlib, PIC, no stack protector
include(embed_asset)
core_embed_asset(frontend style ${ASSETS}/style.css)
core_embed_asset(frontend htmx ${THIRD_PARTY}/htmx/htmx.min.js)
```

```c
#include "core/plugin_api.h"

CORE_PLUGIN_API int plugin_init(const struct host_api_t *api);
CORE_PLUGIN_API int plugin_handle(const char *req, size_t len,
                                  struct plugin_response_t *out);
```

Rules enforced by the audit: no libc imports (use `host_api_t` pointers),
no `malloc`/`free` - responses go into the caller-owned
`plugin_response_t` with `iov[]` parts. See
[lib/core/README.md](../lib/core/README.md) for the full contract.

## Using the event loop

```c
static struct core_conn g_conns[64];      /* static pool, zero heap */

static int handle(struct core_conn *conn)
{
    /* conn->req_buf holds the request through \r\n\r\n;
       fill conn->resp: head = full HTTP response, iov/iov_count = parts */
    return 0;
}

int main(void)
{
    int fd = core_listen(8080);
    return core_loop_run(fd, g_conns, 64, handle);
}
```

Semantics: level-triggered epoll, non-blocking fds, partial `writev`
resumed on `EPOLLOUT`, `Connection: close`, >4 KiB headers dropped.
Keep-alive and timeouts belong to the server repo.

## HTTP helpers

Routing stays declarative - two flat tables and a one-line handler (see
[lib/core/README.md](../lib/core/README.md)):

```c
static const struct core_http_asset_route g_assets[] = {
    {"/",        "text/html",              page_gz,  sizeof(page_gz)},
    {"/htmx.js", "application/javascript", htmx_gz, sizeof(htmx_gz)},
};
static const struct core_http_plugin_route g_plugins[] = {
    {"/plugin/", &g_plugin},
};

static int handle(struct core_conn *conn)
{
    return core_http_route(conn, g_assets, CORE_ARRAY_LEN(g_assets),
                           g_plugins, CORE_ARRAY_LEN(g_plugins));
}
```

## Extending host_api

Extend `struct host_api_t` in **your copy** of `plugin_api.h`:

```c
struct host_api_t {
    int (*snprintf)(char *buf, size_t sz, const char *fmt, ...);
    void *(*memcpy)(void *dst, const void *src, size_t n);
    ssize_t (*writev)(int fd, const struct iovec *iov, int count);  /* new */
    ssize_t (*sendfile)(int out, int in, off_t *off, size_t count); /* new */
};
```

Fill the pointers in the core, pass to `plugin_init` - plugins get new
functions without relinking.

## check.sh reference

1. `clang-format -i` on all tracked C/C++ sources (vendored `utest.h`
   excluded)
2. `cmake -G Ninja` + `ninja` (clang/clang++, Debug)
3. `clang-tidy` on production sources (`*/tests/*` excluded)
4. `ctest` (tests build with ASan/UBSan; allocation counter via
   `-Wl,--wrap=malloc`)
5. Every `build/**/*.so`: `readelf -d` must show **no `NEEDED`** and
   `llvm-nm -u` must show **no `malloc|calloc|realloc|free|_Znw|_Zda`**

Adding tests: `UTEST(suite, name)` cases in `tests/test_*.c`, include the
implementation `.c` directly.

## Reference examples

- [examples/plugin](../examples/plugin/) - plugin pattern + artifact load test
- [examples/server](../examples/server/) - HTTP -> route -> plugin ->
  writev -> embedded HTMX page, e2e-checked with curl in CI

## Updating the image

```bash
docker compose pull && docker compose up -d      # docker recreates the container
```

The `/data` volume survives; `/data/work` checkouts stay in place.
