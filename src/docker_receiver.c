/*
 * omniagent/src/docker_receiver.c
 *
 * cAdvisor-equivalent Docker metrics via Docker Engine API (Unix socket).
 *
 * Metrics emitted per container (matches cAdvisor label set):
 *   container.cpu.utilization         — CPU % (all cores)
 *   container.cpu.user                — user CPU %
 *   container.cpu.system              — kernel CPU %
 *   container.cpu.throttled_periods   — throttled CPU periods (counter)
 *   container.cpu.throttled_time      — throttled CPU time ns (counter)
 *   container.memory.usage            — working set bytes
 *   container.memory.limit            — memory limit bytes
 *   container.memory.rss              — anonymous RSS bytes
 *   container.memory.cache            — page cache bytes
 *   container.memory.swap             — swap bytes
 *   container.memory.utilization      — usage / limit %
 *   container.network.io              — rx/tx bytes (counter, per interface)
 *   container.network.packets         — rx/tx packets (counter)
 *   container.network.errors          — rx/tx errors (counter)
 *   container.network.dropped         — rx/tx drops (counter)
 *   container.disk.io                 — read/write bytes (counter)
 *   container.disk.operations         — read/write ops (counter)
 *   container.restarts                — restart count (gauge)
 *   container.oom_events              — OOM kill count (counter)
 */

#include "omniagent.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <ctype.h>

#define DOCKER_SOCK_PATH  "/var/run/docker.sock"
#define DOCKER_POLL_SECS  15
#define HTTP_BUF_SIZE     (256 * 1024)   /* 256 KB */

/* ── Unix-socket HTTP helper ─────────────────────────────────────────────── */

static int docker_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { LOG_ERROR("docker: socket: %s", strerror(errno)); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(DOCKER_SOCK_PATH) >= sizeof(addr.sun_path)) {
        LOG_ERROR("docker: path too long"); close(fd); return -1;
    }
    strncpy(addr.sun_path, DOCKER_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_WARN("docker: connect(%s): %s", DOCKER_SOCK_PATH, strerror(errno));
        close(fd); return -1;
    }
    /* 10-second timeout */
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

static ssize_t docker_get(const char *path, char *buf, size_t buf_size)
{
    int fd = docker_connect();
    if (fd < 0) return -1;

    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: localhost\r\nAccept: application/json\r\nConnection: close\r\n\r\n",
        path);
    if (req_len < 0 || (size_t)req_len >= sizeof(req)) { close(fd); return -1; }

    ssize_t sent = 0;
    while (sent < req_len) {
        ssize_t n = write(fd, req + sent, (size_t)(req_len - sent));
        if (n <= 0) { close(fd); return -1; }
        sent += n;
    }

    ssize_t total = 0;
    while (total < (ssize_t)buf_size - 1) {
        ssize_t n = read(fd, buf + total, buf_size - (size_t)total - 1);
        if (n < 0) { close(fd); return -1; }
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';
    close(fd);

    /* Find body */
    char *body = strstr(buf, "\r\n\r\n");
    if (!body) return -1;
    body += 4;

    if (strncmp(buf, "HTTP/1.1 2", 10) != 0) return -1;

    /* De-chunk if needed */
    if (strstr(buf, "Transfer-Encoding: chunked") || strstr(buf, "transfer-encoding: chunked")) {
        char *src = body, *dst = body;
        ssize_t body_len = (buf + total) - body;
        while (*src) {
            char *eol = strstr(src, "\r\n");
            if (!eol) break;
            *eol = '\0';
            long csz = strtol(src, NULL, 16);
            *eol = '\r';
            if (csz == 0) break;
            src = eol + 2;
            if (src + csz > body + body_len) break;
            memmove(dst, src, (size_t)csz);
            dst += csz;
            src += csz + 2;
        }
        *dst = '\0';
        total = (dst - buf) + (body - buf);  /* not strictly needed */
        memmove(buf, body, (size_t)(dst - body) + 1);
        return dst - body;
    }

    ssize_t body_len = (buf + total) - body;
    memmove(buf, body, (size_t)body_len + 1);
    return body_len;
}

/* ── Minimal JSON helpers ────────────────────────────────────────────────── */

/* Find value of string key within JSON object starting at p */
static const char *json_str(const char *p, const char *key, char *out, size_t outsz)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *pos = strstr(p, pat);
    if (!pos) return NULL;
    pos += strlen(pat);
    while (*pos == ' ' || *pos == '\t' || *pos == ':') pos++;
    if (*pos != '"') return NULL;
    pos++;
    size_t i = 0;
    while (*pos && *pos != '"' && i < outsz - 1) {
        if (*pos == '\\') { pos++; if (*pos) out[i++] = *pos; }
        else out[i++] = *pos;
        pos++;
    }
    out[i] = '\0';
    if (*pos == '"') pos++;
    return pos;
}

