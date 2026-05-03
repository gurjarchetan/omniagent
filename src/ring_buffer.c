/*
 * omniagent/src/ring_buffer.c
 *
 * Wait-free MPMC ring buffer — Dmitry Vyukov algorithm in C11.
 *
 * Correctness argument
 * ────────────────────
 * Every slot starts with sequence == slot_index.
 *
 * ENQUEUE (producer P):
 *   1. Atomically read `enqueue_pos` (relaxed).
 *   2. Load slot[pos & MASK].sequence (acquire).
 *   3. diff = seq - pos.
 *      diff == 0 → slot is free; attempt CAS(enqueue_pos, pos, pos+1) acq_rel.
 *               → On success: write record, then store(sequence, pos+1, release).
 *      diff < 0  → buffer full; return false.
 *      diff > 0  → pos is stale (another producer won the race); reload pos.
 *
 * DEQUEUE (consumer C):
 *   1. Atomically read `dequeue_pos` (relaxed).
 *   2. Load slot[pos & MASK].sequence (acquire).
 *   3. diff = seq - (pos+1).
 *      diff == 0 → data ready; attempt CAS(dequeue_pos, pos, pos+1) acq_rel.
 *               → On success: read record, store(sequence, pos+RB_CAPACITY, release).
 *      diff < 0  → buffer empty; return NULL.
 *      diff > 0  → pos stale; reload.
 *
 * Memory ordering
 * ───────────────
 * The `release` store on the sequence counter after writing the record ensures
 * the record pointer is visible to any consumer that subsequently does an
 * `acquire` load of that sequence.  This forms the required happens-before
 * edge between producer write and consumer read.
 */

#include "omniagent.h"

/* ── Initialisation ──────────────────────────────────────────────────────── */

void rb_init(RingBuffer *rb)
{
    memset(rb, 0, sizeof(*rb));
    for (uint64_t i = 0; i < RB_CAPACITY; i++) {
        atomic_store_explicit(&rb->slots[i].sequence, i, memory_order_relaxed);
    }
    atomic_store_explicit(&rb->enqueue_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->dequeue_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->drops,       0, memory_order_relaxed);
}

/* ── Enqueue ─────────────────────────────────────────────────────────────── */

bool rb_enqueue(RingBuffer *rb, TelemetryRecord *record)
{
    RBSlot   *slot;
    uint64_t  pos;
    uint64_t  seq;
    int64_t   diff;

    pos = atomic_load_explicit(&rb->enqueue_pos, memory_order_relaxed);

    for (;;) {
        slot = &rb->slots[pos & RB_MASK];
        seq  = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        diff = (int64_t)seq - (int64_t)pos;

        if (diff == 0) {
            /*
             * This slot is free.  Try to claim it by advancing enqueue_pos.
             * On failure another producer claimed it first — loop with the
             * updated pos value already placed in pos by the CAS.
             */
            if (atomic_compare_exchange_weak_explicit(
                    &rb->enqueue_pos, &pos, pos + 1,
                    memory_order_acq_rel, memory_order_relaxed))
            {
                /* We own this slot.  Write payload, then publish. */
                slot->record = record;
                atomic_store_explicit(&slot->sequence, pos + 1,
                                      memory_order_release);
                return true;
            }
            /* pos was updated by the failed CAS — retry immediately */

        } else if (diff < 0) {
            /* Buffer is full — signal caller to drop. */
            atomic_fetch_add_explicit(&rb->drops, 1, memory_order_relaxed);
            return false;

        } else {
            /* Another producer is ahead of us; re-read the actual position. */
            pos = atomic_load_explicit(&rb->enqueue_pos, memory_order_relaxed);
        }
    }
}

/* ── Dequeue ─────────────────────────────────────────────────────────────── */

TelemetryRecord *rb_dequeue(RingBuffer *rb)
{
    RBSlot          *slot;
    uint64_t         pos;
    uint64_t         seq;
    int64_t          diff;
    TelemetryRecord *record;

    pos = atomic_load_explicit(&rb->dequeue_pos, memory_order_relaxed);

    for (;;) {
        slot = &rb->slots[pos & RB_MASK];
        seq  = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        diff = (int64_t)seq - (int64_t)(pos + 1);

        if (diff == 0) {
            /*
             * Data is ready in this slot.  Try to claim the slot.
             */
            if (atomic_compare_exchange_weak_explicit(
                    &rb->dequeue_pos, &pos, pos + 1,
                    memory_order_acq_rel, memory_order_relaxed))
            {
                record = slot->record;
                /*
                 * Release the slot back to producers.  Adding RB_CAPACITY to
                 * the position makes this slot available again exactly one
                 * wrap-around later.
                 */
                atomic_store_explicit(&slot->sequence, pos + RB_CAPACITY,
                                      memory_order_release);
                return record;
            }
            /* pos updated by CAS — retry */

        } else if (diff < 0) {
            /* Buffer is empty. */
            return NULL;

        } else {
            /* Another consumer is ahead of us — reload. */
            pos = atomic_load_explicit(&rb->dequeue_pos, memory_order_relaxed);
        }
    }
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

size_t rb_size_approx(const RingBuffer *rb)
{
    uint64_t eq = atomic_load_explicit(&rb->enqueue_pos, memory_order_relaxed);
    uint64_t dq = atomic_load_explicit(&rb->dequeue_pos, memory_order_relaxed);
    /* eq >= dq always (they grow monotonically) */
    uint64_t diff = eq - dq;
    return (size_t)(diff < RB_CAPACITY ? diff : RB_CAPACITY);
}

uint64_t rb_drop_count(const RingBuffer *rb)
{
    return atomic_load_explicit(&rb->drops, memory_order_relaxed);
}
