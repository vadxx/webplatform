#pragma once

/* Single source of the demo plugin's response contract and defaults:
 * the response format, its marker prefix and the default artifact path
 * used by manual runs (the build wires the absolute path for CI). */
#define HELLO_RESPONSE_FMT "plugin ok: %.*s"
#define HELLO_RESPONSE_MARKER "plugin ok: "
#define HELLO_DEFAULT_PATH "../lib/libhello_plugin.so"
