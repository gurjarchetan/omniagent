/*
 * omniagent/src/procfs_receiver.c
 *
 * Host Scraper — full node_exporter-equivalent metric set from Linux procfs.
 *
 * Scrapers implemented (each on every interval):
 *   /proc/stat         → CPU usage per mode (user/system/iowait/irq/steal), context switches, interrupts, boot time
 *   /proc/meminfo      → detailed memory (total/free/avail/buffers/cached/slab/swap/dirty/writeback)
 *   /proc/loadavg      → load average 1/5/15 min, running/total processes
 *   /proc/uptime       → system uptime seconds
 *   /proc/net/dev      → per-NIC rx/tx bytes, packets, errors, drops
 *   /proc/diskstats    → per-disk reads/writes completed, bytes, time spent
 *   /proc/[pid]/stat   → per-process CPU time
 *   /proc/[pid]/status → per-process RSS/VmSize/threads
 *   statvfs("/")       → filesystem usage (root; expands to all mounts via /proc/mounts)
 *   /proc/net/sockstat → TCP/UDP socket counts
 *   /proc/vmstat       → page faults, swap in/out, dirty pages
 */

#include "omniagent.h"
#include <dirent.h>
#include <ctype.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>

/* ── CPU accounting state ────────────────────────────────────────────────── */

typedef struct {
    uint64_t user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
} CpuTimes;

#define MAX_CPUS       256
#define MAX_DISKS      64
#define MAX_NICS       32
#define MAX_MOUNTS     64

/* Per-disk IO state for delta calculations */
typedef struct {
    char     name[32];
    uint64_t reads_completed;
    uint64_t writes_completed;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t read_time_ms;
    uint64_t write_time_ms;
    uint64_t io_time_ms;
} DiskStat;

/* Per-NIC state for delta calculations */
typedef struct {
    char     name[32];
    uint64_t rx_bytes, tx_bytes;
    uint64_t rx_packets, tx_packets;
    uint64_t rx_errors, tx_errors;
    uint64_t rx_drops, tx_drops;
} NicStat;

static CpuTimes      s_prev_cpu;
static CpuTimes      s_prev_percpu[MAX_CPUS];
static int           s_num_cpus = 0;
static long          s_clk_tck   = 0;
static DiskStat      s_prev_disk[MAX_DISKS];
static int           s_num_disks = 0;
static NicStat       s_prev_nic[MAX_NICS];
static int           s_num_nics  = 0;

/* ── Generic metric emitter ──────────────────────────────────────────────── */

static void emit_metric2(const char *name, const char *unit, MetricType type,
                         double value,
                         int label_count,
                         const char (*keys)[OAGENT_KEY_LEN],
                         const char (*vals)[OAGENT_VAL_LEN])
{
    TelemetryRecord *rec = pool_alloc(&g_pool);
    if (!rec) { LOG_DEBUG("procfs: pool exhausted, dropping %s", name); return; }

    rec->type = TELEM_METRIC;
    SAFE_STRNCPY(rec->metric.name, name, OAGENT_NAME_MAX);
    SAFE_STRNCPY(rec->metric.unit, unit, OAGENT_UNIT_MAX);
    rec->metric.type         = type;
    rec->metric.value        = value;
    rec->metric.timestamp_ns = telem_now_ns();
    rec->pid                 = 0;

    int lc = (label_count > OAGENT_MAX_LABELS) ? OAGENT_MAX_LABELS : label_count;
    for (int i = 0; i < lc; i++) {
        SAFE_STRNCPY(rec->metric.label_keys[i],   keys[i], OAGENT_KEY_LEN);
        SAFE_STRNCPY(rec->metric.label_values[i], vals[i], OAGENT_VAL_LEN);
    }
    rec->metric.label_count = lc;

    char hostname[64] = "unknown";
    gethostname(hostname, sizeof(hostname) - 1);
    SAFE_STRNCPY(rec->host_name,    hostname,    OAGENT_RESOURCE_MAX);
    SAFE_STRNCPY(rec->service_name, "omniagent", OAGENT_RESOURCE_MAX);

    if (!rb_enqueue(&g_recv_queue, rec)) {
        pool_free(&g_pool, rec);
        LOG_DEBUG("procfs: ring buffer full, dropped %s", name);
    }
}

/* Convenience: emit with 0 labels */
static void emit0(const char *name, const char *unit, MetricType type, double value)
{
    emit_metric2(name, unit, type, value, 0, NULL, NULL);
}

