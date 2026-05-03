/*
 * omniagent/include/pool.h
 *
 * Fixed-size slab allocator for TelemetryRecord objects.
 *
 * Allocating from the system heap (malloc) inside a tight receiver loop
 * causes heap fragmentation and unpredictable latency spikes.  This pool
 * pre-allocates all records in one contiguous slab and manages a free-list
 * protected by a lightweight spinlock.
 *
 * Design goals:
 *   - All pool memory is committed at startup → RSS is predictable.
 *   - pool_alloc / pool_free are O(1).
 *   - The spinlock is held for only a few nanoseconds; contention is low
 *     because receivers allocate infrequently relative to CPU speed.
 */

#ifndef OMNIAGENT_POOL_H
#define OMNIAGENT_POOL_H

#include <stdatomic.h>
#include <stddef.h>

#include "telemetry.h"

/* ── Pool capacity ───────────────────────────────────────────────────────── */

/*
 * 8192 records × ~1414 bytes ≈ 11.3 MB RSS.
 * Increased from 2048 to handle per-process metric bursts:
 * a host with ~1500 processes × 2–3 metrics each needs headroom beyond
 * two batch-processor cycles (2 × 1000) to avoid silent drops.
 */
#define POOL_CAPACITY  8192U

/* ── Internal node (embeds the free-list pointer) ─────────────────────────── */

typedef struct PoolNode {
    TelemetryRecord  record;         /* must be first — pool_alloc casts    */
    struct PoolNode *next_free;      /* intrusive free-list link            */
} PoolNode;

/* ── Pool object ─────────────────────────────────────────────────────────── */

typedef struct {
    PoolNode          nodes[POOL_CAPACITY]; /* slab                         */
    PoolNode         *free_head;            /* top of free-list             */
    _Atomic int       spinlock;             /* 0=unlocked, 1=locked         */
    _Atomic size_t    allocated;            /* live records right now        */
    _Atomic uint64_t  total_allocs;         /* lifetime allocation count     */
    _Atomic uint64_t  total_exhausted;      /* times pool was fully depleted */
} RecordPool;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Must be called once before any threads start. */
void pool_init(RecordPool *pool);

/*
 * Allocate one TelemetryRecord.
 * Returns NULL if the pool is exhausted (all 2048 slots in flight).
 * Caller must zero-fill the returned record or set every field it uses.
 */
TelemetryRecord *pool_alloc(RecordPool *pool);

/*
 * Return a previously-allocated TelemetryRecord to the pool.
 * Passing NULL is a no-op.  Double-free is not detected — callers must
 * track ownership.
 */
void pool_free(RecordPool *pool, TelemetryRecord *record);

/* Snapshot of current live allocations (approximate). */
size_t pool_allocated(const RecordPool *pool);

#endif /* OMNIAGENT_POOL_H */
