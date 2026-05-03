/*
 * omniagent/src/prometheus_exporter.c
 *
 * Replaces zstd_exporter.  Two threads:
 *
 *   prometheus_exporter_thread
 *     Pops ExportBatch from g_export_queue.
 *     Metrics   → upsert into in-memory metric store.
 *     LogRecords → HTTP POST to Loki /loki/api/v1/push.
 *     After each batch, re-renders the Prometheus text page.
 *
 *   prometheus_http_thread
 *     TCP server on g_config.metrics_port (default 9100).
 *     GET /metrics  → Prometheus text exposition format.
 *     GET /health   → 200 OK (for liveness probes).
 *
 * Prometheus scrape format produced:
 *   omniagent_system_cpu_usage{host="myhost",mode="total"} 12.50 1714600000123
 *   omniagent_system_memory_usage{host="myhost"} 4294967296 1714600000123
 *   ...
 *
 * Loki push format (POST /loki/api/v1/push):
 *   {"streams":[{"stream":{"job":"omniagent","host":"..."},
 *                "values":[["<ts_ns>","[level] source: body"],...]}]}
 */

#include "omniagent.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ── Limits ──────────────────────────────────────────────────────────────── */

#define METRIC_STORE_SLOTS  8192          /* power-of-2; max distinct series  */
#define METRICS_BUF_SIZE    (1024 * 1024) /* 1 MB rendered Prometheus text    */
#define LOKI_BUF_SIZE       (2 * 1024 * 1024) /* 2 MB per Loki push body     */
#define HTTP_READ_BUF        4096
#define HTTP_RESP_HDR_SIZE   512

/* ── Metric store ────────────────────────────────────────────────────────── */

typedef struct {
    char    name[128];   /* sanitised metric name (dots/dashes → _)          */
    char    labels[256]; /* pre-formatted Prometheus label set {k="v",...}   */
    int     type;        /* MetricType: 0=gauge 1=counter 2=histogram        */
    double  value;
    int64_t ts_ms;       /* Unix epoch milliseconds                          */
    int     occupied;
} MetricSlot;

static MetricSlot      s_store[METRIC_STORE_SLOTS];
static int             s_store_count = 0;

/* Pre-rendered Prometheus text page, rebuilt after every batch. */
static char            s_metrics_text[METRICS_BUF_SIZE];
static size_t          s_metrics_len  = 0;

/* Protects s_store, s_metrics_text, s_metrics_len. */
static pthread_mutex_t s_store_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Name sanitiser ──────────────────────────────────────────────────────── */

/* Replace characters invalid in a Prometheus name/label with '_'. */
static void sanitise(const char *src, char *dst, size_t dsz)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dsz - 1; i++) {
        char c = src[i];
        dst[j++] = (c == '.' || c == '-' || c == ' ' || c == '/') ? '_' : c;
    }
    dst[j] = '\0';
}

/* ── FNV-1a hash ─────────────────────────────────────────────────────────── */