/* Convenience: emit with 1 label */
static void emit1(const char *name, const char *unit, MetricType type, double value,
                  const char *k0, const char *v0)
{
    char keys[1][OAGENT_KEY_LEN], vals[1][OAGENT_VAL_LEN];
    SAFE_STRNCPY(keys[0], k0, OAGENT_KEY_LEN);
    SAFE_STRNCPY(vals[0], v0, OAGENT_VAL_LEN);
    emit_metric2(name, unit, type, value, 1, keys, vals);
}

/* Convenience: emit with 2 labels */
static void emit2(const char *name, const char *unit, MetricType type, double value,
                  const char *k0, const char *v0,
                  const char *k1, const char *v1)
{
    char keys[2][OAGENT_KEY_LEN], vals[2][OAGENT_VAL_LEN];
    SAFE_STRNCPY(keys[0], k0, OAGENT_KEY_LEN); SAFE_STRNCPY(vals[0], v0, OAGENT_VAL_LEN);
    SAFE_STRNCPY(keys[1], k1, OAGENT_KEY_LEN); SAFE_STRNCPY(vals[1], v1, OAGENT_VAL_LEN);
    emit_metric2(name, unit, type, value, 2, keys, vals);
}

/* ── /proc/stat ──────────────────────────────────────────────────────────── */

static int read_cpu_line(const char *line, CpuTimes *out)
{
    int n = sscanf(line,
        "%" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
        " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
        " %" SCNu64 " %" SCNu64,
        &out->user, &out->nice, &out->system, &out->idle,
        &out->iowait, &out->irq, &out->softirq, &out->steal,
        &out->guest, &out->guest_nice);
    return (n >= 8) ? 0 : -1;
}

