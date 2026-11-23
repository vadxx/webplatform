#pragma once
#include <stddef.h>

#include "core/http.h"

/* /proc readers and the periodic collector tick. The parsers are pure
 * functions over a buffer view so tests never depend on the real
 * /proc. */

struct db;

/* Tick context: the poll source callback gets one pointer, so the db
 * and the timer fd (drained on every tick) travel together. */
struct collect_ctx {
    struct db *db;
    int timer_fd;
};

/* Two adjacent /proc/stat samples (jiffies). */
struct collect_stat_sample {
    long long busy;  /* user+nice+system+irq+softirq+steal */
    long long total; /* busy + idle + iowait */
};

/* /proc/meminfo pair (KiB). */
struct collect_mem_sample {
    long total_kb;
    long avail_kb;
};

/* First line of /proc/stat: "cpu user nice system idle iowait irq
 * softirq steal ...". 0 ok, -1 bad input. */
int collect_parse_proc_stat(core_str_view_t buf, struct collect_stat_sample *out);

/* /proc/meminfo: only MemTotal and MemAvailable matter. 0 ok, -1 bad. */
int collect_parse_meminfo(core_str_view_t buf, struct collect_mem_sample *out);

/* /proc/loadavg: first field only. 0 ok, -1 bad input. */
int collect_parse_loadavg(core_str_view_t buf, double *load1);

/* Busy CPU percent between two /proc/stat samples. 0 computed,
 * -1 no delta yet (first sample) or the counters were reset. */
int collect_cpu_busy_percent(const struct collect_stat_sample *prev,
                             const struct collect_stat_sample *cur, double *out);

/* One tick: drain the timer, sample /proc, write one metrics row via
 * ctx->db. The first tick only remembers the baseline and writes
 * nothing. ctx is struct collect_ctx *. */
void collect_on_tick(void *ctx);
