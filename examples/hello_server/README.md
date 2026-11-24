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
- `/api/metrics?range=N` - JSON for the uPlot chart: cpu/mem/load history
  collected every 5 s by a timerfd poll source into SQLite (static
  arena, `/data/metrics.db`, `WEBPLATFORM_DB` overrides), downsampled to
  720 points, default range 3600 s, capped at 86400 s
- The page draws a live uPlot chart from that JSON (`/uplot.iife.min.js`
  and `/uplot.min.css` are embedded gzip assets), refreshing every 5 s
- Response composition in the core: HTTP headers + plugin body in a
  single `writev`

## Run (from the host)

Prerequisite: the dev container is up once (`docker compose up -d` from
the repo root - it publishes `WEBPLATFORM_HTTP_PORT`, 8080 by default)
and the example is built inside it (`./scripts/check.sh`, see the repo
README). While the server below runs, the page opens in a host browser
at `http://localhost:8080`. Logs go to `/data/logs/server.log` on the
persistent volume (`exec -d` output never reaches `docker logs`).

```bash
docker exec -d webplatform sh -c 'cd /data/work/webplatform && mkdir -p /data/logs && build/examples/hello_server/src/hello_server >>/data/logs/server.log 2>&1'
docker exec webplatform curl -sf localhost:8080/plugin/hello        # -> plugin ok: hello
docker exec webplatform curl -sf --compressed localhost:8080/     # -> HTML page (gzip decoded)
docker exec webplatform sh -c 'curl -s localhost:8080/nope | head -1' # -> HTTP/1.1 404 Not Found
docker exec webplatform curl -s 'localhost:8080/api/metrics?range=60' # -> {"ts":[...],"cpu":[...],...}
docker exec webplatform pkill hello_server                        # stop
```