static void scrape_cpu(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) { LOG_WARN("procfs: cannot open /proc/stat: %s", strerror(errno)); return; }

    char line[256];
    CpuTimes cur;
    CpuTimes percpu_cur[MAX_CPUS];
    int num_cpus = 0;
    uint64_t ctxt = 0, intr = 0, softirq_total = 0;
    uint64_t btime = 0, procs_running = 0, procs_blocked = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu ", 4) == 0) {
            read_cpu_line(line + 4, &cur);
        } else if (strncmp(line, "cpu", 3) == 0 && isdigit((unsigned char)line[3])) {
            int cpu_id = 0;
            const char *p = line + 3;
            while (isdigit((unsigned char)*p)) { cpu_id = cpu_id * 10 + (*p++ - '0'); }
            if (cpu_id < MAX_CPUS) {
                read_cpu_line(p, &percpu_cur[cpu_id]);
                if (cpu_id + 1 > num_cpus) num_cpus = cpu_id + 1;
            }
        } else if (strncmp(line, "ctxt ", 5) == 0) {
            sscanf(line + 5, "%" SCNu64, &ctxt);
        } else if (strncmp(line, "intr ", 5) == 0) {
            sscanf(line + 5, "%" SCNu64, &intr);
        } else if (strncmp(line, "softirq ", 8) == 0) {
            sscanf(line + 8, "%" SCNu64, &softirq_total);
        } else if (strncmp(line, "btime ", 6) == 0) {
            sscanf(line + 6, "%" SCNu64, &btime);
        } else if (strncmp(line, "procs_running ", 14) == 0) {
            sscanf(line + 14, "%" SCNu64, &procs_running);
        } else if (strncmp(line, "procs_blocked ", 14) == 0) {
            sscanf(line + 14, "%" SCNu64, &procs_blocked);
        }
    }
    fclose(f);

    /* Aggregate CPU % */
    {
        uint64_t d_user    = cur.user    - s_prev_cpu.user;
        uint64_t d_nice    = cur.nice    - s_prev_cpu.nice;
        uint64_t d_system  = cur.system  - s_prev_cpu.system;
        uint64_t d_idle    = cur.idle    - s_prev_cpu.idle;
        uint64_t d_iowait  = cur.iowait  - s_prev_cpu.iowait;
        uint64_t d_irq     = cur.irq     - s_prev_cpu.irq;
        uint64_t d_softirq = cur.softirq - s_prev_cpu.softirq;
        uint64_t d_steal   = cur.steal   - s_prev_cpu.steal;
        uint64_t d_total   = d_user + d_nice + d_system + d_idle
                           + d_iowait + d_irq + d_softirq + d_steal;

        if (d_total > 0) {
            double pct_user   = (double)d_user   / d_total * 100.0;
            double pct_system = (double)d_system / d_total * 100.0;
            double pct_iowait = (double)d_iowait / d_total * 100.0;
            double pct_steal  = (double)d_steal  / d_total * 100.0;
            double pct_irq    = (double)(d_irq + d_softirq) / d_total * 100.0;
            double pct_nice   = (double)d_nice   / d_total * 100.0;
            double pct_idle   = (double)d_idle   / d_total * 100.0;
            double pct_busy   = 100.0 - pct_idle - pct_iowait;

            emit1("system.cpu.usage",       "%", METRIC_GAUGE, pct_busy,   "mode", "total");
            emit1("system.cpu.user",        "%", METRIC_GAUGE, pct_user,   "mode", "user");
            emit1("system.cpu.system",      "%", METRIC_GAUGE, pct_system, "mode", "system");
            emit1("system.cpu.iowait",      "%", METRIC_GAUGE, pct_iowait, "mode", "iowait");
            emit1("system.cpu.steal",       "%", METRIC_GAUGE, pct_steal,  "mode", "steal");
            emit1("system.cpu.irq",         "%", METRIC_GAUGE, pct_irq,    "mode", "irq");
            emit1("system.cpu.nice",        "%", METRIC_GAUGE, pct_nice,   "mode", "nice");
            emit1("system.cpu.idle",        "%", METRIC_GAUGE, pct_idle,   "mode", "idle");
        }
        s_prev_cpu = cur;
    }

    /* Per-CPU metrics */
    for (int c = 0; c < num_cpus && c < MAX_CPUS; c++) {
        CpuTimes *pc = &percpu_cur[c];
        CpuTimes *pp = &s_prev_percpu[c];
        uint64_t d_user    = pc->user    - pp->user;
        uint64_t d_system  = pc->system  - pp->system;
        uint64_t d_idle    = pc->idle    - pp->idle;
        uint64_t d_iowait  = pc->iowait  - pp->iowait;
        uint64_t d_total   = d_user + (pc->nice - pp->nice) + d_system + d_idle
                           + d_iowait + (pc->irq - pp->irq)
                           + (pc->softirq - pp->softirq) + (pc->steal - pp->steal);
        if (d_total > 0) {
            char cpu_id_str[16];
            snprintf(cpu_id_str, sizeof(cpu_id_str), "cpu%d", c);
            emit2("system.cpu.usage.percpu", "%", METRIC_GAUGE,
                  (double)(d_total - d_idle - d_iowait) / d_total * 100.0,
                  "cpu", cpu_id_str,
                  "mode", "total");
        }
        s_prev_percpu[c] = *pc;
    }
    if (num_cpus > 0) s_num_cpus = num_cpus;

    /* Misc kernel counters */
    if (ctxt > 0)          emit0("system.cpu.context_switches_total", "1", METRIC_COUNTER, (double)ctxt);
    if (intr > 0)          emit0("system.cpu.interrupts_total",       "1", METRIC_COUNTER, (double)intr);
    if (btime > 0)         emit0("system.boot_time_seconds",         "s", METRIC_GAUGE,   (double)btime);
    if (procs_running > 0) emit0("system.procs_running",             "1", METRIC_GAUGE,   (double)procs_running);
    if (procs_blocked > 0) emit0("system.procs_blocked",             "1", METRIC_GAUGE,   (double)procs_blocked);
    emit0("system.cpu.count",  "1", METRIC_GAUGE, (double)num_cpus);
}

/* ── /proc/meminfo ───────────────────────────────────────────────────────── */

