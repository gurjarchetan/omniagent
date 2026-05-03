/*
 * omniagent/src/telemetry.c
 *
 * Utility functions for the telemetry data model:
 *   - telem_now_ns()        — high-resolution timestamp
 *   - telem_batch_to_json() — serialise a batch to OTLP-compatible JSON
 *
 * JSON output format (simplified OTLP envelope):
 *
 *   {
 *     "resourceMetrics": [ ... ],   ← populated from TELEM_METRIC records
 *     "resourceLogs":    [ ... ],   ← populated from TELEM_LOG records
 *     "resourceSpans":   [ ... ]    ← populated from TELEM_SPAN records
 *   }
 *
 * We do NOT use an external JSON library.  Instead we use a minimal
 * snprintf-based writer with an overflow guard.  The output is always
 * well-formed UTF-8 JSON (special characters in string values are escaped).
 */

#include "omniagent.h"
#include <inttypes.h>
#include <math.h>   /* isfinite() */

/* ── telem_now_ns ────────────────────────────────────────────────────────── */

uint64_t telem_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        /* Extremely unlikely; fall back to 0 rather than crashing. */
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ── Internal write helpers ──────────────────────────────────────────────── */

/* Cursor into the output buffer.  Returns false if we have overflowed. */
typedef struct {
    char   *buf;
    size_t  cap;
    size_t  pos;
    int     overflow;
} Cursor;

static void cur_init(Cursor *c, char *buf, size_t cap)
{
    c->buf      = buf;
    c->cap      = cap;
    c->pos      = 0;
    c->overflow = 0;
}

__attribute__((format(printf, 2, 3)))
static void cur_printf(Cursor *c, const char *fmt, ...)
{
    if (c->overflow) return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(c->buf + c->pos, c->cap - c->pos, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= c->cap - c->pos) {
        c->overflow = 1;
    } else {
        c->pos += (size_t)n;
    }
}

/*
 * Write a JSON-escaped string.  Escapes: \ " and control characters.
 * Input must be a valid C string (NUL-terminated).
 */
static void cur_json_str(Cursor *c, const char *s)
{
    if (c->overflow) return;
    cur_printf(c, "\"");
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if      (ch == '"')  cur_printf(c, "\\\"");
        else if (ch == '\\') cur_printf(c, "\\\\");
        else if (ch == '\n') cur_printf(c, "\\n");
        else if (ch == '\r') cur_printf(c, "\\r");
        else if (ch == '\t') cur_printf(c, "\\t");
        else if (ch < 0x20)  cur_printf(c, "\\u%04x", (unsigned)ch);
        else                 cur_printf(c, "%c", ch);
    }
    cur_printf(c, "\"");
}

/* ── Write helpers per telemetry type ───────────────────────────────────── */

static void write_metric(Cursor *c, const Metric *m,
                         const char *service, const char *host,
                         const char *container, int first)
{
    if (!first) cur_printf(c, ",");
    cur_printf(c, "{");
    cur_printf(c, "\"name\":");  cur_json_str(c, m->name);
    cur_printf(c, ",\"unit\":"); cur_json_str(c, m->unit);
    cur_printf(c, ",\"type\":%d", (int)m->type);

    if (isfinite(m->value))
        cur_printf(c, ",\"value\":%.6g", m->value);
    if (m->type == METRIC_HISTOGRAM) {
        cur_printf(c, ",\"sum\":%.6g,\"count\":%" PRIu64, m->sum, m->count);
    }
    cur_printf(c, ",\"timestamp_ns\":\"%" PRIu64 "\"", m->timestamp_ns);

    /* Labels */
    if (m->label_count > 0) {
        cur_printf(c, ",\"labels\":{");
        for (int i = 0; i < m->label_count; i++) {
            if (i) cur_printf(c, ",");
            cur_json_str(c, m->label_keys[i]);
            cur_printf(c, ":");
            cur_json_str(c, m->label_values[i]);
        }
        cur_printf(c, "}");
    }

    /* Resource */
    cur_printf(c, ",\"resource\":{\"service.name\":");
    cur_json_str(c, service);
    cur_printf(c, ",\"host.name\":");
    cur_json_str(c, host);
    if (container[0]) {
        cur_printf(c, ",\"container.id\":");
        cur_json_str(c, container);
    }
    cur_printf(c, "}}");
}

