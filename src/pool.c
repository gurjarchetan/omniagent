/*
 * omniagent/src/pool.c
 *
 * Fixed-size slab allocator for TelemetryRecord objects.
 *
 * The free-list is protected by a ticket-less test-and-set spinlock.
 * Contention is negligible because:
 *   - alloc/free are called infrequently (one per scraped metric)
 *   - the critical section is 3–5 instructions
 *
 * If the pool is exhausted, pool_alloc() returns NULL and the caller must
 * drop the telemetry item.  The exhaustion counter tracks this for diagnosis.
 */

#include "omniagent.h"

/* ── Spinlock helpers ───────────────────────────────────────────────────── */

static inline void spin_lock(_Atomic int *lock)
{
    int expected = 0;
    /*
     * Spin until we atomically change 0 → 1.
     * Use weak CAS in the inner loop for better performance on ARM/RISC-V.
     * memory_order_acquire ensures that loads after the lock are not
     * reordered before we hold it.
     */
    while (!atomic_compare_exchange_weak_explicit(
               lock, &expected, 1,
               memory_order_acquire, memory_order_relaxed))
    {
        expected = 0;
        /* Voluntary yield on high contention — avoids monopolising a core. */
        sched_yield();
    }
}

static inline void spin_unlock(_Atomic int *lock)
{
    atomic_store_explicit(lock, 0, memory_order_release);
}

/* ── pool_init ───────────────────────────────────────────────────────────── */

void pool_init(RecordPool *pool)
{
    memset(pool, 0, sizeof(*pool));
    atomic_store_explicit(&pool->spinlock, 0, memory_order_relaxed);
    atomic_store_explicit(&pool->allocated, 0, memory_order_relaxed);
    atomic_store_explicit(&pool->total_allocs, 0, memory_order_relaxed);
    atomic_store_explicit(&pool->total_exhausted, 0, memory_order_relaxed);

    /* Build the initial free list: node[0] → node[1] → … → node[N-1] → NULL */
    for (size_t i = 0; i < POOL_CAPACITY - 1; i++) {
        pool->nodes[i].next_free = &pool->nodes[i + 1];
    }
    pool->nodes[POOL_CAPACITY - 1].next_free = NULL;
    pool->free_head = &pool->nodes[0];

    LOG_INFO("pool: initialised %u record slots (%zu KB)",
             POOL_CAPACITY,
             (sizeof(pool->nodes)) / 1024);
}

/* ── pool_alloc ──────────────────────────────────────────────────────────── */

TelemetryRecord *pool_alloc(RecordPool *pool)
{
    spin_lock(&pool->spinlock);

    PoolNode *node = pool->free_head;
    if (node == NULL) {
        /* Pool exhausted — caller must drop this telemetry item. */
        spin_unlock(&pool->spinlock);
        atomic_fetch_add_explicit(&pool->total_exhausted, 1,
                                  memory_order_relaxed);
        return NULL;
    }

    pool->free_head = node->next_free;
    spin_unlock(&pool->spinlock);

    atomic_fetch_add_explicit(&pool->allocated,    1, memory_order_relaxed);
    atomic_fetch_add_explicit(&pool->total_allocs, 1, memory_order_relaxed);

    /* Zero the record portion only (not the free-list pointer). */
    memset(&node->record, 0, sizeof(node->record));
    return &node->record;
}

/* ── pool_free ───────────────────────────────────────────────────────────── */

void pool_free(RecordPool *pool, TelemetryRecord *record)
{
    if (record == NULL) {
        return;
    }

    /*
     * Recover the PoolNode by subtracting the offset of `record` within
     * PoolNode.  This is safe because pool_alloc() always hands out
     * &node->record, and PoolNode has record as its first member.
     */
    PoolNode *node = (PoolNode *)record;  /* record IS the first field */

    spin_lock(&pool->spinlock);
    node->next_free = pool->free_head;
    pool->free_head = node;
    spin_unlock(&pool->spinlock);

    atomic_fetch_sub_explicit(&pool->allocated, 1, memory_order_relaxed);
}

/* ── pool_allocated ──────────────────────────────────────────────────────── */

size_t pool_allocated(const RecordPool *pool)
{
    return atomic_load_explicit(&pool->allocated, memory_order_relaxed);
}