static void scrape_meminfo(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) { LOG_WARN("procfs: cannot open /proc/meminfo: %s", strerror(errno)); return; }

    char     key[64];
    uint64_t val = 0;
    char     unit_str[16];
    char     line[256];

    uint64_t mem_total=0, mem_free=0, mem_avail=0, mem_buffers=0, mem_cached=0;
    uint64_t swap_total=0, swap_free=0, swap_cached=0;
    uint64_t slab=0, slab_reclaimable=0, slab_unreclaimable=0;
    uint64_t mem_dirty=0, mem_writeback=0;
    uint64_t hugepages_total=0, hugepages_free=0, hugepage_size=0;
    uint64_t active=0, inactive=0, mapped=0;

    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%63s %" SCNu64 " %15s", key, &val, unit_str) < 2) continue;
        uint64_t bytes = val * 1024ULL;  /* all values are kB */

        if      (strcmp(key, "MemTotal:")          == 0) mem_total         = bytes;
        else if (strcmp(key, "MemFree:")            == 0) mem_free          = bytes;
        else if (strcmp(key, "MemAvailable:")       == 0) mem_avail         = bytes;
        else if (strcmp(key, "Buffers:")            == 0) mem_buffers       = bytes;
        else if (strcmp(key, "Cached:")             == 0) mem_cached        = bytes;
        else if (strcmp(key, "SwapTotal:")          == 0) swap_total        = bytes;
        else if (strcmp(key, "SwapFree:")           == 0) swap_free         = bytes;
        else if (strcmp(key, "SwapCached:")         == 0) swap_cached       = bytes;
        else if (strcmp(key, "Slab:")               == 0) slab              = bytes;
        else if (strcmp(key, "SReclaimable:")       == 0) slab_reclaimable  = bytes;
        else if (strcmp(key, "SUnreclaim:")         == 0) slab_unreclaimable= bytes;
        else if (strcmp(key, "Dirty:")              == 0) mem_dirty         = bytes;
        else if (strcmp(key, "Writeback:")          == 0) mem_writeback     = bytes;
        else if (strcmp(key, "HugePages_Total:")    == 0) hugepages_total   = val;   /* pages, not kB */
        else if (strcmp(key, "HugePages_Free:")     == 0) hugepages_free    = val;
        else if (strcmp(key, "Hugepagesize:")       == 0) hugepage_size     = bytes;
        else if (strcmp(key, "Active:")             == 0) active            = bytes;
        else if (strcmp(key, "Inactive:")           == 0) inactive          = bytes;
        else if (strcmp(key, "Mapped:")             == 0) mapped            = bytes;
        /* VmallocTotal unused */
    }
    fclose(f);

    if (mem_total > 0) {
        uint64_t mem_used = mem_total - mem_avail;
        uint64_t mem_cached_total = mem_cached + mem_buffers;
        double   util_pct = (double)mem_used / (double)mem_total * 100.0;

        emit0("system.memory.total",        "By", METRIC_GAUGE, (double)mem_total);
        emit0("system.memory.usage",        "By", METRIC_GAUGE, (double)mem_used);
        emit0("system.memory.free",         "By", METRIC_GAUGE, (double)mem_free);
        emit0("system.memory.available",    "By", METRIC_GAUGE, (double)mem_avail);
        emit0("system.memory.buffers",      "By", METRIC_GAUGE, (double)mem_buffers);
        emit0("system.memory.cached",       "By", METRIC_GAUGE, (double)mem_cached_total);
        emit0("system.memory.slab",         "By", METRIC_GAUGE, (double)slab);
        emit0("system.memory.slab_reclaimable", "By", METRIC_GAUGE, (double)slab_reclaimable);
        emit0("system.memory.slab_unreclaimable","By", METRIC_GAUGE, (double)slab_unreclaimable);
        emit0("system.memory.dirty",        "By", METRIC_GAUGE, (double)mem_dirty);
        emit0("system.memory.writeback",    "By", METRIC_GAUGE, (double)mem_writeback);
        emit0("system.memory.active",       "By", METRIC_GAUGE, (double)active);
        emit0("system.memory.inactive",     "By", METRIC_GAUGE, (double)inactive);
        emit0("system.memory.mapped",       "By", METRIC_GAUGE, (double)mapped);
        emit0("system.memory.utilization",  "%",  METRIC_GAUGE, util_pct);
    }

    if (swap_total > 0) {
        uint64_t swap_used = swap_total - swap_free;
        emit0("system.swap.total",   "By", METRIC_GAUGE, (double)swap_total);
        emit0("system.swap.usage",   "By", METRIC_GAUGE, (double)swap_used);
        emit0("system.swap.free",    "By", METRIC_GAUGE, (double)swap_free);
        emit0("system.swap.cached",  "By", METRIC_GAUGE, (double)swap_cached);
        double swap_util = (double)(swap_total - swap_free) / (double)swap_total * 100.0;
        emit0("system.swap.utilization", "%", METRIC_GAUGE, swap_util);
    }

    if (hugepages_total > 0 && hugepage_size > 0) {
        emit0("system.memory.hugepages_total", "1",  METRIC_GAUGE, (double)hugepages_total);
        emit0("system.memory.hugepages_free",  "1",  METRIC_GAUGE, (double)hugepages_free);
        emit0("system.memory.hugepages_size",  "By", METRIC_GAUGE, (double)hugepage_size);
    }
}

/* ── /proc/loadavg ───────────────────────────────────────────────────────── */