/* Find numeric value of key */
static int json_num(const char *p, const char *key, double *out)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *pos = strstr(p, pat);
    if (!pos) return 0;
    pos += strlen(pat);
    while (*pos == ' ' || *pos == '\t' || *pos == ':') pos++;
    if (!(*pos == '-' || isdigit((unsigned char)*pos))) return 0;
    char *end;
    *out = strtod(pos, &end);
    return end != pos;
}

/* Find the start of a named JSON object/array */
static const char *json_section(const char *p, const char *key)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *pos = strstr(p, pat);
    if (!pos) return NULL;
    pos += strlen(pat);
    while (*pos == ' ' || *pos == '\t' || *pos == ':') pos++;
    return pos;
}

/* ── Container info ──────────────────────────────────────────────────────── */

typedef struct {
    char id[65];
    char name[128];
    char image[128];
} ContainerInfo;

static int parse_containers_json(const char *json, ContainerInfo *info, int max)
{
    int count = 0;
    const char *p = json;
    while (count < max && (p = strchr(p, '{')) != NULL) {
        ContainerInfo *ci = &info[count];
        memset(ci, 0, sizeof(*ci));
        json_str(p, "Id",    ci->id,    sizeof(ci->id));
        json_str(p, "Image", ci->image, sizeof(ci->image));
        /* Names is a JSON array: ["\/grafana"] — find first quoted string */
        const char *np = json_section(p, "Names");
        if (np) {
            /* skip to opening bracket then find first quoted value */
            const char *arr = strchr(np, '[');
            if (arr) {
                const char *q = strchr(arr, '"');
                if (q) {
                    q++; /* skip opening quote */
                    char raw[128];
                    size_t ri = 0;
                    while (*q && *q != '"' && ri < sizeof(raw) - 1) {
                        if (*q == '\\') { q++; if (*q) raw[ri++] = *q; }
                        else raw[ri++] = *q;
                        q++;
                    }
                    raw[ri] = '\0';
                    /* strip leading slashes */
                    const char *ns = raw;
                    while (*ns == '/') ns++;
                    SAFE_STRNCPY(ci->name, ns, sizeof(ci->name));
                }
            }
        }
        const char *next = strchr(p + 1, '}');
        if (!next) break;
        p = next + 1;
        count++;
    }
    return count;
}

/* ── Metric emitter for containers ─────────────────────────────────────────*/

static void emit_ctr(const char *name, const char *unit, MetricType type,
                     double value, const ContainerInfo *ci,
                     const char *extra_k, const char *extra_v)
{
    TelemetryRecord *rec = pool_alloc(&g_pool);
    if (!rec) return;

    rec->type = TELEM_METRIC;
    SAFE_STRNCPY(rec->metric.name, name, OAGENT_NAME_MAX);
    SAFE_STRNCPY(rec->metric.unit, unit, OAGENT_UNIT_MAX);
    rec->metric.type         = type;
    rec->metric.value        = value;
    rec->metric.timestamp_ns = telem_now_ns();

    int lc = 0;
    SAFE_STRNCPY(rec->metric.label_keys[lc],   "container_name", OAGENT_KEY_LEN);
    SAFE_STRNCPY(rec->metric.label_values[lc],  ci->name,         OAGENT_VAL_LEN); lc++;
    SAFE_STRNCPY(rec->metric.label_keys[lc],   "image",          OAGENT_KEY_LEN);
    SAFE_STRNCPY(rec->metric.label_values[lc],  ci->image,        OAGENT_VAL_LEN); lc++;
    if (extra_k && extra_v) {
        SAFE_STRNCPY(rec->metric.label_keys[lc],   extra_k, OAGENT_KEY_LEN);
        SAFE_STRNCPY(rec->metric.label_values[lc],  extra_v, OAGENT_VAL_LEN); lc++;
    }
    rec->metric.label_count = lc;
    SAFE_STRNCPY(rec->container_id,  ci->id,   OAGENT_RESOURCE_MAX);
    SAFE_STRNCPY(rec->service_name,  ci->name, OAGENT_RESOURCE_MAX);
    char hostname[64] = "unknown";
    gethostname(hostname, sizeof(hostname) - 1);
    SAFE_STRNCPY(rec->host_name, hostname, OAGENT_RESOURCE_MAX);

    if (!rb_enqueue(&g_recv_queue, rec)) pool_free(&g_pool, rec);
}

