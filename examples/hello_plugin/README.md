# examples/plugin

The plugin pattern in miniature: a `-nostdlib` shared library served to a
core executable through `dlopen` + `host_api_t`.

## What it proves

- `hello_plugin.so` has **zero `NEEDED` entries and zero undefined symbols**
  (checked by `scripts/check.sh` with `readelf`/`llvm-nm`) - all libc
  functionality comes in via `host_api_t` pointers.
- A gzip-compressed asset is embedded at build time
  (`core_embed_asset`, `gzip -9 | xxd -i`).
- The response is assembled zero-copy: `iov[0]` head + `iov[1]` static
  asset, no allocations (`--wrap=malloc` test).
- `plugin_host` loads the built `.so` at runtime, passes a real `host_api_t`
  and prints the response.

## Layout

- `lib/hello_plugin.c` - the plugin (`plugin_init`/`plugin_handle`, URI
  whitelist via `core_uri_segment_valid`)
- `src/main.cpp` - `plugin_host`, the loader
- `tests/test_plugin.c` - unit tests (include the `.c` directly)
- `tests/test_load.c` - artifact test: `dlopen` the built `.so`, `dlsym`
  the ABI, handle a request

## Run

```bash
./scripts/check.sh                                  # from the repo root (in the container)
build/examples/plugin/src/plugin_host                   # prints "plugin ok: demo" + gzip asset
build/examples/plugin/src/plugin_host /nonexistent.so   # error path, exit 1
```