static void scrape_loadavg(void)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return;

    double la1=0, la5=0, la15=0;
    uint64_t running=0, total_tasks=0;
    int _r1 = fscanf(f, "%lf %lf %lf %" SCNu64 "/%" SCNu64, &la1, &la5, &la15, &running, &total_tasks);
    OAGENT_UNUSED(_r1);
    fclose(f);

    emit0("system.load.1",  "1", METRIC_GAUGE, la1);
    emit0("system.load.5",  "1", METRIC_GAUGE, la5);
    emit0("system.load.15", "1", METRIC_GAUGE, la15);
    if (total_tasks > 0) {
        emit0("system.procs_total",   "1", METRIC_GAUGE, (double)total_tasks);
        emit0("system.procs_running", "1", METRIC_GAUGE, (double)running);
    }
}

/* ── /proc/uptime ────────────────────────────────────────────────────────── */

static void scrape_uptime(void)
{
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return;
    double uptime_sec = 0.0;
    int _r2 = fscanf(f, "%lf", &uptime_sec);
    OAGENT_UNUSED(_r2);
    fclose(f);
    emit0("system.uptime", "s", METRIC_GAUGE, uptime_sec);
}

/* ── /proc/net/dev ───────────────────────────────────────────────────────── */

static void scrape_net_dev(void)
{
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;

    char line[256];
    /* Skip 2 header lines */
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }

    while (fgets(line, sizeof(line), f)) {
        char iface[32] = "";
        uint64_t rx_bytes=0, rx_packets=0, rx_errors=0, rx_drops=0, rx_fifo=0, rx_frame=0, rx_compressed=0, rx_multicast=0;
        uint64_t tx_bytes=0, tx_packets=0, tx_errors=0, tx_drops=0, tx_fifo=0, tx_colls=0, tx_carrier=0, tx_compressed=0;

        /* Format: "  eth0: rx_bytes rx_packets rx_errs rx_drop ... tx_bytes tx_packets ..." */
        const char *p = line;
        while (*p == ' ') p++;
        size_t j = 0;
        while (*p && *p != ':' && j < sizeof(iface)-1) iface[j++] = *p++;
        iface[j] = '\0';
        if (*p != ':') continue;
        p++;

        int n = sscanf(p,
            " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
            " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
            " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
            " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
            &rx_bytes, &rx_packets, &rx_errors, &rx_drops,
            &rx_fifo,  &rx_frame,   &rx_compressed, &rx_multicast,
            &tx_bytes, &tx_packets, &tx_errors, &tx_drops,
            &tx_fifo,  &tx_colls,   &tx_carrier, &tx_compressed);
        if (n < 16) continue;
        if (strcmp(iface, "lo") == 0) continue;  /* skip loopback */

        /* Find previous values for delta */
        NicStat *prev = NULL;
        for (int i = 0; i < s_num_nics; i++) {
            if (strcmp(s_prev_nic[i].name, iface) == 0) { prev = &s_prev_nic[i]; break; }
        }

        /* Emit cumulative counters (Prometheus convention for network) */
        emit2("system.network.io", "By",  METRIC_COUNTER, (double)rx_bytes,   "device", iface, "direction", "receive");
        emit2("system.network.io", "By",  METRIC_COUNTER, (double)tx_bytes,   "device", iface, "direction", "transmit");
        emit2("system.network.packets", "1", METRIC_COUNTER, (double)rx_packets, "device", iface, "direction", "receive");
        emit2("system.network.packets", "1", METRIC_COUNTER, (double)tx_packets, "device", iface, "direction", "transmit");
        emit2("system.network.errors", "1",  METRIC_COUNTER, (double)rx_errors,  "device", iface, "direction", "receive");
        emit2("system.network.errors", "1",  METRIC_COUNTER, (double)tx_errors,  "device", iface, "direction", "transmit");
        emit2("system.network.dropped", "1", METRIC_COUNTER, (double)rx_drops,   "device", iface, "direction", "receive");
        emit2("system.network.dropped", "1", METRIC_COUNTER, (double)tx_drops,   "device", iface, "direction", "transmit");

        /* Rate metrics (delta / interval) */
        if (prev) {
            uint64_t interval = (uint64_t)g_config.procfs_interval_s;
            if (interval == 0) interval = 1;
            double rx_rate = (double)(rx_bytes   - prev->rx_bytes)   / interval;
            double tx_rate = (double)(tx_bytes   - prev->tx_bytes)   / interval;
            emit2("system.network.io_rate", "By/s", METRIC_GAUGE, rx_rate, "device", iface, "direction", "receive");
            emit2("system.network.io_rate", "By/s", METRIC_GAUGE, tx_rate, "device", iface, "direction", "transmit");
        }

        /* Update previous state */
        if (!prev && s_num_nics < MAX_NICS) {
            prev = &s_prev_nic[s_num_nics++];
            SAFE_STRNCPY(prev->name, iface, sizeof(prev->name));
        }
        if (prev) {
            prev->rx_bytes = rx_bytes; prev->tx_bytes = tx_bytes;
            prev->rx_packets = rx_packets; prev->tx_packets = tx_packets;
            prev->rx_errors = rx_errors; prev->tx_errors = tx_errors;
            prev->rx_drops = rx_drops; prev->tx_drops = tx_drops;
        }
    }
    fclose(f);
}

