# webplatform

Dev container and foundation for the webplatform project: an Alpine + musl +
Clang toolchain for building the future HTTP server and its `-nostdlib`
plugins, plus a reusable core library (`lib/core`), a universal check script
and reference examples.

Access is `docker exec`. This project is based on my [devbox](https://github.com/vadxx/devbox).

## Quick start

Docker is assumed to be installed.

```bash
docker compose up -d --build          # build image, start container
docker exec -it webplatform bash      # workdir is /data/work
docker logs webplatform               # one-screen hint on every start
```

## Data

- `/data` - named volume, survives container restarts, recreation and image
  updates. Only `docker compose down -v` deletes it.
  - `/data/work` - checkouts and scratch builds
  - `/data/logs` - future server logs
- `/share` <-> `~/webplatform-share` on the host - simple file exchange.

## HTTP port convention

The server listens on `8080` inside the container by default, and this
repo's compose publishes the same port to the host, bound to `127.0.0.1`
only (the examples ship demo passwords, they are not for the LAN). The
`WEBPLATFORM_HTTP_PORT` environment variable overrides it end to end:
compose passes it into the container and publishes it, the entrypoint
hint prints it and the example server (`examples/server`) binds it.

## Editor

VS Code: attach with **Dev Containers** (`Attach to Running Container`).
There is no ssh layer; if you ever need one, add it as a feature to the
Containerfile.

## Reusing this base in the server repository

- Copy `lib/core/`, `third-party/` and `scripts/check.sh` into the server repo - it owns its
  copies and evolves them freely (e.g. extends `host_api_t`).
- See [docs/platform.md](docs/platform.md) for the full development guide,
  [lib/core/README.md](lib/core/README.md) for the library contract.

## Examples

- [examples/plugin](examples/hello_plugin/) - the plugin pattern: `-nostdlib` `.so`
  loaded via `dlopen` with a `host_api_t`.
- [examples/server](examples/hello_server/) - a vertical slice: HTTP request ->
  route -> plugin -> `writev` -> embedded HTMX page.

```bash
# inside the container, repo synced to /data/work/webplatform
./scripts/check.sh
```

## Third-party

Vendored files (utest.h, htmx.min.js) live in [third-party/](third-party/) -
see [third-party/README.md](third-party/README.md) for the index with
sources, versions and licenses.
