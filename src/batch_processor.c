/*
 * omniagent/src/batch_processor.c
 *
 * Batch Processor — bridges the recv_queue (filled by receivers) and the
 * export_queue (consumed by the Zstd exporter).
 *
 * Algorithm
 * ─────────
 * 1. Drain records from the MPMC ring buffer one at a time.
 * 2. Pass each record through the Memory Limiter processor.
 *    If it returns 0 (drop), pool_free() has already been called; skip.
 * 3. Accumulate passing records into an ExportBatch.
 * 4. Flush the batch to the export_queue when EITHER:
 *      a. batch.count >= BATCH_MAX_RECORDS (1000), OR
 *      b. Time since last flush >= BATCH_MAX_SECONDS (5 s)
 * 5. After flushing, reset the batch and update the flush timestamp.
 *
 * If the ring buffer is empty, we do a short nanosleep to avoid
 * busy-spinning and wasting a full CPU core.  The sleep is short enough
 * (1 ms) that the time-based flush trigger is accurate to ~1 ms.
 *
 * Backpressure: if export_queue_push() blocks (export queue full because
 * the exporter is slow / backend unreachable), the batch processor will
 * pause here.  This propagates backpressure up to the ring buffer, which
 * will then start dropping records.  This is the intended behaviour — we
 * prefer dropping telemetry over causing the host process to OOM.
 */

#include "omniagent.h"

/* ── Batch Processor thread ──────────────────────────────────────────────── */

void *batch_processor_thread(void *arg)
{
    OAGENT_UNUSED(arg);

    LOG_INFO("batch_processor: started (max_records=%u, max_seconds=%d)",
             BATCH_MAX_RECORDS, BATCH_MAX_SECONDS);

    ExportBatch batch;
    memset(&batch, 0, sizeof(batch));

    struct timespec flush_deadline;
    clock_gettime(CLOCK_MONOTONIC, &flush_deadline);
    flush_deadline.tv_sec += BATCH_MAX_SECONDS;

    uint64_t total_processed = 0;
    uint64_t total_batches   = 0;

    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {

        /* ── Try to drain the ring buffer ── */
        TelemetryRecord *rec = rb_dequeue(&g_recv_queue);

        if (rec == NULL) {
            /*
             * Ring buffer empty.  Check if the time-based flush is due
             * before sleeping, so we don't miss the deadline.
             */
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);

            int time_expired =
                (now.tv_sec  > flush_deadline.tv_sec) ||
                (now.tv_sec == flush_deadline.tv_sec &&
                 now.tv_nsec >= flush_deadline.tv_nsec);

            if (batch.count > 0 && time_expired) {
                goto flush;
            }

            /* Sleep 1 ms to avoid busy-waiting on an empty queue */
            struct timespec sleep_ts = { .tv_sec = 0, .tv_nsec = 1000000L };
            nanosleep(&sleep_ts, NULL);
            continue;
        }

        /* ── Memory Limiter ── */
        if (!memory_limiter_check(rec)) {
            /* record was dropped and freed inside memory_limiter_check */
            continue;
        }

        /* ── Add to batch ── */
        batch.records[batch.count++] = rec;
        total_processed++;

        /* ── Check flush conditions ── */
        if (batch.count >= BATCH_MAX_RECORDS) {
            goto flush;
        }

        /* Check time-based deadline */
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec  > flush_deadline.tv_sec ||
               (now.tv_sec == flush_deadline.tv_sec &&
                now.tv_nsec >= flush_deadline.tv_nsec))
            {
                goto flush;
            }
        }

        continue;

    flush:
        if (batch.count == 0) {
            /* Nothing to flush (time expired on empty batch) */
            clock_gettime(CLOCK_MONOTONIC, &flush_deadline);
            flush_deadline.tv_sec += BATCH_MAX_SECONDS;
            continue;
        }

        LOG_DEBUG("batch_processor: flushing %zu records (total_batches=%" PRIu64
                  " rss=%" PRIu64 "KB drops=%" PRIu64 ")",
                  batch.count, total_batches,
                  memory_limiter_rss_kb(),
                  memory_limiter_drop_count());

        /*
         * Hand the batch to the exporter.
         * This call may block if the export_queue is full (8 batches).
         * Once it returns, the exporter owns the records and will call
         * pool_free() on each one after successful export.
         */
        export_queue_push(&g_export_queue, &batch);
        total_batches++;

        /* Reset batch and schedule next flush deadline */
        memset(&batch, 0, sizeof(batch));
        clock_gettime(CLOCK_MONOTONIC, &flush_deadline);
        flush_deadline.tv_sec += BATCH_MAX_SECONDS;
    }

    /*
     * Shutdown path: flush whatever is in the current batch so the exporter
     * gets a chance to send the last partial batch.
     */
    if (batch.count > 0) {
        LOG_INFO("batch_processor: final flush of %zu records at shutdown",
                 batch.count);
        export_queue_push(&g_export_queue, &batch);
    }

    LOG_INFO("batch_processor: exiting "
             "(total_processed=%" PRIu64 " total_batches=%" PRIu64 ")",
             total_processed, total_batches);
    return NULL;
}