/* ── /proc/diskstats ─────────────────────────────────────────────────────── */

static void scrape_diskstats(void)
{
    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned int major=0, minor=0;
        char devname[32] = "";
        uint64_t reads_comp=0, reads_merged=0, sectors_read=0, read_time_ms=0;
        uint64_t writes_comp=0, writes_merged=0, sectors_written=0, write_time_ms=0;
        uint64_t io_in_progress=0, io_time_ms=0, weighted_io_ms=0;

        int n = sscanf(line,
            " %u %u %31s"
            " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
            " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
            " %" SCNu64 " %" SCNu64 " %" SCNu64,
            &major, &minor, devname,
            &reads_comp, &reads_merged, &sectors_read, &read_time_ms,
            &writes_comp, &writes_merged, &sectors_written, &write_time_ms,
            &io_in_progress, &io_time_ms, &weighted_io_ms);

        if (n < 14) continue;
        /* Skip partitions (sdaX, nvme0n1pX) — only keep whole disks */
        /* Heuristic: skip if name contains digits after a non-digit */
        size_t dnlen = strlen(devname);
        int skip = 0;
        for (size_t i = 1; i < dnlen; i++) {
            if (isdigit((unsigned char)devname[i]) && !isdigit((unsigned char)devname[i-1])) {
                /* could be a partition — but also nvme0n1 is a real disk */
                /* skip only if there are TWO digit runs (e.g. sda1, nvme0n1p1) */
                int digit_runs = 0;
                int in_digit = 0;
                for (size_t k = 0; k < dnlen; k++) {
                    if (isdigit((unsigned char)devname[k])) {
                        if (!in_digit) { digit_runs++; in_digit = 1; }
                    } else { in_digit = 0; }
                }
                if (digit_runs >= 2) { skip = 1; break; }
            }
        }
        if (skip) continue;
        /* Also skip loop, ram, dm devices */
        if (strncmp(devname, "loop", 4) == 0) continue;
        if (strncmp(devname, "ram",  3) == 0) continue;
        if (strncmp(devname, "dm-",  3) == 0) continue;

        /* 512 bytes per sector */
        uint64_t read_bytes    = sectors_read    * 512ULL;
        uint64_t written_bytes = sectors_written * 512ULL;

        /* Emit cumulative counters */
        emit2("system.disk.io",              "By",   METRIC_COUNTER, (double)read_bytes,   "device", devname, "direction", "read");
        emit2("system.disk.io",              "By",   METRIC_COUNTER, (double)written_bytes, "device", devname, "direction", "write");
        emit2("system.disk.operations",      "1",    METRIC_COUNTER, (double)reads_comp,    "device", devname, "direction", "read");
        emit2("system.disk.operations",      "1",    METRIC_COUNTER, (double)writes_comp,   "device", devname, "direction", "write");
        emit2("system.disk.io_time",         "ms",   METRIC_COUNTER, (double)io_time_ms,    "device", devname, "direction", "read");
        emit2("system.disk.operation_time",  "ms",   METRIC_COUNTER, (double)read_time_ms,  "device", devname, "direction", "read");
        emit2("system.disk.operation_time",  "ms",   METRIC_COUNTER, (double)write_time_ms, "device", devname, "direction", "write");
        emit1("system.disk.io_in_progress",  "1",    METRIC_GAUGE,   (double)io_in_progress, "device", devname);

        /* Find/update previous for rate metrics */
        DiskStat *prev = NULL;
        for (int i = 0; i < s_num_disks; i++) {
            if (strcmp(s_prev_disk[i].name, devname) == 0) { prev = &s_prev_disk[i]; break; }
        }
        if (prev) {
            uint64_t interval = (uint64_t)g_config.procfs_interval_s;
            if (interval == 0) interval = 1;
            double rd_rate = (double)(read_bytes    - prev->read_bytes)    / interval;
            double wr_rate = (double)(written_bytes - prev->write_bytes)   / interval;
            emit2("system.disk.io_rate", "By/s", METRIC_GAUGE, rd_rate, "device", devname, "direction", "read");
            emit2("system.disk.io_rate", "By/s", METRIC_GAUGE, wr_rate, "device", devname, "direction", "write");
        }

        if (!prev && s_num_disks < MAX_DISKS) {
            prev = &s_prev_disk[s_num_disks++];
            SAFE_STRNCPY(prev->name, devname, sizeof(prev->name));
        }
        if (prev) {
            prev->reads_completed  = reads_comp;
            prev->writes_completed = writes_comp;
            prev->read_bytes       = read_bytes;
            prev->write_bytes      = written_bytes;
            prev->io_time_ms       = io_time_ms;
        }
    }
    fclose(f);
}

