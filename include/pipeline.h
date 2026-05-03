/*
 * omniagent/include/pipeline.h
 *
 * Pipeline configuration and shared agent state.
 *
 * All receiver threads share a single RecordPool and a single recv_queue
 * (RingBuffer).  The batch processor drains recv_queue, applies processors,
 * and fills a Batch which is then handed to the exporter.
 */

#ifndef OMNIAGENT_PIPELINE_H
#define OMNIAGENT_PIPELINE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

#include "ring_buffer.h"
#include "pool.h"

/* ── Compile-time pipeline constants ────────────────────────────────────── */

#define BATCH_MAX_RECORDS   1000U   /* flush batch when this many collected */
#define BATCH_MAX_SECONDS      5    /* flush batch after this many seconds  */
#define MEMORY_LIMIT_MB       64    /* drop records above this RSS (MB)     */
#define EXPORT_RETRY_MAX       3    /* HTTP POST retry attempts             */
#define OTLP_HTTP_PORT      4318    /* TCP port for OTLP/HTTP receiver      */

/* ── Agent configuration (parsed from argv) ─────────────────────────────── */

typedef struct {
    /* Loki endpoint for log push (POST /loki/api/v1/push)                  */
    char    loki_host[256];     /* default: localhost                       */
    int     loki_port;          /* default: 3100                            */

    /* Prometheus metrics HTTP server port (GET /metrics)                   */
    int     metrics_port;       /* default: 9100                            */

    /* Additional log directories to watch via inotify (colon-separated)   */
    char    extra_log_dirs[1024];

    /* Whether to enable each receiver (auto-detected at startup)           */
    int     enable_procfs;
    int     enable_docker;
    int     enable_inotify;
    int     enable_otlp_http;

    /* Scrape interval for procfs receiver (seconds, default 5)             */
    int     procfs_interval_s;

    /* Feature toggles (env: OAGENT_ENABLE_*), default 1 = enabled         */
    int     enable_metrics;     /* procfs + docker stats + prometheus       */
    int     enable_logs;        /* inotify log file collection → Loki       */
    int     enable_otlp;        /* OTLP/HTTP receiver on :4318              */

    /* Log file name filters (fnmatch patterns, comma-separated)            */
    char    log_include[512];   /* OAGENT_LOG_INCLUDE: empty = all files    */
    char    log_exclude[512];   /* OAGENT_LOG_EXCLUDE: e.g. *.gz,*.bz2     */
} AgentConfig;

/* ── Batch structure handed from batch_processor to exporter ────────────── */

#define EXPORT_BATCH_QUEUE_DEPTH  8   /* max batches queued for export      */

typedef struct {
    TelemetryRecord *records[BATCH_MAX_RECORDS];
    size_t           count;
} ExportBatch;

typedef struct {
    ExportBatch      batches[EXPORT_BATCH_QUEUE_DEPTH];
    size_t           head;          /* next slot to write (batch_processor) */
    size_t           tail;          /* next slot to read  (exporter)        */
    size_t           size;          /* items currently queued               */
    pthread_mutex_t  mutex;
    pthread_cond_t   not_empty;
    pthread_cond_t   not_full;
} ExportQueue;

/* ── Global pipeline state ───────────────────────────────────────────────── */
/*
 * Declared here, defined in pipeline.c.
 * All source files that #include "pipeline.h" share the same objects.
 */

extern AgentConfig   g_config;
extern RecordPool    g_pool;
extern RingBuffer    g_recv_queue;
extern ExportQueue   g_export_queue;
extern _Atomic int   g_running;     /* set to 0 by SIGTERM/SIGINT handler  */

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Initialise all pipeline objects.  Must be called before spawning threads. */
int pipeline_init(void);

/* Graceful shutdown: signal all threads to stop and join them. */
void pipeline_shutdown(pthread_t *threads, size_t n_threads);

/* Push a completed batch into the export queue (blocks if queue is full). */
void export_queue_push(ExportQueue *q, const ExportBatch *batch);

/* Pop a batch from the export queue (blocks until one is available). */
ExportBatch export_queue_pop(ExportQueue *q);

/* ── Memory limiter API (implemented in memory_limiter.c) ─────────────── */

/* Returns 1 to pass the record, 0 to drop it (calls pool_free internally). */
int      memory_limiter_check(TelemetryRecord *record);
uint64_t memory_limiter_drop_count(void);
uint64_t memory_limiter_rss_kb(void);

#endif /* OMNIAGENT_PIPELINE_H */
