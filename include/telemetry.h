/*
 * omniagent/include/telemetry.h
 *
 * Core telemetry data model: Metrics, Logs, Traces.
 * Mirrors the OpenTelemetry data model but in lean, fixed-size C structs
 * suitable for a pre-allocated memory pool.
 *
 * Memory budget per TelemetryRecord: ~1.5 KB
 * Pool of 2048 records: ~3.0 MB RSS
 */

#ifndef OMNIAGENT_TELEMETRY_H
#define OMNIAGENT_TELEMETRY_H

#include <stdint.h>
#include <sys/types.h>

/* ── String capacity constants ─────────────────────────────────────────── */
#define OAGENT_NAME_MAX       64    /* metric/span name                     */
#define OAGENT_UNIT_MAX       16    /* metric unit (ms, bytes, %)           */
#define OAGENT_KEY_LEN        40    /* label / attribute key                */
#define OAGENT_VAL_LEN        80    /* label / attribute value              */
#define OAGENT_LOG_BODY_MAX  1024   /* inline log body                      */
#define OAGENT_SOURCE_MAX    128    /* log source file or container name    */
#define OAGENT_RESOURCE_MAX   64    /* service.name / host.name string      */
#define OAGENT_TRACE_LEN      33    /* 32 hex chars + NUL                   */
#define OAGENT_SPAN_LEN       17    /* 16 hex chars + NUL                   */
#define OAGENT_MAX_LABELS      6    /* labels per metric                    */
#define OAGENT_MAX_ATTRS       8    /* attributes per span                  */

/* ── Metric ─────────────────────────────────────────────────────────────── */

typedef enum {
    METRIC_GAUGE     = 0,   /* instantaneous value (e.g. cpu.usage_pct)  */
    METRIC_COUNTER   = 1,   /* monotonically increasing (e.g. req.total) */
    METRIC_HISTOGRAM = 2,   /* distribution (value=sum, count=count)     */
} MetricType;

typedef struct {
    char       name          [OAGENT_NAME_MAX];
    char       unit          [OAGENT_UNIT_MAX];
    MetricType type;
    double     value;                       /* gauge/counter reading        */
    double     sum;                         /* histogram: cumulative sum    */
    uint64_t   count;                       /* histogram: sample count      */
    uint64_t   timestamp_ns;                /* Unix epoch nanoseconds       */
    char       label_keys  [OAGENT_MAX_LABELS][OAGENT_KEY_LEN];
    char       label_values[OAGENT_MAX_LABELS][OAGENT_VAL_LEN];
    int        label_count;
} Metric;
/* sizeof(Metric) ≈ 64+16+4+8+8+8+8+6*(40+80)+4  ≈  804 bytes */

/* ── Log Record ─────────────────────────────────────────────────────────── */

typedef enum {
    LOG_SEVERITY_UNSET = 0,
    LOG_SEVERITY_TRACE = 1,
    LOG_SEVERITY_DEBUG = 5,
    LOG_SEVERITY_INFO  = 9,
    LOG_SEVERITY_WARN  = 13,
    LOG_SEVERITY_ERROR = 17,
    LOG_SEVERITY_FATAL = 21,
} LogSeverity;

typedef struct {
    uint64_t     timestamp_ns;
    LogSeverity  severity;
    char         body    [OAGENT_LOG_BODY_MAX];
    char         source  [OAGENT_SOURCE_MAX];   /* filename or container  */
    char         trace_id[OAGENT_TRACE_LEN];
    char         span_id [OAGENT_SPAN_LEN];
} LogRecord;
/* sizeof(LogRecord) ≈ 8+4+1024+128+33+17  ≈  1214 bytes */

/* ── Trace Span ──────────────────────────────────────────────────────────── */

typedef enum {
    SPAN_STATUS_UNSET = 0,
    SPAN_STATUS_OK    = 1,
    SPAN_STATUS_ERROR = 2,
} SpanStatus;

typedef struct {
    char         trace_id      [OAGENT_TRACE_LEN];
    char         span_id       [OAGENT_SPAN_LEN];
    char         parent_span_id[OAGENT_SPAN_LEN];
    char         name          [OAGENT_NAME_MAX];
    uint64_t     start_time_ns;
    uint64_t     end_time_ns;
    SpanStatus   status;
    char         attr_keys  [OAGENT_MAX_ATTRS][OAGENT_KEY_LEN];
    char         attr_values[OAGENT_MAX_ATTRS][OAGENT_VAL_LEN];
    int          attr_count;
} Span;
/* sizeof(Span) ≈ 33+17+17+64+8+8+4+8*(40+80)+4  ≈  1115 bytes */

/* ── Unified Telemetry Record ────────────────────────────────────────────── */

typedef enum {
    TELEM_METRIC = 0,
    TELEM_LOG    = 1,
    TELEM_SPAN   = 2,
} TelemetryType;

typedef struct TelemetryRecord {
    TelemetryType type;

    union {
        Metric    metric;
        LogRecord log;
        Span      span;
    };

    /* OTel resource attributes — filled by each receiver */
    char     service_name [OAGENT_RESOURCE_MAX];    /* service.name      */
    char     host_name    [OAGENT_RESOURCE_MAX];    /* host.name         */
    char     container_id [OAGENT_RESOURCE_MAX];    /* container.id/name */
    pid_t    pid;                                   /* 0 if not a process */
} TelemetryRecord;
/*
 * sizeof(TelemetryRecord):
 *   4 (type) + max(804,1214,1115) (union) + 64*3 (resources) + 4 (pid)
 *   ≈  4 + 1214 + 192 + 4  =  ~1414 bytes
 *
 * 2048 records × 1414 bytes ≈ 2.9 MB  (well within the 10 MB target)
 */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Returns current Unix time in nanoseconds. */
uint64_t telem_now_ns(void);

/*
 * Serialise an array of TelemetryRecord pointers into a JSON string.
 *
 * The output buffer must be caller-allocated.  Returns the number of bytes
 * written (excluding the NUL terminator) or -1 on overflow.
 *
 * The format is a minimal OTLP-compatible JSON envelope that every major
 * observability backend understands.
 */
int telem_batch_to_json(TelemetryRecord *const *records, size_t count,
                        char *buf, size_t buf_size);

#endif /* OMNIAGENT_TELEMETRY_H */
