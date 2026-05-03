/*
 * omniagent/src/memory_limiter.c
 *
 * Memory Limiter Processor
 *
 * Reads the agent's own RSS from /proc/self/status and decides whether
 * to pass or drop an incoming TelemetryRecord.
 *
 * Behaviour:
 *   • Below MEMORY_LIMIT_MB (64 MB): record passes through unchanged.
 *   • At or above the limit: record is dropped (pool_free'd) and a drop
 *     counter is incremented.  Dropped record count is logged every 60 s.
 *
 * Implementation notes
 * ────────────────────
 * Reading /proc/self/status on every record would be wasteful.  Instead we
 * cache the RSS value and refresh it every CHECK_INTERVAL_RECORDS records.
 * This amortises the filesystem read cost while still reacting within a
 * ~1000-record window.
 *
 * The function is called synchronously from the batch_processor thread so
 * it does not need its own mutex.
 */

#include "omniagent.h"

#define CHECK_INTERVAL_RECORDS  128    /* re-read /proc/self/status every N records */

/* ── Module state ────────────────────────────────────────────────────────── */

static uint64_t s_rss_kb          = 0;
static uint64_t s_check_counter   = 0;
static uint64_t s_drop_total      = 0;
static uint64_t s_last_log_drops  = 0;
static time_t   s_last_log_time   = 0;

/* ── Read RSS from /proc/self/status ─────────────────────────────────────── */

static uint64_t read_self_rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;

    char line[128];
    uint64_t rss = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line, "VmRSS: %" SCNu64, &rss);
            break;
        }
    }
    fclose(f);
    return rss;
}

/* ── memory_limiter_check ────────────────────────────────────────────────── */

/*
 * Returns 1 if the record should be PASSED to the batch.
 * Returns 0 if the record should be DROPPED (caller must pool_free it).
 */
int memory_limiter_check(TelemetryRecord *record)
{
    /* Refresh RSS periodically */
    if (s_check_counter % CHECK_INTERVAL_RECORDS == 0) {
        s_rss_kb = read_self_rss_kb();
        LOG_DEBUG("memory_limiter: RSS=%" PRIu64 " KB (limit=%d MB)",
                  s_rss_kb, MEMORY_LIMIT_MB);
    }
    s_check_counter++;

    uint64_t limit_kb = (uint64_t)MEMORY_LIMIT_MB * 1024ULL;

    if (s_rss_kb == 0 || s_rss_kb < limit_kb) {
        return 1;  /* PASS */
    }

    /* We're over the limit — drop */
    s_drop_total++;
    pool_free(&g_pool, record);

    /* Log a warning at most once per 60 seconds to avoid log spam */
    time_t now = time(NULL);
    if (now - s_last_log_time >= 60) {
        uint64_t new_drops = s_drop_total - s_last_log_drops;
        LOG_WARN("memory_limiter: RSS=%" PRIu64 " KB exceeds %d MB limit — "
                 "dropped %" PRIu64 " records in last 60s",
                 s_rss_kb, MEMORY_LIMIT_MB, new_drops);
        s_last_log_drops = s_drop_total;
        s_last_log_time  = now;
    }

    return 0;  /* DROP */
}

/* ── memory_limiter_stats ────────────────────────────────────────────────── */

uint64_t memory_limiter_drop_count(void)
{
    return s_drop_total;
}

uint64_t memory_limiter_rss_kb(void)
{
    return s_rss_kb;
}
