/*
 * omniagent/src/otlp_http_receiver.c
 *
 * OTLP HTTP Receiver — listens on TCP port 4318 for incoming
 * OpenTelemetry JSON payloads sent by application SDKs or other
 * OTel collectors.
 *
 * Implements a minimal HTTP/1.1 server using raw POSIX sockets:
 *   POST /v1/metrics      → parse + ingest as Metric records
 *   POST /v1/logs         → parse + ingest as LogRecord records
 *   POST /v1/traces       → parse + ingest as Span records
 *   GET  /healthz         → returns "200 OK" for load balancer health checks
 *
 * We do NOT use libmicrohttpd or any external library to keep the binary
 * dependency list to just {libc, libpthread, libzstd}.
 *
 * Concurrency model: single-threaded accept loop with one request processed
 * at a time.  This is intentional — OTLP senders typically batch aggressively
 * (1 request per 5 s) so throughput is not the concern; latency spikes are.
 * A single thread avoids the overhead of a thread pool for low-rate inputs.
 *
 * For the payload body we do a best-effort parse.  We do not implement a
 * complete OTLP JSON schema parser; instead we look for the salient fields
 * (service name, metric names, log bodies, trace IDs) using the same
 * hand-rolled JSON scanner used in docker_receiver.c.
 *
 * Security considerations:
 *   • Content-Length is enforced against MAX_BODY_SIZE to prevent DoS.
 *   • The server binds to 127.0.0.1 only (localhost) by default.
 *     To accept remote OTLP, the caller must change BIND_ADDR.
 *   • No authentication is implemented in this receiver; that is handled
 *     by the export layer (outbound auth token).
 */

#include "omniagent.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

/* ── Constants ───────────────────────────────────────────────────────────── */

#define BIND_ADDR       "127.0.0.1"     /* loopback only — change for LAN  */
#define LISTEN_BACKLOG  16
#define MAX_BODY_SIZE   (4 * 1024 * 1024)  /* 4 MB max request body        */
#define REQ_HDR_SIZE    (8 * 1024)         /* 8 KB for request headers      */
#define RECV_TIMEOUT_S  5               /* per-connection read timeout       */

/* ── HTTP response helpers ───────────────────────────────────────────────── */

static void http_respond(int fd, int status, const char *status_text,
                         const char *body)
{
    char resp[512];
    size_t body_len = body ? strlen(body) : 0;
    int n = snprintf(resp, sizeof(resp),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, body_len);

    if (n > 0 && (size_t)n < sizeof(resp)) {
        /* Best-effort write — ignore partial write on a closing connection */
        if (write(fd, resp, (size_t)n) < 0) goto write_done;
        if (body && body_len > 0) {
            if (write(fd, body, body_len) < 0) goto write_done;
        }
    }
write_done:;
}

/* ── Read the full request into buf (headers + body) ─────────────────────── */

/*
 * Reads from fd until we have a complete HTTP request.
 * Returns the total bytes read, or -1 on error / timeout.
 *
 * We detect the end of headers at \r\n\r\n, parse Content-Length,
 * then read the remaining body bytes.
 */
static ssize_t read_http_request(int fd, char *buf, size_t buf_size,
                                 size_t *body_offset_out)
{
    struct timeval tv = { .tv_sec = RECV_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t total = 0;
    char  *header_end = NULL;

    /* Phase 1: read until we see \r\n\r\n */
    while (total < buf_size - 1) {
        ssize_t n = recv(fd, buf + total, buf_size - total - 1, 0);
        if (n <= 0) {
            if (n == 0) break;  /* connection closed */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                LOG_WARN("otlp_http: read timeout on fd %d", fd);
                return -1;
            }
            return -1;
        }
        total += (size_t)n;
        buf[total] = '\0';

        header_end = strstr(buf, "\r\n\r\n");
        if (header_end) break;
    }

    if (!header_end) {
        LOG_WARN("otlp_http: no header separator found");
        return -1;
    }

    size_t hdr_size = (size_t)(header_end - buf) + 4;  /* 4 = len("\r\n\r\n") */
    *body_offset_out = hdr_size;

    /* Parse Content-Length */
    long content_length = 0;
    const char *cl = strstr(buf, "Content-Length:");
    if (!cl) cl = strstr(buf, "content-length:");
    if (cl) {
        content_length = strtol(cl + 15, NULL, 10);
    }

    if (content_length < 0 || content_length > MAX_BODY_SIZE) {
        LOG_WARN("otlp_http: Content-Length %ld out of bounds", content_length);
        return -1;
    }

    size_t body_needed = (size_t)content_length;
    size_t body_have   = total - hdr_size;

    /* Phase 2: read remaining body bytes */
    while (body_have < body_needed && total < buf_size - 1) {
        ssize_t n = recv(fd, buf + total,
                         buf_size - total - 1, 0);
        if (n <= 0) break;
        total     += (size_t)n;
        body_have += (size_t)n;
        buf[total] = '\0';
    }

    return (ssize_t)total;
}