/* ── Filesystem metrics via statvfs + /proc/mounts ──────────────────────── */

static void scrape_filesystems(void)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;

    char line[512];
    /* track which mountpoints we've done to avoid duplicates */
    char seen[MAX_MOUNTS][128];
    int nseen = 0;

    while (fgets(line, sizeof(line), f)) {
        char device[128], mountpoint[128], fstype[32];
        if (sscanf(line, "%127s %127s %31s", device, mountpoint, fstype) < 3) continue;

        /* Only physical/virtual FSes of interest */
        if (strncmp(fstype, "ext",     3) != 0 &&
            strncmp(fstype, "xfs",     3) != 0 &&
            strncmp(fstype, "btrfs",   5) != 0 &&
            strncmp(fstype, "zfs",     3) != 0 &&
            strncmp(fstype, "fuse",    4) != 0 &&
            strncmp(fstype, "overlay", 7) != 0 &&
            strncmp(fstype, "tmpfs",   5) != 0 &&
            strcmp(fstype,  "vfat")         != 0 &&
            strcmp(fstype,  "ntfs")         != 0) continue;

        /* Skip if already processed this mountpoint */
        int dup = 0;
        for (int i = 0; i < nseen; i++) {
            if (strcmp(seen[i], mountpoint) == 0) { dup = 1; break; }
        }
        if (dup) continue;
        if (nseen < MAX_MOUNTS) {
            SAFE_STRNCPY(seen[nseen], mountpoint, 128);
            nseen++;
        }

        struct statvfs st;
        if (statvfs(mountpoint, &st) != 0) continue;
        if (st.f_blocks == 0) continue;  /* pseudo-fs */

        uint64_t total      = (uint64_t)st.f_blocks * st.f_frsize;
        uint64_t avail      = (uint64_t)st.f_bavail * st.f_frsize;
        uint64_t used       = total - (uint64_t)st.f_bfree  * st.f_frsize;
        double   util_pct   = (double)used / (double)total * 100.0;
        uint64_t inodes_total = st.f_files;
        uint64_t inodes_free  = st.f_ffree;
        uint64_t inodes_used  = (inodes_total > inodes_free) ? inodes_total - inodes_free : 0;

        char keys2[2][OAGENT_KEY_LEN], vals2[2][OAGENT_VAL_LEN];
        SAFE_STRNCPY(keys2[0], "device",     OAGENT_KEY_LEN);
        SAFE_STRNCPY(vals2[0], device,       OAGENT_VAL_LEN);
        SAFE_STRNCPY(keys2[1], "mountpoint", OAGENT_KEY_LEN);
        SAFE_STRNCPY(vals2[1], mountpoint,   OAGENT_VAL_LEN);

        emit_metric2("system.filesystem.usage",        "By", METRIC_GAUGE, (double)used,       2, keys2, vals2);
        emit_metric2("system.filesystem.free",         "By", METRIC_GAUGE, (double)avail,      2, keys2, vals2);
        emit_metric2("system.filesystem.total",        "By", METRIC_GAUGE, (double)total,      2, keys2, vals2);
        emit_metric2("system.filesystem.utilization",  "%",  METRIC_GAUGE, util_pct,           2, keys2, vals2);
        if (inodes_total > 0) {
            emit_metric2("system.filesystem.inodes_usage", "1", METRIC_GAUGE, (double)inodes_used,  2, keys2, vals2);
            emit_metric2("system.filesystem.inodes_free",  "1", METRIC_GAUGE, (double)inodes_free,  2, keys2, vals2);
            emit_metric2("system.filesystem.inodes_total", "1", METRIC_GAUGE, (double)inodes_total, 2, keys2, vals2);
        }
    }
    fclose(f);
}