static uint32_t fnv1a(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

/* ── Label string builder ────────────────────────────────────────────────── */

/*
 * Build a Prometheus labels string from a TelemetryRecord's metric.
 * Result example: {host="myhost",mode="total"}
 * If no labels:   {host="myhost"}
 * If no host:     {}
 */
static void build_label_str(const TelemetryRecord *r, char *out, size_t outsz)
{
    const Metric *m = &r->metric;
    size_t pos = 0;

    /* Opening brace */
    if (pos < outsz - 1) out[pos++] = '{';

    /* host label (always first) */
    if (r->host_name[0]) {
        int n = snprintf(out + pos, outsz - pos, "host=\"%s\"",
                         r->host_name);
        if (n > 0) pos += (size_t)n;
    }

    /* User-defined labels from the metric */
    for (int i = 0; i < m->label_count; i++) {
        if (pos >= outsz - 4) break;

        char skey[64];
        sanitise(m->label_keys[i], skey, sizeof(skey));

        if (pos < outsz - 2) out[pos++] = ',';

        /* key=" */
        int n = snprintf(out + pos, outsz - pos, "%s=\"", skey);
        if (n > 0) pos += (size_t)n;

        /* escaped value */
        const char *v = m->label_values[i];
        for (; *v && pos < outsz - 4; v++) {
            if (*v == '"' || *v == '\\') {
                out[pos++] = '\\';
            }
            out[pos++] = *v;
        }
        if (pos < outsz - 2) out[pos++] = '"';
    }

    /* Closing brace */
    if (pos < outsz - 1) out[pos++] = '}';
    out[pos] = '\0';
}

/* ── Metric store upsert ─────────────────────────────────────────────────── */

static void metric_store_upsert(const TelemetryRecord *r)
{
    char san_name[128];
    sanitise(r->metric.name, san_name, sizeof(san_name));

    char labels[256];
    build_label_str(r, labels, sizeof(labels));

    /* Build lookup key: name + labels */
    char key[384];
    int klen = snprintf(key, sizeof(key), "%s%s", san_name, labels);
    if (klen <= 0) return;

    uint32_t h    = fnv1a(key, (size_t)klen);
    uint32_t base = h & (METRIC_STORE_SLOTS - 1);

    for (int probe = 0; probe < METRIC_STORE_SLOTS; probe++) {
        uint32_t idx = (base + (uint32_t)probe) & (METRIC_STORE_SLOTS - 1);
        MetricSlot *ms = &s_store[idx];

        if (!ms->occupied) {
            /* Empty slot — insert */
            ms->occupied = 1;
            SAFE_STRNCPY(ms->name,   san_name, sizeof(ms->name));
            SAFE_STRNCPY(ms->labels, labels,   sizeof(ms->labels));
            ms->type  = (int)r->metric.type;
            ms->value = r->metric.value;
            ms->ts_ms = (int64_t)(r->metric.timestamp_ns / 1000000ULL);
            s_store_count++;
            return;
        }

        if (strcmp(ms->name, san_name) == 0
            && strcmp(ms->labels, labels) == 0)
        {
            /* Existing slot — update value and timestamp */
            ms->value = r->metric.value;
            ms->ts_ms = (int64_t)(r->metric.timestamp_ns / 1000000ULL);
            return;
        }
    }

    LOG_WARN("prometheus: metric store full (%d/%d slots used)",
             s_store_count, METRIC_STORE_SLOTS);
}

/* ── Rebuild Prometheus text page ────────────────────────────────────────── */

/*
 * Iterates all occupied metric slots and writes the Prometheus exposition
 * text into s_metrics_text.  Called under s_store_mutex.
 *
 * Format per series:
 *   # TYPE omniagent_<name> gauge|counter|summary
 *   omniagent_<name><labels> <value> <ts_ms>
 */
static void rebuild_metrics_text(void)
{
    char  *buf = s_metrics_text;
    size_t cap = METRICS_BUF_SIZE;
    size_t pos = 0;

#define MPRINT(...) \
    do { \
        int _n = snprintf(buf + pos, cap - pos, __VA_ARGS__); \
        if (_n > 0 && (size_t)_n < cap - pos) pos += (size_t)_n; \
    } while (0)

    for (int i = 0; i < METRIC_STORE_SLOTS; i++) {
        const MetricSlot *ms = &s_store[i];
        if (!ms->occupied) continue;
        if (pos >= cap - 512) break;   /* safety: stop near buffer end */

        const char *type_str = (ms->type == 1) ? "counter" :
                               (ms->type == 2) ? "summary" : "gauge";

        MPRINT("# TYPE omniagent_%s %s\n", ms->name, type_str);
        MPRINT("omniagent_%s%s %.6g %" PRId64 "\n",
               ms->name, ms->labels, ms->value, ms->ts_ms);
    }

#undef MPRINT

    s_metrics_len = pos;
}

/* ── TCP helpers (same pattern as original zstd_exporter) ───────────────── */

static int tcp_connect_to(const char *host, int port)
{
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        LOG_WARN("loki: getaddrinfo(%s:%d) failed", host, port);
        return -1;
    }

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int tcp_send_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ── Loki push ───────────────────────────────────────────────────────────── */

static size_t json_escape_str(const char *src, char *dst, size_t dsz)
{
    size_t pos = 0;
    for (; *src && pos < dsz - 4; src++) {
        unsigned char c = (unsigned char)*src;
        if      (c == '"')  { dst[pos++] = '\\'; dst[pos++] = '"';  }
        else if (c == '\\') { dst[pos++] = '\\'; dst[pos++] = '\\'; }
        else if (c == '\n') { dst[pos++] = '\\'; dst[pos++] = 'n';  }
        else if (c == '\r') { dst[pos++] = '\\'; dst[pos++] = 'r';  }
        else if (c == '\t') { dst[pos++] = '\\'; dst[pos++] = 't';  }
        else if (c < 0x20)  {
            int n = snprintf(dst + pos, dsz - pos, "\\u%04x", (unsigned)c);
            if (n > 0) pos += (size_t)n;
        }
        else { dst[pos++] = (char)c; }
    }
    dst[pos] = '\0';
    return pos;
}

static const char *severity_name(int sev)
{
    if (sev >= 21) return "fatal";
    if (sev >= 17) return "error";
    if (sev >= 13) return "warn";
    if (sev >= 9)  return "info";
    if (sev >= 5)  return "debug";
    if (sev >= 1)  return "trace";
    return "unknown";
}

/*
 * Push a batch of log records to Loki as a single HTTP POST.
 * All records go into one stream keyed by {job="omniagent", host="<hostname>"}.
 * Source and severity are encoded into the log line body.
 */
static void loki_push_logs(TelemetryRecord **logs, size_t count)
{
    if (count == 0) return;
    if (g_config.loki_host[0] == '\0') return;   /* Loki not configured */

    static char body[LOKI_BUF_SIZE];
    size_t pos = 0;

#define LPRINT(...) \
    do { \
        int _n = snprintf(body + pos, sizeof(body) - pos, __VA_ARGS__); \
        if (_n > 0 && (size_t)_n < sizeof(body) - pos) pos += (size_t)_n; \
    } while (0)

    char esc_host[128] = "unknown";
    if (logs[0]->host_name[0]) {
        json_escape_str(logs[0]->host_name, esc_host, sizeof(esc_host));
    }

    LPRINT("{\"streams\":[{\"stream\":{\"job\":\"omniagent\",\"host\":\"%s\"},",
           esc_host);
    LPRINT("\"values\":[");

    for (size_t i = 0; i < count; i++) {
        const LogRecord *l = &logs[i]->log;

        /* Build line: "[level] source: body" */
        char line_raw[2048];
        if (l->source[0]) {
            snprintf(line_raw, sizeof(line_raw), "[%s] %s: %s",
                     severity_name((int)l->severity), l->source, l->body);
        } else {
            snprintf(line_raw, sizeof(line_raw), "[%s] %s",
                     severity_name((int)l->severity), l->body);
        }

        char esc_line[2200];
        json_escape_str(line_raw, esc_line, sizeof(esc_line));

        if (i > 0 && pos < sizeof(body) - 2) body[pos++] = ',';
        LPRINT("[\"%" PRIu64 "\",\"%s\"]", l->timestamp_ns, esc_line);
    }

    LPRINT("]}]}");

#undef LPRINT

    int fd = tcp_connect_to(g_config.loki_host, g_config.loki_port);
    if (fd < 0) {
        LOG_WARN("loki: cannot connect to %s:%d — logs dropped",
                 g_config.loki_host, g_config.loki_port);
        return;
    }

    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "POST /loki/api/v1/push HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        g_config.loki_host, g_config.loki_port, pos);

    if (hlen <= 0
        || tcp_send_all(fd, hdr,  (size_t)hlen) != 0
        || tcp_send_all(fd, body, pos)           != 0)
    {
        LOG_WARN("loki: failed to send %zu log records", count);
        close(fd);
        return;
    }

    /* Drain response to confirm; Loki returns 204 No Content on success. */
    char resp[64] = "";
    recv(fd, resp, sizeof(resp) - 1, 0);
    close(fd);

    if (strncmp(resp, "HTTP/1.1 204", 12) != 0
        && strncmp(resp, "HTTP/1.0 204", 12) != 0)
    {
        LOG_WARN("loki: unexpected response: %.40s", resp);
    } else {
        LOG_DEBUG("loki: pushed %zu log records", count);
    }
}

/* ── prometheus_exporter_thread ──────────────────────────────────────────── */

void *prometheus_exporter_thread(void *arg)
{
    OAGENT_UNUSED(arg);

    LOG_INFO("prometheus_exporter: started (loki=%s:%d metrics_port=%d)",
             g_config.loki_host[0] ? g_config.loki_host : "(disabled)",
             g_config.loki_port,
             g_config.metrics_port);

    /* Reuse a fixed buffer for log pointers per batch (at most 1000). */
    TelemetryRecord *log_batch[BATCH_MAX_RECORDS];

    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {

        ExportBatch batch = export_queue_pop(&g_export_queue);
        if (batch.count == 0) break;   /* empty sentinel → shutdown */

        size_t log_count = 0;

        pthread_mutex_lock(&s_store_mutex);

        for (size_t i = 0; i < batch.count; i++) {
            TelemetryRecord *r = batch.records[i];

            switch (r->type) {
            case TELEM_METRIC:
                metric_store_upsert(r);
                pool_free(&g_pool, r);
                break;

            case TELEM_LOG:
                if (log_count < BATCH_MAX_RECORDS) {
                    log_batch[log_count++] = r;
                } else {
                    pool_free(&g_pool, r);
                }
                break;

            default:
                /* Spans: not forwarded in this mode */
                pool_free(&g_pool, r);
                break;
            }
        }

        rebuild_metrics_text();

        pthread_mutex_unlock(&s_store_mutex);

        /* Push logs to Loki outside the lock (network I/O can block). */
        loki_push_logs(log_batch, log_count);
        for (size_t i = 0; i < log_count; i++) {
            pool_free(&g_pool, log_batch[i]);
        }

        LOG_DEBUG("prometheus_exp: batch done "
                  "(%zu records, %zu logs pushed, store=%d entries)",
                  batch.count, log_count, s_store_count);
    }

    LOG_INFO("prometheus_exporter: stopped");
    return NULL;
}

/* ── prometheus_http_thread ──────────────────────────────────────────────── */

/*
 * Minimal HTTP/1.1 server — single-threaded, serial accept loop.
 * Prometheus scrapes are infrequent (typically every 15 s) so no need
 * for concurrency here.
 */
void *prometheus_http_thread(void *arg)
{
    OAGENT_UNUSED(arg);

    int port = g_config.metrics_port;
    LOG_INFO("prometheus_http: listening on :%d", port);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        LOG_ERROR("prometheus_http: socket: %s", strerror(errno));
        return NULL;
    }

    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_ERROR("prometheus_http: bind(:%d): %s", port, strerror(errno));
        close(srv);
        return NULL;
    }

    if (listen(srv, 8) != 0) {
        LOG_ERROR("prometheus_http: listen: %s", strerror(errno));
        close(srv);
        return NULL;
    }

    /* Static response body buffer — used under s_store_mutex snapshot. */
    static char resp_body[METRICS_BUF_SIZE];

    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {

        /* Timed select so we check g_running every second. */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        if (select(srv + 1, &rfds, NULL, NULL, &tv) <= 0) continue;

        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) continue;

        /* Set a short receive/send timeout so we don't stall. */
        struct timeval cto = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &cto, sizeof(cto));
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &cto, sizeof(cto));

        char req[HTTP_READ_BUF];
        memset(req, 0, sizeof(req));
        recv(cfd, req, sizeof(req) - 1, 0);

        if (strstr(req, "GET /metrics") != NULL) {
            /* Take snapshot of rendered text under lock. */
            pthread_mutex_lock(&s_store_mutex);
            size_t body_len = s_metrics_len;
            memcpy(resp_body, s_metrics_text, body_len);
            pthread_mutex_unlock(&s_store_mutex);

            char hdr[HTTP_RESP_HDR_SIZE];
            int hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n"
                "\r\n",
                body_len);

            if (hlen > 0) {
                send(cfd, hdr,       (size_t)hlen, MSG_NOSIGNAL);
                send(cfd, resp_body, body_len,     MSG_NOSIGNAL);
            }

        } else if (strstr(req, "GET /health") != NULL) {
            const char *ok =
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: 2\r\n"
                "Connection: close\r\n"
                "\r\n"
                "OK";
            send(cfd, ok, strlen(ok), MSG_NOSIGNAL);

        } else {
            const char *nf =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n"
                "\r\n";
            send(cfd, nf, strlen(nf), MSG_NOSIGNAL);
        }

        close(cfd);
    }

    close(srv);
    LOG_INFO("prometheus_http: stopped");
    return NULL;
}