/* ── Parse /containers/<id>/stats ────────────────────────────────────────── */

static void fetch_container_stats(const ContainerInfo *ci, char *buf, size_t buf_size)
{
    char path[256];
    snprintf(path, sizeof(path), "/containers/%.64s/stats?stream=false", ci->id);

    ssize_t n = docker_get(path, buf, buf_size);
    if (n <= 0) return;

    /* ── CPU ── */
    const char *cpu_pos     = json_section(buf,    "cpu_stats");
    const char *precpu_pos  = json_section(buf,    "precpu_stats");
    const char *cpu_usage   = cpu_pos    ? json_section(cpu_pos,    "cpu_usage") : NULL;
    const char *pcpu_usage  = precpu_pos ? json_section(precpu_pos, "cpu_usage") : NULL;

    double total_usage_cur=0, total_usage_prev=0, system_cur=0, system_prev=0;
    double throttled_periods=0, throttled_time_ns=0;
    double cpu_user=0, cpu_kernel=0;

    if (cpu_usage)   json_num(cpu_usage,  "total_usage",  &total_usage_cur);
    if (cpu_usage)   json_num(cpu_usage,  "usage_in_usermode",   &cpu_user);
    if (cpu_usage)   json_num(cpu_usage,  "usage_in_kernelmode", &cpu_kernel);
    if (cpu_pos)     json_num(cpu_pos,    "system_cpu_usage", &system_cur);
    if (pcpu_usage)  json_num(pcpu_usage, "total_usage",  &total_usage_prev);
    if (precpu_pos)  json_num(precpu_pos, "system_cpu_usage", &system_prev);

    /* Throttling */
    const char *throttling = cpu_pos ? json_section(cpu_pos, "throttling_data") : NULL;
    if (throttling) {
        json_num(throttling, "throttled_periods", &throttled_periods);
        json_num(throttling, "throttled_time",    &throttled_time_ns);
    }

    /* Count CPUs */
    long num_cpus = 1;
    const char *percpu_pos = strstr(buf, "\"percpu_usage\"");
    if (percpu_pos) {
        const char *a = strchr(percpu_pos, '[');
        const char *z = a ? strchr(a, ']') : NULL;
        if (a && z) {
            num_cpus = 0;
            for (const char *cp = a; cp < z; cp++) if (*cp == ',') num_cpus++;
            num_cpus++;
        }
    }
    /* Fallback: get online_cpus field */
    if (num_cpus <= 0) {
        double nc = 0;
        if (cpu_pos && json_num(cpu_pos, "online_cpus", &nc) && nc > 0) num_cpus = (long)nc;
        else num_cpus = 1;
    }

    double cpu_pct = 0.0, cpu_user_pct=0.0, cpu_kernel_pct=0.0;
    double sys_delta = system_cur - system_prev;
    double cpu_delta = total_usage_cur - total_usage_prev;
    if (sys_delta > 0.0 && cpu_delta >= 0.0) {
        cpu_pct        = (cpu_delta / sys_delta) * (double)num_cpus * 100.0;
        double user_d  = cpu_user   - (pcpu_usage ? ({double v=0; json_num(pcpu_usage,"usage_in_usermode",&v); v;}) : 0.0);
        double kern_d  = cpu_kernel - (pcpu_usage ? ({double v=0; json_num(pcpu_usage,"usage_in_kernelmode",&v); v;}) : 0.0);
        cpu_user_pct   = (user_d   / sys_delta) * (double)num_cpus * 100.0;
        cpu_kernel_pct = (kern_d   / sys_delta) * (double)num_cpus * 100.0;
    }

    emit_ctr("container.cpu.utilization",       "%",  METRIC_GAUGE,   cpu_pct,            ci, NULL, NULL);
    emit_ctr("container.cpu.user",              "%",  METRIC_GAUGE,   cpu_user_pct,        ci, NULL, NULL);
    emit_ctr("container.cpu.system",            "%",  METRIC_GAUGE,   cpu_kernel_pct,      ci, NULL, NULL);
    if (throttled_periods > 0)
        emit_ctr("container.cpu.throttled_periods", "1",  METRIC_COUNTER, throttled_periods,  ci, NULL, NULL);
    if (throttled_time_ns > 0)
        emit_ctr("container.cpu.throttled_time",    "ns", METRIC_COUNTER, throttled_time_ns,  ci, NULL, NULL);

    /* ── Memory ── */
    const char *mem_pos = json_section(buf, "memory_stats");
    double mem_usage=0, mem_limit=0, mem_cache=0, mem_rss=0, mem_swap=0;
    double mem_oom=0;
    if (mem_pos) {
        json_num(mem_pos, "usage",  &mem_usage);
        json_num(mem_pos, "limit",  &mem_limit);
        json_num(mem_pos, "failcnt",&mem_oom);
        const char *stats_pos = json_section(mem_pos, "stats");
        if (stats_pos) {
            json_num(stats_pos, "cache",        &mem_cache);
            json_num(stats_pos, "rss",          &mem_rss);
            /* cgroups v2 uses anon instead of rss */
            if (mem_rss == 0) json_num(stats_pos, "anon", &mem_rss);
            double swap_val=0;
            if (json_num(stats_pos, "swap", &swap_val)) mem_swap = swap_val - mem_usage;
            if (mem_swap < 0) mem_swap = 0;
        }
    }

    /* Working set = usage - cache (matches cAdvisor definition) */
    double working_set = (mem_usage > mem_cache) ? mem_usage - mem_cache : mem_usage;

    emit_ctr("container.memory.usage",       "By", METRIC_GAUGE, working_set, ci, NULL, NULL);
    emit_ctr("container.memory.rss",         "By", METRIC_GAUGE, mem_rss,     ci, NULL, NULL);
    emit_ctr("container.memory.cache",       "By", METRIC_GAUGE, mem_cache,   ci, NULL, NULL);
    if (mem_limit > 0 && mem_limit < 1e18) {
        emit_ctr("container.memory.limit",   "By", METRIC_GAUGE, mem_limit,   ci, NULL, NULL);
        double util = working_set / mem_limit * 100.0;
        emit_ctr("container.memory.utilization", "%", METRIC_GAUGE, util,     ci, NULL, NULL);
    }
    if (mem_swap > 0)
        emit_ctr("container.memory.swap",    "By", METRIC_GAUGE, mem_swap,    ci, NULL, NULL);
    if (mem_oom > 0)
        emit_ctr("container.oom_events",     "1",  METRIC_COUNTER, mem_oom,   ci, NULL, NULL);

    /* ── Network ── */
    const char *net_pos = json_section(buf, "networks");
    if (net_pos) {
        /* Iterate each interface: "eth0": { "rx_bytes": ... } */
        const char *p = net_pos;
        while ((p = strchr(p, '"')) != NULL) {
            /* Read interface name */
            p++;
            char iface[32] = "";
            size_t j = 0;
            while (*p && *p != '"' && j < sizeof(iface)-1) iface[j++] = *p++;
            iface[j] = '\0';
            if (*p != '"') break;
            p++;
            /* Skip to '{' */
            while (*p && *p != '{' && *p != ']') p++;
            if (*p != '{') break;

            double rx_bytes=0, tx_bytes=0, rx_pkts=0, tx_pkts=0;
            double rx_err=0, tx_err=0, rx_drop=0, tx_drop=0;
            json_num(p, "rx_bytes",   &rx_bytes);
            json_num(p, "tx_bytes",   &tx_bytes);
            json_num(p, "rx_packets", &rx_pkts);
            json_num(p, "tx_packets", &tx_pkts);
            json_num(p, "rx_errors",  &rx_err);
            json_num(p, "tx_errors",  &tx_err);
            json_num(p, "rx_dropped", &rx_drop);
            json_num(p, "tx_dropped", &tx_drop);

            emit_ctr("container.network.io",      "By", METRIC_COUNTER, rx_bytes, ci, "direction", "receive");
            emit_ctr("container.network.io",      "By", METRIC_COUNTER, tx_bytes, ci, "direction", "transmit");
            emit_ctr("container.network.packets", "1",  METRIC_COUNTER, rx_pkts,  ci, "direction", "receive");
            emit_ctr("container.network.packets", "1",  METRIC_COUNTER, tx_pkts,  ci, "direction", "transmit");
            if (rx_err > 0) emit_ctr("container.network.errors",  "1", METRIC_COUNTER, rx_err,  ci, "direction", "receive");
            if (tx_err > 0) emit_ctr("container.network.errors",  "1", METRIC_COUNTER, tx_err,  ci, "direction", "transmit");
            if (rx_drop > 0) emit_ctr("container.network.dropped","1", METRIC_COUNTER, rx_drop, ci, "direction", "receive");
            if (tx_drop > 0) emit_ctr("container.network.dropped","1", METRIC_COUNTER, tx_drop, ci, "direction", "transmit");

            /* Skip to closing '}' for this interface */
            while (*p && *p != '}') p++;
            if (*p == '}') p++;
        }
    }

    /* ── Block I/O ── */
    const char *blk_pos = json_section(buf, "blkio_stats");
    if (blk_pos) {
        /* io_service_bytes_recursive: [{"major":...,"minor":...,"op":"Read","value":...}, ...] */
        const char *io_bytes = strstr(blk_pos, "\"io_service_bytes_recursive\"");
        if (io_bytes) {
            const char *arr = strchr(io_bytes, '[');
            if (arr) {
                double read_bytes=0, write_bytes=0;
                const char *entry = arr;
                while ((entry = strchr(entry, '{')) != NULL) {
                    char op[16] = "";
                    double val=0;
                    json_str(entry, "op", op, sizeof(op));
                    json_num(entry, "value", &val);
                    if (strcasecmp(op, "Read")  == 0) read_bytes  += val;
                    if (strcasecmp(op, "Write") == 0) write_bytes += val;
                    const char *end = strchr(entry + 1, '}');
                    if (!end) break;
                    entry = end + 1;
                    if (*entry == ']') break;
                }
                if (read_bytes > 0 || write_bytes > 0) {
                    emit_ctr("container.disk.io", "By", METRIC_COUNTER, read_bytes,  ci, "direction", "read");
                    emit_ctr("container.disk.io", "By", METRIC_COUNTER, write_bytes, ci, "direction", "write");
                }
            }
        }

        /* io_serviced_recursive: ops count */
        const char *io_ops = strstr(blk_pos, "\"io_serviced_recursive\"");
        if (io_ops) {
            const char *arr = strchr(io_ops, '[');
            if (arr) {
                double read_ops=0, write_ops=0;
                const char *entry = arr;
                while ((entry = strchr(entry, '{')) != NULL) {
                    char op[16] = "";
                    double val=0;
                    json_str(entry, "op", op, sizeof(op));
                    json_num(entry, "value", &val);
                    if (strcasecmp(op, "Read")  == 0) read_ops  += val;
                    if (strcasecmp(op, "Write") == 0) write_ops += val;
                    const char *end = strchr(entry + 1, '}');
                    if (!end) break;
                    entry = end + 1;
                    if (*entry == ']') break;
                }
                if (read_ops > 0 || write_ops > 0) {
                    emit_ctr("container.disk.operations", "1", METRIC_COUNTER, read_ops,  ci, "direction", "read");
                    emit_ctr("container.disk.operations", "1", METRIC_COUNTER, write_ops, ci, "direction", "write");
                }
            }
        }
    }
}

