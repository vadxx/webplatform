# lib/core

Reusable foundation of the webplatform: the plugin ABI, URI whitelist
validation, a zero-heap epoll event loop, CMake helpers and a vendored
`utest.h` (vendored in `third-party/utest`). The server repository copies
this directory and owns its copy.

## Targets

| Target | Contents | Links into |
|---|---|---|
| `core_headers` | INTERFACE, `include/` | everything |
| `core_lib` | `src/uri.c` - pure code | plugins (`.so`, `-nostdlib`) and cores |
| `core_srv` | `src/event_loop.c`, `src/http.c` - sockets/epoll/writev, HTTP composition, dlopen | **executables only**, never `.so` |

`core_srv` uses libc (`epoll_*`, `read`, `writev`) - linking it into a
`-nostdlib` plugin would break the ELF audit. `core_lib` is kept pure.

## Plugin ABI (`core/plugin_api.h`)

- `struct host_api_t` - functions the host passes to the plugin by pointer
  (`snprintf`, `memcpy`, ...). The server repo extends this struct in its own
  copy (add `writev`, `sendfile`, ...).
- `struct plugin_response_t` - the plugin writes the response into a
  caller-owned struct (per-connection in a server; concurrency-safe).
  `head[4096]` + `iov[2]` (`iov[0]` dynamic head, `iov[1]` read-only static
  asset) + lengths.
- `plugin_init(const struct host_api_t *)`, `plugin_handle(request, len,
  response*)` - the `dlsym` contract; `0` ok, `-1` error. Export with
  `CORE_PLUGIN_API` (fights the global `-fvisibility=hidden`).

## URI validation (`core/uri.h`)

`core_uri_segment_valid(segment, len)` - whitelist `[a-z0-9._-]`, no `..`
substring, length 1..128.

## Event loop (`core/event_loop.h`)

```c
static struct core_conn g_conns[64];          /* pool owned by the caller */

int listen_fd = core_listen(8080);
core_loop_run(listen_fd, g_conns, 64, handler);  /* blocks forever */
```

The handler fires when a full request (`\r\n\r\n`) sits in
`conn->req_buf`; it fills `conn->resp` (head = complete HTTP response,
iov/iov_count = parts for `writev`). Level-triggered epoll, non-blocking
fds, partial writes are resumed on `EPOLLOUT`, `Connection: close`
semantics. Buffer overflow (`> 4 KiB` headers) drops the connection.
Keep-alive/timeouts are the server repository's job.

## CMake helpers (`cmake/`)

```cmake
include(plugin)      # core_plugin(<target>): PIC, -fno-stack-protector, -nostdlib
include(embed_asset) # core_embed_asset(<target> <name> <file>)
```

`core_embed_asset` gzips the file (`gzip -9`) and embeds it via `xxd -i` as
`<target>_<name>_gz[]` + `<target>_<name>_gz_len` in
`<target>_<name>_assets.h`; it wires dependencies and include paths itself.

## HTTP helpers and plugin loading (`core/http.h`)

Server-side building blocks (executables only, part of `core_srv`):

- `core_http_request_path(req, len, &path_len)` - the request path from the
  first line
- `core_http_head(buf, cap, status, type, content_len, gzip)` - HTTP header
  block primitive
- `core_http_serve_asset(conn, type, gz, gz_len)` - response from an
  embedded gzip asset (`Content-Encoding: gzip`)
- `core_http_not_found(conn)` - plain 404
- `core_http_wrap_body(resp)` - wraps a plugin response (`head` +
  `iov[1..`) with HTTP headers, preserving the tail iovs
- `core_http_match_exact(path, len, route)` /
  `core_http_match_prefix(path, len, prefix, &seg, &seg_len)` - routing
  primitives
- `core_http_serve_plugin(conn, &plugin, seg, seg_len)` - the fragment
  pattern: validate, compose `GET /<seg> HTTP/1.1`, call, wrap head-only
- `struct core_http_asset_route` / `struct core_http_plugin_route` +
  `core_http_route(conn, assets, n, plugins, m)` - the full router over two
  flat tables (`CORE_ARRAY_LEN` for the counts)
- `core_content_type_html/css/js/json/plain` - named content-type
  constants for static route-table initializers
- `core_host_api_default(&api)` - host API from the executable's libc
- `struct core_plugin` + `core_plugin_open(&p, path, &api)` -
  `dlopen` + `dlsym` + `plugin_init` in one call

## Tests

`tests/` - `test_uri.c`,
`test_event_loop.c`. Tests include the implementation `.c` directly, run
under ASan/UBSan with `-Wl,--wrap=malloc` (allocation counter; a dedicated
test asserts the module does not allocate). Add new cases with
`UTEST(suite, name)`.