/* ── Parse request line ──────────────────────────────────────────────────── */

typedef enum {
    OTLP_PATH_METRICS = 0,
    OTLP_PATH_LOGS,
    OTLP_PATH_TRACES,
    OTLP_PATH_HEALTH,
    OTLP_PATH_UNKNOWN,
} OtlpPath;

static OtlpPath parse_request_line(const char *buf,
                                   char *method_out, size_t method_size)
{
    char method[16] = "";
    char path[256]  = "";

    /* "POST /v1/metrics HTTP/1.1\r\n" */
    sscanf(buf, "%15s %255s", method, path);
    SAFE_STRNCPY(method_out, method, method_size);

    if (strncmp(path, "/v1/metrics", 11) == 0) return OTLP_PATH_METRICS;
    if (strncmp(path, "/v1/logs",     8) == 0) return OTLP_PATH_LOGS;
    if (strncmp(path, "/v1/traces",  10) == 0) return OTLP_PATH_TRACES;
    if (strncmp(path, "/healthz",     8) == 0) return OTLP_PATH_HEALTH;
    return OTLP_PATH_UNKNOWN;
}

/* ── Minimal OTLP JSON helpers ───────────────────────────────────────────── */

/* Identical to the one in docker_receiver.c — shared logic kept inline to
 * avoid a separate translation unit for a 30-line function. */
static const char *otlp_find_str(const char *p, const char *key,
                                 char *out, size_t out_size)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(p, pattern);
    if (!pos) return NULL;
    pos += strlen(pattern);
    while (*pos == ' ' || *pos == ':' || *pos == '\t') pos++;
    if (*pos != '"') return NULL;
    pos++;
    size_t i = 0;
    while (*pos && *pos != '"' && i < out_size - 1) {
        if (*pos == '\\') { pos++; out[i++] = *pos ? *pos : '\\'; }
        else               out[i++] = *pos;
        pos++;
    }
    out[i] = '\0';
    if (*pos == '"') pos++;
    return pos;
}

/* ── Ingest OTLP Metrics JSON ────────────────────────────────────────────── */

static void ingest_metrics(const char *json)
{
    /*
     * We scan for metric names and their data point values.
     * OTLP/JSON format:
     *   { "resourceMetrics": [{ "resource": {...}, "scopeMetrics":
     *     [{ "metrics": [{ "name": "...", "gauge": { "dataPoints":
     *        [{ "asDouble": 1.23, "timeUnixNano": "..." }] } }] }] }] }
     *
     * We iterate over "name" keys and read the nearest "asDouble" value.
     */
    const char *p = json;

    /* Extract service name from resource attributes */
    char service_name[OAGENT_RESOURCE_MAX] = "";
    otlp_find_str(json, "stringValue", service_name, sizeof(service_name));

    while ((p = strstr(p, "\"name\"")) != NULL) {
        char name[OAGENT_NAME_MAX] = "";
        const char *after = otlp_find_str(p, "name", name, sizeof(name));
        if (!after || name[0] == '\0') { p++; continue; }

        /* Look for the numeric value nearby (within 512 bytes) */
        double val = 0.0;
        char   val_str[64] = "";
        const char *vp = strstr(after, "\"asDouble\"");
        if (!vp || vp > after + 512) {
            vp = strstr(after, "\"asInt\"");
        }
        if (vp && vp < after + 512) {
            const char *vafter = otlp_find_str(vp, "asDouble", val_str,
                                               sizeof(val_str));
            if (!vafter || val_str[0] == '\0') {
                /* asInt is quoted in some SDKs */
                otlp_find_str(vp, "asInt", val_str, sizeof(val_str));
            }
            if (val_str[0]) val = strtod(val_str, NULL);
        }

        /* Extract timeUnixNano */
        uint64_t ts_ns = telem_now_ns();
        char ts_str[32] = "";
        if (strstr(after, "\"timeUnixNano\"")) {
            otlp_find_str(after, "timeUnixNano", ts_str, sizeof(ts_str));
            if (ts_str[0]) ts_ns = strtoull(ts_str, NULL, 10);
        }

        TelemetryRecord *rec = pool_alloc(&g_pool);
        if (!rec) { p = after; continue; }

        rec->type = TELEM_METRIC;
        SAFE_STRNCPY(rec->metric.name, name, OAGENT_NAME_MAX);
        rec->metric.type         = METRIC_GAUGE;
        rec->metric.value        = val;
        rec->metric.timestamp_ns = ts_ns;

        if (service_name[0])
            SAFE_STRNCPY(rec->service_name, service_name, OAGENT_RESOURCE_MAX);

        char hostname[64] = "unknown";
        gethostname(hostname, sizeof(hostname) - 1);
        SAFE_STRNCPY(rec->host_name, hostname, OAGENT_RESOURCE_MAX);

        if (!rb_enqueue(&g_recv_queue, rec)) pool_free(&g_pool, rec);

        p = after;
    }
}