/* ── Fetch restart count from /containers/<id>/json ─────────────────────── */

static void fetch_container_inspect(const ContainerInfo *ci, char *buf, size_t buf_size)
{
    char path[256];
    snprintf(path, sizeof(path), "/containers/%.64s/json", ci->id);
    ssize_t n = docker_get(path, buf, buf_size);
    if (n <= 0) return;

    double restart_count = 0;
    if (json_num(buf, "RestartCount", &restart_count) && restart_count > 0) {
        emit_ctr("container.restarts", "1", METRIC_GAUGE, restart_count, ci, NULL, NULL);
    }
}

/* ── Main scrape cycle ───────────────────────────────────────────────────── */

static void docker_scrape(char *buf, size_t buf_size)
{
    ssize_t n = docker_get("/containers/json", buf, buf_size);
    if (n <= 0) return;

    ContainerInfo containers[64];
    int count = parse_containers_json(buf, containers, (int)(sizeof(containers)/sizeof(containers[0])));
    LOG_DEBUG("docker: found %d running containers", count);

    for (int i = 0; i < count; i++) {
        if (!atomic_load_explicit(&g_running, memory_order_relaxed)) break;
        if (containers[i].id[0] == '\0') continue;
        fetch_container_stats(&containers[i], buf, buf_size);
        if (!atomic_load_explicit(&g_running, memory_order_relaxed)) break;
        fetch_container_inspect(&containers[i], buf, buf_size);
    }
}

/* ── Receiver thread ─────────────────────────────────────────────────────── */

void *docker_receiver_thread(void *arg)
{
    OAGENT_UNUSED(arg);

    char *buf = malloc(HTTP_BUF_SIZE);
    if (!buf) { LOG_ERROR("docker: cannot allocate I/O buffer"); return NULL; }

    LOG_INFO("docker_receiver: started (poll=%ds, cAdvisor-equivalent mode)", DOCKER_POLL_SECS);

    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        docker_scrape(buf, HTTP_BUF_SIZE);
        for (int t = 0; t < DOCKER_POLL_SECS; t++) {
            if (!atomic_load_explicit(&g_running, memory_order_relaxed)) break;
            sleep(1);
        }
    }

    free(buf);
    LOG_INFO("docker_receiver: exiting");
    return NULL;
}
