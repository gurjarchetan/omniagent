/*
 * omniagent/src/zstd_exporter.c
 *
 * Zstd Exporter — serialises batches to JSON, compresses with Zstandard,
 * and HTTP POSTs the compressed payload to the configured remote backend.
 *
 * Data path per batch:
 *   ExportBatch (TelemetryRecord*[])
 *     → telem_batch_to_json()          → JSON string (~64 KB typical)
 *     → ZSTD_compress()               → compressed bytes (~5–20 KB typical)
 *     → HTTP POST (raw TCP socket)    → remote OTel Collector / Datadog
 *     → pool_free() all records       → records returned to pool
 *
 * HTTP POST format:
 *   POST /v1/batch HTTP/1.1
 *   Host: <endpoint_host>
 *   Content-Type: application/json
 *   Content-Encoding: zstd
 *   Content-Length: <compressed_size>
 *   Authorization: Bearer <auth_token>  ← only if token is configured
 *
 * Retry policy:
 *   Up to EXPORT_RETRY_MAX (3) attempts per batch with exponential backoff
 *   (1 s, 2 s, 4 s).  On permanent failure the batch is dropped and records
 *   are freed.
 *
 * Compression:
 *   We use ZSTD_compress() (one-shot, not streaming) because batches fit in
 *   memory.  Level 3 gives a good speed/ratio trade-off (~3–5× compression)
 *   while remaining well under 1 ms per batch on modern hardware.
 *
 * Memory:
 *   JSON buffer:        8 MB (worst-case 1000 × ~8KB log records)
 *   Compressed buffer:  ZSTD_compressBound(8 MB) ≈ 8.1 MB (worst case pre-alloc)
 *   These buffers are allocated once at thread startup and reused.
 */

#include "omniagent.h"
#include <zstd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ── JSON buffer sizes ───────────────────────────────────────────────────── */

/* 1000 records × worst-case ~8 KB each = 8 MB.  In practice <1 MB. */
#define JSON_BUF_SIZE   (8 * 1024 * 1024)

/* ── TCP connect to export endpoint ─────────────────────────────────────── */

static int tcp_connect(const char *host, int port)
{
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;    /* IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        LOG_WARN("exporter: getaddrinfo(%s:%d): %s",
                 host, port, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        /* Connection timeout: 5 seconds */
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;  /* success */
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd < 0) {
        LOG_WARN("exporter: cannot connect to %s:%d", host, port);
    }

    return fd;
}

/* ── Send entire buffer over TCP (handles partial writes) ────────────────── */

static int tcp_send_all(int fd, const void *data, size_t size)
{
    const char *ptr = (const char *)data;
    size_t      remaining = size;

    while (remaining > 0) {
        ssize_t n = send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (n <= 0) {
            LOG_WARN("exporter: send: %s", strerror(errno));
            return -1;
        }
        ptr       += n;
        remaining -= (size_t)n;
    }
    return 0;
}

/* ── Read HTTP response status line ──────────────────────────────────────── */

static int read_http_status(int fd)
{
    char resp[256] = "";
    size_t pos = 0;

    while (pos < sizeof(resp) - 1) {
        ssize_t n = recv(fd, resp + pos, 1, 0);
        if (n <= 0) break;
        pos++;
        if (pos >= 2 && resp[pos-2] == '\r' && resp[pos-1] == '\n') break;
    }
    resp[pos] = '\0';

    /* "HTTP/1.1 200 OK\r\n" */
    int status_code = 0;
    sscanf(resp, "%*s %d", &status_code);
    return status_code;
}

/* ── Drain remainder of HTTP response ────────────────────────────────────── */

static void drain_response(int fd)
{
    char drain_buf[4096];
    /* Read until connection closes or we've drained enough */
    for (int i = 0; i < 32; i++) {
        ssize_t n = recv(fd, drain_buf, sizeof(drain_buf), MSG_DONTWAIT);
        if (n <= 0) break;
    }
}

/* ── Export one batch ────────────────────────────────────────────────────── */

