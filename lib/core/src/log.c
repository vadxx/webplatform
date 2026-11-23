#include "core/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char g_ring[CORE_LOG_RING_LINES][CORE_LOG_LINE_CAP];
static size_t g_head;  /* next slot to overwrite */
static size_t g_count; /* lines kept, capped at the ring size */
static int g_stderr_enabled = 1;

void core_log_stderr(int enable) { g_stderr_enabled = enable; }

void core_log(char level, const char *tag, const char *fmt, ...) {
    if (tag == NULL || fmt == NULL) {
        return;
    }
    char line[CORE_LOG_LINE_CAP];
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    size_t pos = strftime(line, sizeof(line), "%Y-%m-%dT%H:%M:%SZ", &utc);
    if (pos == 0) {
        return;
    }
    const int prefix = snprintf(line + pos, sizeof(line) - pos, " %c %s: ", level, tag);
    if (prefix <= 0 || (size_t)prefix >= sizeof(line) - pos) {
        return;
    }
    pos += (size_t)prefix;
    va_list args;
    va_start(args, fmt);
    const int body = vsnprintf(line + pos, sizeof(line) - pos, fmt, args);
    va_end(args);
    if (body < 0) {
        return;
    }
    /* a truncated tail is still a line: the stamp and tag made it out */
    pos = strlen(line);

    memcpy(g_ring[g_head], line, pos + 1);
    g_head = (g_head + 1) % CORE_LOG_RING_LINES;
    if (g_count < CORE_LOG_RING_LINES) {
        ++g_count;
    }

    if (g_stderr_enabled) {
        char with_nl[CORE_LOG_LINE_CAP + 1];
        memcpy(with_nl, line, pos);
        with_nl[pos] = '\n';
        (void)write(STDERR_FILENO, with_nl, pos + 1);
    }
}

size_t core_log_count(void) { return g_count; }

const char *core_log_line(size_t index) {
    if (index >= g_count) {
        return NULL;
    }
    const size_t oldest = (g_head + CORE_LOG_RING_LINES - g_count) % CORE_LOG_RING_LINES;
    return g_ring[(oldest + index) % CORE_LOG_RING_LINES];
}
