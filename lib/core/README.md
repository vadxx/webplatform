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
| `core_srv` | `src/event_loop.c`, `src/http.c`, `src/log.c`, `src/session.c` - sockets/epoll/writev/timers, HTTP composition and pooled bodies, logging, sessions, dlopen | **executables only**, never `.so` |

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
static struct core_conn g_conns[64]; /* pool owned by the caller */
const struct core_conn_pool pool = {.conns = g_conns, .count = CORE_ARRAY_LEN(g_conns)};

int listen_fd = core_listen(8080);
core_loop_run(listen_fd, &pool, handler, NULL); /* blocks forever */
```

The handler fires when a complete request head (`\r\n\r\n`) sits in
`conn->req_buf`; it fills `conn->resp` (head = complete HTTP response,
iov/iov_count = parts for `writev`). Level-triggered epoll, non-blocking
fds, partial writes are resumed on `EPOLLOUT`, `Connection: close`
semantics. Buffer overflow (`> 4 KiB` headers) drops the connection.
The last argument adds extra poll sources (`struct core_poll_set`) -
readable fds like `core_timer_fd(sec)` for periodic jobs; see
`examples/hello_server` for the collector pattern.
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

- `core_http_request_path(req, &path)` / `core_http_request_method(req,
  &method)` - zero-copy views into the request line
- `core_http_content_length(req, &len)` - Content-Length, case
  insensitive name, digits-only value
- `core_http_cookie(req, name, &val)` / `core_http_form_field(body,
  name, &val)` - zero-copy cookie and form-field lookup (no URL
  decoding)
- `core_http_head(buf, cap, &spec)` - the header block primitive
  (`struct core_http_head_spec`: status, type, len, gzip flag)
- `core_http_serve_asset(conn, type, gz, gz_len)` / `..._asset_route()`
  - responses from embedded gzip assets (`Content-Encoding: gzip`);
  the route variant walks a flat table
  (`struct core_http_routes`: assets + plugins)
- `core_http_serve_body(conn, status, type, body)` - a pooled body slot
  (`core_http_body_checkout`, `CORE_BODY_SLOTS x 32 KiB`) as the
  response; `core_http_append(buf, cap, &pos, fmt, ...)` composes into
  a slot bounded
- `core_http_serve_status(conn, status, body)` - short plain-text
  statuses; `core_http_serve_redirect(conn, location)` - 302;
  `core_http_not_found(conn)` - 404
- `core_http_set_cookie(head, cap, &spec)` - inserts a Set-Cookie line
  into a composed head. It edits the buffer via `strlen` and knows
  nothing of the resp bookkeeping: after the call the caller must
  recompute `conn->resp.head_len` and `resp.iov[0].iov_len` (see
  `examples/hello_app/src/login.c`)
- `core_http_wrap_body(resp)` - wraps a plugin response (`head` +
  `iov[1..)`) with HTTP headers, preserving the tail iovs
- `core_http_match_exact(path, route)` /
  `core_http_match_prefix(path, prefix, &seg)` - routing primitives
- `core_http_serve_plugin(conn, &plugin, seg)` - the fragment pattern:
  validate, compose `GET /<seg> HTTP/1.1`, call, wrap head-only
- `core_content_type_html/css/js/json/plain` and
  `core_http_status_ok/redirect/unauthorized/not_found/
  method_not_allowed/unavailable` - named constants for status lines
  and content types (single source with the core's own responses)
- `core_http_port_env(default)` - reads `WEBPLATFORM_HTTP_PORT`
- `core_host_api_default(&api)` - host API from the executable's libc
- `struct core_plugin` + `core_plugin_open(&p, path, &api)` -
  `dlopen` + `dlsym` + `plugin_init` in one call

## Tests

`tests/` - `test_uri.c`,
`test_event_loop.c`, `test_log.c`, `test_session.c`. Tests include the implementation `.c` directly, run
under ASan/UBSan with `-Wl,--wrap=malloc` (allocation counter; a dedicated
test asserts the module does not allocate). Add new cases with
`UTEST(suite, name)`.
