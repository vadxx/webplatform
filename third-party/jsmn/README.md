# jsmn

Minimalistic JSON tokenizer: header-only, zero allocations, the token
pool is owned by the caller.

- Source: https://github.com/zserge/jsmn
- Vendored: release v1.1.0 (2019-06-23), `jsmn.h` only
- License: MIT - the full text is in the file header
- Used by: `lib/core/tests` (include path `third-party/jsmn`). Future
  consumers include it with `-DJSMN_STATIC` (each TU gets its own copy,
  no libc symbols - safe for `-nostdlib` plugins).

## Security note (CVE-2026-95117)

The CVE (denial of service via `dump()` in `example/jsondump.c`) lives
in the upstream example code, which this repository does not vendor;
`jsmn_parse()` itself stays in bounds on the published PoC. Two rules
for consumers keep it that way:

- Always bound token-tree walks by the token count returned from
  `jsmn_parse()`. On malformed input in non-strict mode a token's
  `size` may overstate the number of children actually in the pool.
- Prefer `-DJSMN_STRICT` when parsing untrusted input.

Update: download a fresh `jsmn.h` from the repo above, verify the
license header, replace this file.
