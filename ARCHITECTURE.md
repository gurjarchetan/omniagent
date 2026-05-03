
# OmniAgent — Architecture

## Thread Interaction Diagram

```
╔══════════════════════════════════════════════════════════════════════════════════╗
║                           OmniAgent Process  (<10 MB RSS)                       ║
╠══════════════════════════════════════════════════════════════════════════════════╣
║                                                                                  ║
║  RECEIVERS                                                                       ║
║  ─────────                                                                       ║
║  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────────────────┐   ║
║  │ procfs_receiver  │  │ docker_receiver  │  │     inotify_receiver         │   ║
║  │   (Thread #1)    │  │   (Thread #2)    │  │       (Thread #3)            │   ║
║  │                  │  │                  │  │                              │   ║
║  │ /proc/stat       │  │ /var/run/        │  │  /var/log/                   │   ║
║  │ /proc/meminfo    │  │  docker.sock     │  │  /var/lib/docker/containers/ │   ║
║  │ /proc/[pid]/stat │  │ (Unix Domain     │  │  (Linux inotify_add_watch)   │   ║
║  │ Poll: every 5s   │  │  Socket)         │  │  Edge-triggered, fseek diff  │   ║
║  │ Emits: Metric    │  │ Poll: every 30s  │  │  Emits: LogRecord            │   ║
║  │                  │  │ Emits: Metric    │  │                              │   ║
║  └────────┬─────────┘  └────────┬─────────┘  └────────────┬─────────────────┘   ║
║           │                     │                          │                     ║
║           │           ┌─────────┴──────────┐              │                     ║
║           │           │  otlp_http_receiver│              │                     ║
║           │           │     (Thread #4)    │              │                     ║
║           │           │                    │              │                     ║
║           │           │  TCP listen :4318  │              │                     ║
║           │           │  Raw POSIX sockets │              │                     ║
║           │           │  Accepts OTLP/JSON │              │                     ║
║           │           │  Emits: any type   │              │                     ║
║           │           └─────────┬──────────┘              │                     ║
║           │                     │                          │                     ║
║           └─────────────────────┼──────────────────────────┘                    ║
║                                 │ pool_alloc() + rb_enqueue()                   ║
║                                 ▼                                                ║
║  ┌─────────────────────────────────────────────────────────────────────────┐     ║
║  │          MPMC Lock-Free Ring Buffer  [recv_queue]                       │     ║
║  │          4096 slots × TelemetryRecord*  (64 KB overhead)               │     ║
║  │          Vyukov Algorithm — C11 _Atomic, no mutex on hot path          │     ║
║  │          Each slot: atomic uint64_t sequence + TelemetryRecord* ptr    │     ║
║  └─────────────────────────────┬───────────────────────────────────────────┘     ║
║                                │ rb_dequeue()                                    ║
║                                ▼                                                 ║
║  ┌─────────────────────────────────────────────────────────────────────────┐     ║
║  │                  Batch Processor  (Thread #5)                           │     ║
║  │                                                                         │     ║
║  │  ┌──────────────────────────────┐   ┌──────────────────────────────┐   │     ║
║  │  │  Memory Limiter Processor    │   │  Batch Accumulator           │   │     ║
║  │  │                              │   │                              │   │     ║
║  │  │  Reads /proc/self/status     │──▶│  Internal array[1000]        │   │     ║
║  │  │  If RSS > 15 MB:             │   │  Flush trigger:              │   │     ║
║  │  │    DROP record               │   │    count >= 1000 records     │   │     ║
║  │  │    pool_free()               │   │    OR elapsed >= 5 seconds   │   │     ║
║  │  │  Else: PASS through          │   │                              │   │     ║
║  │  └──────────────────────────────┘   └──────────────┬───────────────┘   │     ║
║  └────────────────────────────────────────────────────┼─────────────────────┘    ║
║                                                       │ signal via condvar        ║
║                                                       ▼                           ║
║  ┌─────────────────────────────────────────────────────────────────────────┐     ║
║  │                   Zstd Exporter  (Thread #6)                            │     ║
║  │                                                                         │     ║
║  │  Batch → telemetry_batch_to_json() → ZSTD_compress(level=3)            │     ║
║  │       → HTTP POST (raw socket) → OTel Collector / Datadog endpoint      │     ║
║  │                                                                         │     ║
║  │  On success: pool_free() all records in batch                           │     ║
║  │  On failure: retry with exponential backoff (max 3 attempts)            │     ║
║  └─────────────────────────────────────────────────────────────────────────┘     ║
║                                                                                  ║
╠══════════════════════════════════════════════════════════════════════════════════╣
║  MEMORY LAYOUT (target < 10 MB RSS)                                             ║
║  ──────────────────────────────────────────────────────────────────────────────  ║
║  RecordPool:  2048 × TelemetryRecord  ≈  3.0 MB  (pre-allocated slab)          ║
║  recv_queue:  4096 × RBSlot           ≈  64 KB   (ring buffer)                 ║
║  export_buf:  ExportBatch (1000 ptrs) ≈  8 KB                                  ║
║  HTTP bufs:   4 × 64 KB              ≈  256 KB   (receiver I/O scratch)        ║
║  Stack RSS:   6 threads × ~128 KB    ≈  768 KB   (only touched pages)          ║
║  Code + BSS:                         ≈  200 KB                                 ║
║  ─────────────────────────────────────────────────────────────────────────────  ║
║  Total (approx):                      ≈  4.3 MB  RSS                           ║
╚══════════════════════════════════════════════════════════════════════════════════╝

## Data Flow

  [Kernel /proc] ──▶ procfs_recv ──┐
  [Docker socket] ─▶ docker_recv ──┤
  [inotify events]─▶ inotify_recv ─┼──▶ recv_queue ──▶ batch_proc ──▶ zstd_exporter
  [OTLP TCP :4318]─▶ otlp_recv ───┘         ▲
                                      pool_alloc()
                                      pool_free()

## Key Design Decisions

1. **Lock-Free Hot Path**: Receivers write to ring buffer using Vyukov MPMC with
   C11 atomics (`memory_order_release` / `memory_order_acquire`).  No mutex
   contention between receiver threads.

2. **Slab Memory Pool**: All TelemetryRecords are allocated from a fixed slab.
   Eliminates heap fragmentation and makes RSS predictable.

3. **Zero-Config Discovery**: No YAML.  The agent probes /proc, /var/run/docker.sock,
   and /var/log at startup.  Missing subsystems are silently skipped.

4. **Graceful Backpressure**: If the ring buffer is full (receivers faster than
   exporter), new records are dropped with a counter increment (never blocking
   the producer thread).

5. **OOM Protection**: The memory limiter reads /proc/self/status VmRSS before
   passing each record to the batch.  Above 15 MB, records are dropped until
   the exporter catches up.
```
