# examples/server

A vertical slice of the future server architecture: HTTP request ->
route -> `-nostdlib` plugin -> `writev` -> HTMX page. Two flat route tables
(assets + plugins) and a one-line handler on top of `lib/core`; the
router itself lives in `core/http.h`.

## What it demonstrates

- `core_srv` event loop: static connection pool (`g_conns[64]`), zero heap
- Embedded gzip assets served with `Content-Encoding: gzip`:
  `/` (the page) and `/htmx.min.js` (vendored htmx)
- `/plugin/<segment>` - the segment is validated with
  `core_uri_segment_valid` (traversal like `/plugin/../etc` is rejected),
  a synthetic request is composed and routed into `hello_plugin.so`;
  the fragment comes back as an HTMX swap target
- Response composition in the core: HTTP headers + plugin body in a
  single `writev`

## Run (inside the container)

```bash
build/examples/server/src/hello_server &
curl -sf localhost:8080/plugin/hello        # -> plugin ok: hello
curl -sf --compressed localhost:8080/     # -> HTML page (gzip decoded)
curl -s localhost:8080/nope | head -1     # -> HTTP/1.1 404 Not Found
```

The base repo's compose intentionally publishes no ports - publishing
`WEBPLATFORM_HTTP_PORT` is the server repository's job. To see the page
in a browser, run a temporary container with `-p 8080:8080` from the
server repo image, or check with curl as above.
