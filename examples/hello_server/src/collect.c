#include "collect.h"

#include "core/log.h"
#include "db.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    PROC_BUF_CAP = 1024,
    SQL_CAP = 96,
    RETENTION_SEC = 30 * 24 * 3600,
    RETENTION_INTERVAL_SEC = 3600,
    STAT_FIELDS_MANDATORY = 4, /* user nice system idle */
};

static const char PROC_STAT_PATH[] = "/proc/stat";
static const char PROC_MEMINFO_PATH[] = "/proc/meminfo";
static const char PROC_LOADAVG_PATH[] = "/proc/loadavg";
static const char MEM_TOTAL_KEY[] = "MemTotal:";
static const char MEM_AVAIL_KEY[] = "MemAvailable:";
static const char STAT_FMT[] = "cpu %lld %lld %lld %lld %lld %lld %lld %lld";
static const char MEM_TOTAL_FMT[] = "MemTotal: %ld";
static const char MEM_AVAIL_FMT[] = "MemAvailable: %ld";
static const char INSERT_FMT[] = "INSERT INTO metrics VALUES (%lld, %.2f, %ld, %.3f)";
static const char RETENTION_FMT[] = "DELETE FROM metrics WHERE ts < %lld";
static const char LOG_TAG[] = "collect";

/* Tick state: the baseline sample and the retention clock */
static struct collect_stat_sample g_prev;
static int g_has_prev;
static long long g_last_retention_ts = -1;

/* One warn line per failed-insert streak, not one per tick */
static int g_insert_fail_streak;

int collect_parse_proc_stat(core_str_view_t buf, struct collect_stat_sample *out) {
    if (buf.data == NULL || out == NULL || buf.len == 0) {
        return -1;
    }
    long long user = 0;
    long long nice = 0;
    long long system_ticks = 0;
    long long idle = 0;
    long long iowait = 0;
    long long irq = 0;
    long long softirq = 0;
    long long steal = 0;
    const int fields = sscanf(buf.data, STAT_FMT, &user, &nice, &system_ticks, &idle, &iowait, &irq,
                              &softirq, &steal);
    if (fields < STAT_FIELDS_MANDATORY) {
        return -1;
    }
    out->busy = user + nice + system_ticks + irq + softirq + steal;
    out->total = out->busy + idle + iowait;
    return 0;
}

int collect_parse_meminfo(core_str_view_t buf, struct collect_mem_sample *out) {
    if (buf.data == NULL || out == NULL || buf.len == 0) {
        return -1;
    }
    const char *total_line = strstr(buf.data, MEM_TOTAL_KEY);
    const char *avail_line = strstr(buf.data, MEM_AVAIL_KEY);
    if (total_line == NULL || avail_line == NULL) {
        return -1;
    }
    if (sscanf(total_line, MEM_TOTAL_FMT, &out->total_kb) != 1) {
        return -1;
    }
    if (sscanf(avail_line, MEM_AVAIL_FMT, &out->avail_kb) != 1) {
        return -1;
    }
    return 0;
}

int collect_parse_loadavg(core_str_view_t buf, double *load1) {
    if (buf.data == NULL || load1 == NULL || buf.len == 0) {
        return -1;
    }
    return sscanf(buf.data, "%lf", load1) == 1 ? 0 : -1;
}

int collect_cpu_busy_percent(const struct collect_stat_sample *prev,
                             const struct collect_stat_sample *cur, double *out) {
    if (out == NULL || cur->total <= prev->total) {
        return -1; /* no delta yet, or the counters were reset */
    }
    const long long busy_delta = cur->busy - prev->busy;
    const long long total_delta = cur->total - prev->total;
    *out = (double)busy_delta * 100.0 / (double)total_delta;
    return 0;
}

/* Reads a whole small /proc file into buf as a NUL-terminated string */
static int read_proc_file(const char *path, char *buf, size_t cap) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    const ssize_t got = read(fd, buf, cap - 1);
    close(fd);
    if (got <= 0) {
        return -1;
    }
    buf[got] = '\0';
    return 0;
}

static long long now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec;
}

/* Hourly retention trim: timestamp-based, survives restarts */
static void trim_history(struct db *db, long long now) {
    if (g_last_retention_ts < 0) {
        g_last_retention_ts = now;
        return;
    }
    if (now - g_last_retention_ts < RETENTION_INTERVAL_SEC) {
        return;
    }
    char sql[SQL_CAP];
    snprintf(sql, sizeof(sql), RETENTION_FMT, now - RETENTION_SEC);
    if (db_exec(db, sql) == 0) {
        core_log('I', LOG_TAG, "retention trimmed %ld rows", db_changes(db));
    }
    g_last_retention_ts = now;
}

/* One /proc reading: everything a tick writes into the database. */
struct proc_snapshot {
    struct collect_stat_sample stat;
    struct collect_mem_sample mem;
    double cpu_busy; /* filled by the caller from two snapshots */
    double load1;
};

/* Writes one sample row; logs the start of a failure streak */
static void write_sample(struct db *db, const struct proc_snapshot *snap, long long now) {
    const long mem_used_kb = snap->mem.total_kb - snap->mem.avail_kb;
    char sql[SQL_CAP];
    snprintf(sql, sizeof(sql), INSERT_FMT, now, snap->cpu_busy, mem_used_kb, snap->load1);
    if (db_exec(db, sql) == 0) {
        g_insert_fail_streak = 0; /* a duplicate ts just skips the row */
        return;
    }
    if (g_insert_fail_streak == 0) {
        core_log('W', LOG_TAG, "INSERT failed, metrics history paused");
    }
    ++g_insert_fail_streak;
}

/* Samples /proc into out; 0 ok, -1 when a file or a parse failed. */
static int take_snapshot(struct proc_snapshot *out) {
    char stat_buf[PROC_BUF_CAP];
    char mem_buf[PROC_BUF_CAP];
    char load_buf[64];
    if (read_proc_file(PROC_STAT_PATH, stat_buf, sizeof(stat_buf)) != 0 ||
        read_proc_file(PROC_MEMINFO_PATH, mem_buf, sizeof(mem_buf)) != 0 ||
        read_proc_file(PROC_LOADAVG_PATH, load_buf, sizeof(load_buf)) != 0) {
        return -1; /* /proc disappeared: skip the tick */
    }
    if (collect_parse_proc_stat(CORE_SV(stat_buf), &out->stat) != 0 ||
        collect_parse_meminfo(CORE_SV(mem_buf), &out->mem) != 0 ||
        collect_parse_loadavg(CORE_SV(load_buf), &out->load1) != 0) {
        return -1;
    }
    return 0;
}

void collect_on_tick(void *ctx) {
    struct collect_ctx *collect = ctx;
    if (collect == NULL) {
        return;
    }
    /* Level-triggered epoll: the timer must be drained every tick or
     * the loop spins on a readable fd. */
    if (collect->timer_fd >= 0) {
        uint64_t ticks = 0;
        (void)read(collect->timer_fd, &ticks, sizeof(ticks));
    }
    struct proc_snapshot snap = {0};
    if (take_snapshot(&snap) != 0) {
        return;
    }
    double cpu_busy = 0;
    if (!g_has_prev) {
        g_has_prev = 1; /* first tick: baseline only */
    } else if (collect_cpu_busy_percent(&g_prev, &snap.stat, &cpu_busy) != 0) {
        /* counters reset: re-baseline without writing */
    } else {
        snap.cpu_busy = cpu_busy;
        const long long now = now_sec();
        write_sample(collect->db, &snap, now);
        trim_history(collect->db, now);
    }
    g_prev = snap.stat;
}