/* ── Ingest OTLP Logs JSON ───────────────────────────────────────────────── */

static void ingest_logs(const char *json)
{
    const char *p = json;
    char service_name[OAGENT_RESOURCE_MAX] = "";
    otlp_find_str(json, "stringValue", service_name, sizeof(service_name));

    /* Each log record has a "body" → { "stringValue": "..." } */
    while ((p = strstr(p, "\"body\"")) != NULL) {
        const char *body_pos = p + 6;

        char body[OAGENT_LOG_BODY_MAX] = "";
        otlp_find_str(body_pos, "stringValue", body, sizeof(body));
        if (body[0] == '\0') { p++; continue; }

        char sev_str[16] = "";
        long sev_num = LOG_SEVERITY_INFO;
        otlp_find_str(p, "severityNumber", sev_str, sizeof(sev_str));
        if (sev_str[0]) sev_num = strtol(sev_str, NULL, 10);

        char trace_id[OAGENT_TRACE_LEN] = "";
        char span_id [OAGENT_SPAN_LEN]  = "";
        otlp_find_str(p, "traceId",  trace_id, sizeof(trace_id));
        otlp_find_str(p, "spanId",   span_id,  sizeof(span_id));

        TelemetryRecord *rec = pool_alloc(&g_pool);
        if (!rec) { p++; continue; }

        rec->type = TELEM_LOG;
        rec->log.timestamp_ns = telem_now_ns();
        rec->log.severity     = (LogSeverity)sev_num;
        SAFE_STRNCPY(rec->log.body,     body,     OAGENT_LOG_BODY_MAX);
        SAFE_STRNCPY(rec->log.source,   "otlp",   OAGENT_SOURCE_MAX);
        SAFE_STRNCPY(rec->log.trace_id, trace_id, OAGENT_TRACE_LEN);
        SAFE_STRNCPY(rec->log.span_id,  span_id,  OAGENT_SPAN_LEN);

        if (service_name[0])
            SAFE_STRNCPY(rec->service_name, service_name, OAGENT_RESOURCE_MAX);

        char hostname[64] = "unknown";
        gethostname(hostname, sizeof(hostname) - 1);
        SAFE_STRNCPY(rec->host_name, hostname, OAGENT_RESOURCE_MAX);

        if (!rb_enqueue(&g_recv_queue, rec)) pool_free(&g_pool, rec);

        p++;
    }
}

/* ── Ingest OTLP Traces JSON ─────────────────────────────────────────────── */

static void ingest_traces(const char *json)
{
    const char *p = json;
    char service_name[OAGENT_RESOURCE_MAX] = "";
    otlp_find_str(json, "stringValue", service_name, sizeof(service_name));

    while ((p = strstr(p, "\"spanId\"")) != NULL) {
        char span_id  [OAGENT_SPAN_LEN]  = "";
        char trace_id [OAGENT_TRACE_LEN] = "";
        char name     [OAGENT_NAME_MAX]  = "";
        char start_str[32] = "";
        char end_str  [32] = "";
        char par_span [OAGENT_SPAN_LEN]  = "";

        const char *after = otlp_find_str(p, "spanId",   span_id,   sizeof(span_id));
        if (!after || span_id[0] == '\0') { p++; continue; }

        /* Look back a little for traceId */
        const char *look_start = (p > json + 512) ? p - 512 : json;
        otlp_find_str(look_start, "traceId",        trace_id, sizeof(trace_id));
        otlp_find_str(look_start, "parentSpanId",   par_span, sizeof(par_span));
        otlp_find_str(after,      "name",           name,     sizeof(name));
        otlp_find_str(after,      "startTimeUnixNano", start_str, sizeof(start_str));
        otlp_find_str(after,      "endTimeUnixNano",   end_str,   sizeof(end_str));

        TelemetryRecord *rec = pool_alloc(&g_pool);
        if (!rec) { p = after; continue; }

        rec->type = TELEM_SPAN;
        SAFE_STRNCPY(rec->span.trace_id,       trace_id, OAGENT_TRACE_LEN);
        SAFE_STRNCPY(rec->span.span_id,        span_id,  OAGENT_SPAN_LEN);
        SAFE_STRNCPY(rec->span.parent_span_id, par_span, OAGENT_SPAN_LEN);
        SAFE_STRNCPY(rec->span.name,           name,     OAGENT_NAME_MAX);
        rec->span.start_time_ns = start_str[0] ? strtoull(start_str, NULL, 10) : 0;
        rec->span.end_time_ns   = end_str[0]   ? strtoull(end_str,   NULL, 10) : 0;

        if (service_name[0])
            SAFE_STRNCPY(rec->service_name, service_name, OAGENT_RESOURCE_MAX);

        char hostname[64] = "unknown";
        gethostname(hostname, sizeof(hostname) - 1);
        SAFE_STRNCPY(rec->host_name, hostname, OAGENT_RESOURCE_MAX);

        if (!rb_enqueue(&g_recv_queue, rec)) pool_free(&g_pool, rec);

        p = after;
    }
}