static void write_log(Cursor *c, const LogRecord *l,
                      const char *service, const char *host,
                      const char *container, int first)
{
    if (!first) cur_printf(c, ",");
    cur_printf(c, "{");
    cur_printf(c, "\"timestamp_ns\":\"%" PRIu64 "\"", l->timestamp_ns);
    cur_printf(c, ",\"severity_number\":%d", (int)l->severity);
    cur_printf(c, ",\"body\":");         cur_json_str(c, l->body);
    cur_printf(c, ",\"source\":");       cur_json_str(c, l->source);
    if (l->trace_id[0]) {
        cur_printf(c, ",\"trace_id\":"); cur_json_str(c, l->trace_id);
    }
    if (l->span_id[0]) {
        cur_printf(c, ",\"span_id\":");  cur_json_str(c, l->span_id);
    }
    cur_printf(c, ",\"resource\":{\"service.name\":");
    cur_json_str(c, service);
    cur_printf(c, ",\"host.name\":");    cur_json_str(c, host);
    if (container[0]) {
        cur_printf(c, ",\"container.id\":");
        cur_json_str(c, container);
    }
    cur_printf(c, "}}");
}

static void write_span(Cursor *c, const Span *s,
                       const char *service, const char *host,
                       const char *container, int first)
{
    if (!first) cur_printf(c, ",");
    cur_printf(c, "{");
    cur_printf(c, "\"trace_id\":");        cur_json_str(c, s->trace_id);
    cur_printf(c, ",\"span_id\":");        cur_json_str(c, s->span_id);
    if (s->parent_span_id[0]) {
        cur_printf(c, ",\"parent_span_id\":"); cur_json_str(c, s->parent_span_id);
    }
    cur_printf(c, ",\"name\":");           cur_json_str(c, s->name);
    cur_printf(c, ",\"start_time_ns\":\"%" PRIu64 "\"", s->start_time_ns);
    cur_printf(c, ",\"end_time_ns\":\"%" PRIu64 "\"",   s->end_time_ns);
    cur_printf(c, ",\"status\":%d", (int)s->status);

    if (s->attr_count > 0) {
        cur_printf(c, ",\"attributes\":{");
        for (int i = 0; i < s->attr_count; i++) {
            if (i) cur_printf(c, ",");
            cur_json_str(c, s->attr_keys[i]);
            cur_printf(c, ":");
            cur_json_str(c, s->attr_values[i]);
        }
        cur_printf(c, "}");
    }

    cur_printf(c, ",\"resource\":{\"service.name\":");
    cur_json_str(c, service);
    cur_printf(c, ",\"host.name\":");      cur_json_str(c, host);
    if (container[0]) {
        cur_printf(c, ",\"container.id\":");
        cur_json_str(c, container);
    }
    cur_printf(c, "}}");
}

/* ── telem_batch_to_json ─────────────────────────────────────────────────── */

int telem_batch_to_json(TelemetryRecord *const *records, size_t count,
                        char *buf, size_t buf_size)
{
    Cursor c;
    cur_init(&c, buf, buf_size);

    cur_printf(&c, "{");

    /* ── Metrics ── */
    cur_printf(&c, "\"resourceMetrics\":[");
    int first_m = 1;
    for (size_t i = 0; i < count; i++) {
        const TelemetryRecord *r = records[i];
        if (r->type != TELEM_METRIC) continue;
        write_metric(&c, &r->metric,
                     r->service_name, r->host_name, r->container_id,
                     first_m);
        first_m = 0;
    }
    cur_printf(&c, "]");

    /* ── Logs ── */
    cur_printf(&c, ",\"resourceLogs\":[");
    int first_l = 1;
    for (size_t i = 0; i < count; i++) {
        const TelemetryRecord *r = records[i];
        if (r->type != TELEM_LOG) continue;
        write_log(&c, &r->log,
                  r->service_name, r->host_name, r->container_id,
                  first_l);
        first_l = 0;
    }
    cur_printf(&c, "]");

    /* ── Spans ── */
    cur_printf(&c, ",\"resourceSpans\":[");
    int first_s = 1;
    for (size_t i = 0; i < count; i++) {
        const TelemetryRecord *r = records[i];
        if (r->type != TELEM_SPAN) continue;
        write_span(&c, &r->span,
                   r->service_name, r->host_name, r->container_id,
                   first_s);
        first_s = 0;
    }
    cur_printf(&c, "]");

    cur_printf(&c, "}");

    if (c.overflow) {
        return -1;
    }
    return (int)c.pos;
}