static int export_batch(const ExportBatch *batch,
                        char *json_buf, size_t json_buf_size,
                        void *zstd_buf, size_t zstd_buf_size)
{
    /* ── Serialise to JSON ── */
    int json_len = telem_batch_to_json(
                       (TelemetryRecord *const *)batch->records,
                       batch->count,
                       json_buf, json_buf_size);

    if (json_len < 0) {
        LOG_WARN("exporter: JSON serialisation overflowed (batch=%zu records)",
                 batch->count);
        return -1;
    }

    LOG_DEBUG("exporter: JSON serialised %zu records → %d bytes",
              batch->count, json_len);

    /* ── Compress with Zstd ── */
    size_t compressed_size = ZSTD_compress(
                                 zstd_buf, zstd_buf_size,
                                 json_buf, (size_t)json_len,
                                 g_config.zstd_level);

    if (ZSTD_isError(compressed_size)) {
        LOG_WARN("exporter: ZSTD_compress failed: %s",
                 ZSTD_getErrorName(compressed_size));
        return -1;
    }

    double ratio = (double)json_len / (double)compressed_size;
    LOG_DEBUG("exporter: compressed %d → %zu bytes (%.1f× ratio)",
              json_len, compressed_size, ratio);

    /* ── Build HTTP POST request ── */
    char headers[1024];
    int hdr_len = snprintf(headers, sizeof(headers),
        "POST /v1/batch HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Encoding: zstd\r\n"
        "Content-Length: %zu\r\n"
        "User-Agent: OmniAgent/1.0\r\n"
        "%s%s%s"
        "Connection: close\r\n"
        "\r\n",
        g_config.endpoint_host,
        compressed_size,
        g_config.auth_token[0] ? "Authorization: Bearer " : "",
        g_config.auth_token[0] ? g_config.auth_token      : "",
        g_config.auth_token[0] ? "\r\n"                   : ""
    );

    if (hdr_len < 0 || (size_t)hdr_len >= sizeof(headers)) {
        LOG_ERROR("exporter: HTTP headers too large");
        return -1;
    }

    /* ── Connect and send ── */
    int fd = tcp_connect(g_config.endpoint_host, g_config.endpoint_port);
    if (fd < 0) return -1;

    int ret = 0;

    if (tcp_send_all(fd, headers, (size_t)hdr_len) != 0) {
        ret = -1;
        goto done;
    }

    if (tcp_send_all(fd, zstd_buf, compressed_size) != 0) {
        ret = -1;
        goto done;
    }

    /* Read response status */
    int status = read_http_status(fd);
    drain_response(fd);

    if (status >= 200 && status < 300) {
        LOG_DEBUG("exporter: HTTP %d — export successful", status);
        ret = 0;
    } else {
        LOG_WARN("exporter: HTTP %d — server rejected batch (will retry)",
                 status);
        ret = -1;
    }

done:
    close(fd);
    return ret;
}

/* ── Zstd Exporter thread ────────────────────────────────────────────────── */

void *zstd_exporter_thread(void *arg)
{
    OAGENT_UNUSED(arg);

    /* Allocate I/O buffers once at startup — reused across all batches */
    char  *json_buf  = malloc(JSON_BUF_SIZE);
    size_t zstd_bound = ZSTD_compressBound(JSON_BUF_SIZE);
    void  *zstd_buf  = malloc(zstd_bound);

    if (!json_buf || !zstd_buf) {
        LOG_ERROR("exporter: cannot allocate I/O buffers (json=%zuMB zstd=%zuMB)",
                  (size_t)(JSON_BUF_SIZE / 1024 / 1024),
                  (size_t)(zstd_bound    / 1024 / 1024));
        free(json_buf);
        free(zstd_buf);
        return NULL;
    }

    LOG_INFO("zstd_exporter: started (endpoint=%s:%d level=%d)",
             g_config.endpoint_host, g_config.endpoint_port,
             g_config.zstd_level);

    uint64_t total_exported = 0;
    uint64_t total_dropped  = 0;

    while (atomic_load_explicit(&g_running, memory_order_relaxed)
           || g_export_queue.size > 0)
    {
        ExportBatch batch = export_queue_pop(&g_export_queue);

        if (batch.count == 0) {
            /* Queue was empty and we're shutting down */
            break;
        }

        /* ── Retry loop ── */
        int success = 0;
        for (int attempt = 0; attempt < EXPORT_RETRY_MAX; attempt++) {
            if (attempt > 0) {
                /* Exponential backoff: 1s, 2s, 4s */
                unsigned int backoff = 1u << (unsigned)(attempt - 1);
                LOG_WARN("exporter: retry %d/%d for batch of %zu records "
                         "(backoff=%us)",
                         attempt, EXPORT_RETRY_MAX, batch.count, backoff);
                sleep(backoff);

                /* Check if shutdown was requested during backoff */
                if (!atomic_load_explicit(&g_running, memory_order_relaxed)) {
                    break;
                }
            }

            if (export_batch(&batch, json_buf, JSON_BUF_SIZE,
                             zstd_buf, zstd_bound) == 0)
            {
                success = 1;
                break;
            }
        }

        if (success) {
            total_exported += batch.count;
        } else {
            LOG_WARN("exporter: batch of %zu records permanently failed "
                     "(total_dropped=%" PRIu64 ")",
                     batch.count, total_dropped + batch.count);
            total_dropped += batch.count;
        }

        /* Return all records to the pool regardless of export outcome */
        for (size_t i = 0; i < batch.count; i++) {
            pool_free(&g_pool, batch.records[i]);
        }
    }

    free(json_buf);
    free(zstd_buf);

    LOG_INFO("zstd_exporter: exiting "
             "(total_exported=%" PRIu64 " total_dropped=%" PRIu64 ")",
             total_exported, total_dropped);
    return NULL;
}