/* ── Handle one client connection ────────────────────────────────────────── */

static void handle_client(int client_fd)
{
    /* Allocate request buffer on the heap — REQ_HDR_SIZE + MAX_BODY_SIZE */
    size_t buf_size = REQ_HDR_SIZE + MAX_BODY_SIZE;
    char  *buf      = malloc(buf_size);
    if (!buf) {
        http_respond(client_fd, 503, "Service Unavailable",
                     "{\"error\":\"out of memory\"}");
        close(client_fd);
        return;
    }

    size_t  body_offset = 0;
    ssize_t total = read_http_request(client_fd, buf, buf_size, &body_offset);

    if (total <= 0) {
        free(buf);
        close(client_fd);
        return;
    }

    char    method[16]  = "";
    OtlpPath path_type  = parse_request_line(buf, method, sizeof(method));

    /* Health check: quick response, no body parsing */
    if (path_type == OTLP_PATH_HEALTH) {
        http_respond(client_fd, 200, "OK", "{\"status\":\"ok\"}");
        free(buf);
        close(client_fd);
        return;
    }

    /* Only accept POST for data paths */
    if (strcmp(method, "POST") != 0) {
        http_respond(client_fd, 405, "Method Not Allowed",
                     "{\"error\":\"use POST\"}");
        free(buf);
        close(client_fd);
        return;
    }

    if (path_type == OTLP_PATH_UNKNOWN) {
        http_respond(client_fd, 404, "Not Found",
                     "{\"error\":\"unknown path\"}");
        free(buf);
        close(client_fd);
        return;
    }

    const char *body = buf + body_offset;

    switch (path_type) {
    case OTLP_PATH_METRICS: ingest_metrics(body); break;
    case OTLP_PATH_LOGS:    ingest_logs(body);    break;
    case OTLP_PATH_TRACES:  ingest_traces(body);  break;
    default: break;
    }

    http_respond(client_fd, 200, "OK", "{}");
    free(buf);
    close(client_fd);
}

/* ── Receiver thread ─────────────────────────────────────────────────────── */

void *otlp_http_receiver_thread(void *arg)
{
    OAGENT_UNUSED(arg);

    /* Create TCP listening socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        LOG_ERROR("otlp_http: socket: %s", strerror(errno));
        return NULL;
    }

    /* Allow quick port reuse after restart */
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)OTLP_HTTP_PORT);
    inet_pton(AF_INET, BIND_ADDR, &addr.sin_addr);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_ERROR("otlp_http: bind(%s:%d): %s",
                  BIND_ADDR, OTLP_HTTP_PORT, strerror(errno));
        close(listen_fd);
        return NULL;
    }

    if (listen(listen_fd, LISTEN_BACKLOG) != 0) {
        LOG_ERROR("otlp_http: listen: %s", strerror(errno));
        close(listen_fd);
        return NULL;
    }

    /* Set non-blocking so we can poll for shutdown */
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    LOG_INFO("otlp_http_receiver: listening on %s:%d",
             BIND_ADDR, OTLP_HTTP_PORT);

    struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };

    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        int ready = poll(&pfd, 1, 500 /* ms */);
        if (ready < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("otlp_http: poll: %s", strerror(errno));
            break;
        }
        if (ready == 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        struct sockaddr_in client_addr;
        socklen_t          client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd,
                               (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            LOG_WARN("otlp_http: accept: %s", strerror(errno));
            continue;
        }

        LOG_DEBUG("otlp_http: connection from %s",
                  inet_ntoa(client_addr.sin_addr));

        handle_client(client_fd);
    }

    close(listen_fd);
    LOG_INFO("otlp_http_receiver: exiting");
    return NULL;
}
