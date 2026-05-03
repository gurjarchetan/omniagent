/*
 * omniagent/include/ring_buffer.h
 *
 * Wait-free MPMC (Multi-Producer, Multi-Consumer) ring buffer.
 *
 * Algorithm: Dmitry Vyukov's bounded MPMC queue, adapted for C11 atomics.
 * Each slot carries an atomic sequence counter.  Producers and consumers
 * race only on the shared position counters; the sequence counter per slot
 * acts as a per-slot handshake that avoids any mutex.
 *
 * Memory layout (cache-line aware):
 *   slots[]      — RB_CAPACITY × { sequence, ptr }  padded to 64 bytes
 *   enqueue_pos  — on its own cache line
 *   dequeue_pos  — on its own cache line
 *
 * This layout ensures that the two atomic counters never share a cache
 * line with each other or with slot data, eliminating false sharing.
 */

#ifndef OMNIAGENT_RING_BUFFER_H
#define OMNIAGENT_RING_BUFFER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "telemetry.h"

/* ── Tunable constants ───────────────────────────────────────────────────── */

/* Must be a power of two.  4096 slots × 16 bytes = 64 KB ring overhead.  */
#define RB_CAPACITY  4096U
#define RB_MASK      (RB_CAPACITY - 1U)

#define CACHE_LINE   64U

/* ── Ring-buffer slot ────────────────────────────────────────────────────── */

typedef struct {
    _Atomic uint64_t    sequence;   /* Vyukov handshake counter             */
    TelemetryRecord    *record;     /* pointer into the global RecordPool   */
    /* Pad to one cache line to prevent false sharing between adjacent slots */
    char                _pad[CACHE_LINE - sizeof(_Atomic uint64_t)
                                        - sizeof(TelemetryRecord *)];
} RBSlot;

_Static_assert(sizeof(RBSlot) == CACHE_LINE,
               "RBSlot must be exactly one cache line");

/* ── Ring buffer ─────────────────────────────────────────────────────────── */

typedef struct {
    RBSlot           slots[RB_CAPACITY];    /* hot data — 256 KB            */

    /* Producer position — on its own cache line */
    char             _pad1[CACHE_LINE];
    _Atomic uint64_t enqueue_pos;
    char             _pad2[CACHE_LINE - sizeof(_Atomic uint64_t)];

    /* Consumer position — on its own cache line */
    _Atomic uint64_t dequeue_pos;
    char             _pad3[CACHE_LINE - sizeof(_Atomic uint64_t)];

    /* Drop counter — incremented when enqueue fails (buffer full) */
    _Atomic uint64_t drops;
} RingBuffer;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Initialise a zero-filled RingBuffer (also correct after memset to 0). */
void rb_init(RingBuffer *rb);

/*
 * Enqueue a record pointer.
 * Returns true on success, false if the buffer is full (caller must drop).
 * Lock-free: safe to call from any number of threads simultaneously.
 */
bool rb_enqueue(RingBuffer *rb, TelemetryRecord *record);

/*
 * Dequeue a record pointer.
 * Returns NULL if the buffer is empty.
 * Lock-free: safe to call from any number of threads simultaneously.
 */
TelemetryRecord *rb_dequeue(RingBuffer *rb);

/* Approximate number of items in the buffer (may be stale). */
size_t rb_size_approx(const RingBuffer *rb);

/* Total items dropped due to full buffer since rb_init(). */
uint64_t rb_drop_count(const RingBuffer *rb);

#endif /* OMNIAGENT_RING_BUFFER_H */