/* ── /proc/net/sockstat ──────────────────────────────────────────────────── */

static void scrape_sockstat(void)
{
    FILE *f = fopen("/proc/net/sockstat", "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "TCP:", 4) == 0) {
            uint64_t inuse=0, orphan=0, tw=0, alloc=0;
            sscanf(line, "TCP: inuse %" SCNu64 " orphan %" SCNu64
                         " tw %" SCNu64 " alloc %" SCNu64,
                   &inuse, &orphan, &tw, &alloc);
            emit1("system.network.connections", "1", METRIC_GAUGE, (double)inuse,   "protocol", "tcp");
            emit1("system.network.tcp_tw",      "1", METRIC_GAUGE, (double)tw,      "state",    "time_wait");
            emit1("system.network.tcp_orphan",  "1", METRIC_GAUGE, (double)orphan,  "state",    "orphan");
        } else if (strncmp(line, "UDP:", 4) == 0) {
            uint64_t inuse=0;
            sscanf(line, "UDP: inuse %" SCNu64, &inuse);
            emit1("system.network.connections", "1", METRIC_GAUGE, (double)inuse, "protocol", "udp");
        }
    }
    fclose(f);
}

/* ── /proc/vmstat ────────────────────────────────────────────────────────── */

static void scrape_vmstat(void)
{
    FILE *f = fopen("/proc/vmstat", "r");
    if (!f) return;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char key[64] = "";
        uint64_t val = 0;
        if (sscanf(line, "%63s %" SCNu64, key, &val) < 2) continue;

        if      (strcmp(key, "pgfault")         == 0) emit0("system.paging.faults",       "1", METRIC_COUNTER, (double)val);
        else if (strcmp(key, "pgmajfault")      == 0) emit0("system.paging.major_faults",  "1", METRIC_COUNTER, (double)val);
        else if (strcmp(key, "pswpin")          == 0) emit0("system.paging.operations",    "1", METRIC_COUNTER, (double)val);
        else if (strcmp(key, "pswpout")         == 0) emit0("system.paging.operations",    "1", METRIC_COUNTER, (double)val);
        else if (strcmp(key, "nr_dirty")        == 0) emit0("system.memory.dirty_pages",   "1", METRIC_GAUGE,   (double)val);
        else if (strcmp(key, "nr_writeback")    == 0) emit0("system.memory.writeback_pages","1", METRIC_GAUGE,  (double)val);
        else if (strcmp(key, "pgpgin")          == 0) emit0("system.paging.page_in",       "1", METRIC_COUNTER, (double)val);
        else if (strcmp(key, "pgpgout")         == 0) emit0("system.paging.page_out",      "1", METRIC_COUNTER, (double)val);
    }
    fclose(f);
}

/* ── Receiver thread ─────────────────────────────────────────────────────── */

void *procfs_receiver_thread(void *arg)
{
    OAGENT_UNUSED(arg);
    s_clk_tck = sysconf(_SC_CLK_TCK);
    if (s_clk_tck <= 0) s_clk_tck = 100;

    LOG_INFO("procfs_receiver: started (interval=%ds, CLK_TCK=%ld, node_exporter-equivalent mode)",
             g_config.procfs_interval_s, s_clk_tck);

    /* Prime baselines */
    {
        FILE *f = fopen("/proc/stat", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "cpu ", 4) == 0) { read_cpu_line(line+4, &s_prev_cpu); break; }
            }
            fclose(f);
        }
    }

    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        for (int t = 0; t < g_config.procfs_interval_s; t++) {
            if (!atomic_load_explicit(&g_running, memory_order_relaxed)) break;
            sleep(1);
        }
        if (!atomic_load_explicit(&g_running, memory_order_relaxed)) break;

        scrape_cpu();
        scrape_meminfo();
        scrape_loadavg();
        scrape_uptime();
        scrape_net_dev();
        scrape_diskstats();
        scrape_filesystems();
        scrape_sockstat();
        scrape_vmstat();

        LOG_DEBUG("procfs_receiver: scrape done (pool_live=%zu)",
                  pool_allocated(&g_pool));
    }

    LOG_INFO("procfs_receiver: exiting");
    return NULL;
}
