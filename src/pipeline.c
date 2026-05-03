/*
 * omniagent/src/pipeline.c
 *
 * Global pipeline state definitions and lifecycle helpers.
 */

#include "omniagent.h"

/* ── Global definitions ─────────────────────────────────────────────────── */

AgentConfig   g_config;
RecordPool    g_pool;
RingBuffer    g_recv_queue;
ExportQueue   g_export_queue;
_Atomic int   g_running = 1;
int           g_debug_mode = 0;

/* ── pipeline_init ───────────────────────────────────────────────────────── */

int pipeline_init(void)
{
    /* Memory pool */
    pool_init(&g_pool);

    /* Receiver ring buffer */
    rb_init(&g_recv_queue);

    /* Export queue */
    memset(&g_export_queue, 0, sizeof(g_export_queue));

    int rc;
    rc = pthread_mutex_init(&g_export_queue.mutex, NULL);
    if (rc != 0) {
        LOG_ERROR("pthread_mutex_init: %s", strerror(rc));
        return -1;
    }
    rc = pthread_cond_init(&g_export_queue.not_empty, NULL);
    if (rc != 0) {
        LOG_ERROR("pthread_cond_init(not_empty): %s", strerror(rc));
        pthread_mutex_destroy(&g_export_queue.mutex);
        return -1;
    }
    rc = pthread_cond_init(&g_export_queue.not_full, NULL);
    if (rc != 0) {
        LOG_ERROR("pthread_cond_init(not_full): %s", strerror(rc));
        pthread_cond_destroy(&g_export_queue.not_empty);
        pthread_mutex_destroy(&g_export_queue.mutex);
        return -1;
    }

    LOG_INFO("pipeline: initialised (recv_queue=%u slots, export_queue=%u batches)",
             RB_CAPACITY, EXPORT_BATCH_QUEUE_DEPTH);
    return 0;
}

/* ── pipeline_shutdown ───────────────────────────────────────────────────── */

void pipeline_shutdown(pthread_t *threads, size_t n_threads)
{
    LOG_INFO("pipeline: shutdown — joining %zu threads", n_threads);

    /* Signal all threads to stop. */
    atomic_store_explicit(&g_running, 0, memory_order_release);

    /* Wake the exporter in case it's blocked on the condvar. */
    pthread_mutex_lock(&g_export_queue.mutex);
    pthread_cond_broadcast(&g_export_queue.not_empty);
    pthread_cond_broadcast(&g_export_queue.not_full);
    pthread_mutex_unlock(&g_export_queue.mutex);

    /* Join all threads (wait for clean exit). */
    for (size_t i = 0; i < n_threads; i++) {
        if (threads[i] == 0) continue;
        int rc = pthread_join(threads[i], NULL);
        if (rc != 0) {
            LOG_WARN("pthread_join[%zu]: %s", i, strerror(rc));
        }
    }

    /* Destroy synchronisation objects. */
    pthread_cond_destroy(&g_export_queue.not_full);
    pthread_cond_destroy(&g_export_queue.not_empty);
    pthread_mutex_destroy(&g_export_queue.mutex);

    LOG_INFO("pipeline: shutdown complete. drops=%" PRIu64
             " pool_exhausted=%" PRIu64,
             rb_drop_count(&g_recv_queue),
             atomic_load_explicit(&g_pool.total_exhausted,
                                  memory_order_relaxed));
}

/* ── export_queue_push ───────────────────────────────────────────────────── */

void export_queue_push(ExportQueue *q, const ExportBatch *batch)
{
    pthread_mutex_lock(&q->mutex);

    while (q->size == EXPORT_BATCH_QUEUE_DEPTH
           && atomic_load_explicit(&g_running, memory_order_relaxed))
    {
        /* Block until the exporter frees a slot. */
        pthread_cond_wait(&q->not_full, &q->mutex);
    }

    if (q->size < EXPORT_BATCH_QUEUE_DEPTH) {
        q->batches[q->head] = *batch;
        q->head = (q->head + 1) % EXPORT_BATCH_QUEUE_DEPTH;
        q->size++;
        pthread_cond_signal(&q->not_empty);
    } else {
        /* Agent is shutting down — free the records that never got exported. */
        LOG_WARN("export_queue: full at shutdown, dropping %zu records",
                 batch->count);
        for (size_t i = 0; i < batch->count; i++) {
            pool_free(&g_pool, batch->records[i]);
        }
    }

    pthread_mutex_unlock(&q->mutex);
}

/* ── export_queue_pop ────────────────────────────────────────────────────── */

ExportBatch export_queue_pop(ExportQueue *q)
{
    ExportBatch empty = { .count = 0 };

    pthread_mutex_lock(&q->mutex);

    while (q->size == 0
           && atomic_load_explicit(&g_running, memory_order_relaxed))
    {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }

    ExportBatch batch = empty;
    if (q->size > 0) {
        batch = q->batches[q->tail];
        q->tail = (q->tail + 1) % EXPORT_BATCH_QUEUE_DEPTH;
        q->size--;
        pthread_cond_signal(&q->not_full);
    }

    pthread_mutex_unlock(&q->mutex);
    return batch;
}
