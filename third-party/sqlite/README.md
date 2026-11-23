# sqlite

The SQLite database engine, vendored as its official single-file
distribution (sqlite3.c + sqlite3.h; SQLite calls this "the
amalgamation"). Compiled into the server executable behind the internal
db.h seam; never edited, only replaced wholesale.

- Source: https://sqlite.org/amalgamation.html
- Vendored: release 3.54.1 (2026), from
  https://sqlite.org/2026/sqlite-amalgamation-3540100.zip
- License: Public Domain - the notice is in the file headers
- Used by: `examples/server` (CMake target `sqlite3`, linked into the
  `hello_server` executable)

## Compile-time configuration (definitions only, never edits)

    SQLITE_THREADSAFE=0            single-threaded server loop
    SQLITE_DEFAULT_MEMSTATUS=0     no memory statistics bookkeeping
    SQLITE_OMIT_LOAD_EXTENSION     no runtime extension loading
    SQLITE_OMIT_DEPRECATED         no deprecated APIs
    SQLITE_DQS=0                   double-quoted strings are not identifiers
    SQLITE_ENABLE_MEMSYS5          enables SQLITE_CONFIG_HEAP (static arena)

Runtime configuration lives in `examples/server/src/db.c`:
SQLITE_CONFIG_HEAP over a static arena, cache_size=24, synchronous=NORMAL
(SQLITE_THREADSAFE=0 already compiles mutexes out; no
SQLITE_CONFIG_SINGLETHREAD call - that case is compiled out and would
return SQLITE_ERROR). Under memsys5 each allocation rounds up to a
power of two, so a ~4 KiB page costs an 8 KiB chunk plus one 64 KiB
lookaside block: cache_size=24 keeps the whole working set inside the
512 KiB arena (a larger cache starves it - SQLITE_NOMEM).

Update: download the current amalgamation zip from sqlite.org, replace
both files wholesale, record the new version and sha256 here.

sha256 (3.54.1):
- sqlite3.c: 0c657666d819f261cc4007526ad57e1669de9949df7a8ea847aa284a2bf0ee52
- sqlite3.h: d6c84557261893439b341d2e6c35187ae3ea24fbfd843ad94616f72d89242b8c
