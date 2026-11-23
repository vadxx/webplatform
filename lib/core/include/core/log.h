#pragma once
#include <stddef.h>

/* Level-filtered one-line logger: a single write() to stderr (docker
 * logs collects it, rotation is the container's job) plus a static
 * ring buffer for a dashboard endpoint. Zero heap: lines are formatted
 * on the stack and copied into the ring. */

enum {
    CORE_LOG_RING_LINES = 256,
    CORE_LOG_LINE_CAP = 128,
};

/* level: one character, 'I' / 'W' / 'E' by convention; tag: component
 * name ("server", "collect", "metrics"). Lines look like
 * "2026-11-23T14:09:21Z W collect: INSERT failed". */
void core_log(char level, const char *tag, const char *fmt, ...);

/* Ring iteration, oldest first; NULL past the count. */
size_t core_log_count(void);
const char *core_log_line(size_t index);

/* Tests silence stderr while keeping the ring. */
void core_log_stderr(int enable);
