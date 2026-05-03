# OmniAgent

> Ultra-lightweight Linux observability daemon written in pure C11.
> **~104 KB binary · ~7 MB RSS · zero config · no runtime dependencies beyond libc, libpthread, libzstd.**

[![CI](https://github.com/gurjarchetan/omniagent/actions/workflows/ci.yml/badge.svg)](https://github.com/gurjarchetan/omniagent/actions/workflows/ci.yml)
[![Release](https://github.com/gurjarchetan/omniagent/actions/workflows/release.yml/badge.svg)](https://github.com/gurjarchetan/omniagent/actions/workflows/release.yml)
[![CodeQL](https://github.com/gurjarchetan/omniagent/actions/workflows/codeql.yml/badge.svg)](https://github.com/gurjarchetan/omniagent/actions/workflows/codeql.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Language: C11](https://img.shields.io/badge/Language-C11-orange.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Platform: Linux](https://img.shields.io/badge/Platform-Linux-lightgrey.svg)](https://kernel.org)
[![Docker](https://img.shields.io/badge/Docker-ghcr.io%2Fgurjarchetan%2Fomniagent-blue?logo=docker)](https://github.com/gurjarchetan/omniagent/pkgs/container/omniagent)

OmniAgent is a single-binary host agent that replaces `node_exporter` + `cAdvisor` + a log shipper in one process.
It scrapes `/proc`, talks to the Docker socket, watches log files via inotify, and serves a Prometheus `/metrics`
endpoint while simultaneously pushing logs to Loki — **without any OTel Collector, bridge, or sidecar in the middle.**

```
OmniAgent (host)
  ├── :9100/metrics  ←── Prometheus scrapes every 15 s ──┐
  └── POST → Loki :3100  (per batch, ~5 s)  ─────────────┴→ Grafana  (12 pre-built dashboards)
```

---

## Table of Contents

- [Why OmniAgent](#why-omniagent)
- [What it collects](#what-it-collects)
- [Quick start](#quick-start)
- [CLI reference](#cli-reference)
- [Environment variables](#environment-variables)
- [Feature toggles](#feature-toggles)
- [Log filtering](#log-filtering)
- [Production install](#production-install--systemd)
- [Visualisation stack](#visualisation-stack)
- [Grafana dashboards](#grafana-dashboards)
- [Pipeline architecture](#pipeline-architecture)
- [Memory budget](#memory-budget)
- [Prometheus metrics reference](#prometheus-metrics-reference)
- [Loki log format](#loki-log-format)
- [Build targets](#build-targets)
- [Troubleshooting](#troubleshooting)
- [Requirements](#requirements)
- [Contributing](#contributing)
- [License](#license)

---

## Why OmniAgent

Most observability stacks on Linux require at least three separate agents:

| What you need | Typical solution | RAM |
|---|---|---|
| Host metrics | `node_exporter` | ~15 MB |
| Container metrics | `cAdvisor` | ~60 MB |
| Log shipping | `Promtail` or `Filebeat` | ~50 MB |

**OmniAgent does all three in a single ~104 KB binary using ~7 MB of RAM.**

It is deliberately minimal:
- No YAML config file — everything is CLI flags or environment variables
- No runtime-downloaded plugins or schemas
- No OTel Collector or agent-in-the-middle required
- No heap allocations on the hot path — all records come from a pre-allocated slab pool
- Clean shutdown on `SIGTERM` / `SIGINT`, zero leaks (validated with ASan + LSan)

---

## What it collects

### Host metrics (via `/proc` and `sysfs`)

| Source file | Metrics emitted |
|-------------|-----------------|
| `/proc/stat` | CPU usage per mode: user, nice, system, idle, iowait, irq, softirq, steal; context switches; interrupts; boot time |
| `/proc/meminfo` | Total/free/available/buffers/cached/slab/dirty/writeback memory; swap used/free |
| `/proc/loadavg` | Load average 1 / 5 / 15 min; running and total thread counts |
| `/proc/uptime` | System uptime (seconds) |
| `/proc/net/dev` | Per-NIC: rx/tx bytes, packets, errors, drops |
| `/proc/diskstats` | Per-disk: reads/writes completed, bytes read/written, I/O time |
| `/proc/mounts` + `statvfs` | Per-mount: total/used/free/available blocks and inodes, utilisation % |
| `/proc/net/sockstat` | TCP/UDP/raw socket counts |
| `/proc/vmstat` | Page faults, swap-in/out, dirty pages |

### Container metrics (via Docker socket `/var/run/docker.sock`)

Per running container: CPU utilisation, user/system CPU %, throttled periods, throttled time,
memory usage/limit/RSS/cache/swap/utilisation, network rx/tx bytes/packets/errors/drops,
disk read/write bytes/ops, restart count, OOM event count.

Matches the `cAdvisor` label set — works with existing Grafana dashboards that target cAdvisor.

### Logs (via Linux inotify)

Watches `/var/log/` and `/var/lib/docker/containers/` by default.
Any additional directories can be added via `-l` or `OAGENT_LOG_DIRS`.
New lines are emitted as structured log records with auto-detected severity,
source file path, hostname, and nanosecond timestamp.

### OTLP/HTTP receiver (port `4318`)

Accepts metrics, logs, and traces from any OpenTelemetry SDK over HTTP/JSON.
Useful for forwarding application telemetry into the same Loki/Prometheus pipeline.

---

## Quick start

### 1. Install build dependencies

```bash
# Ubuntu / Debian
sudo apt-get install -y build-essential libzstd-dev

# CentOS / RHEL / Fedora / Amazon Linux 2023
sudo dnf install -y gcc make libzstd-devel

# Arch Linux
sudo pacman -Sy gcc make zstd

# Alpine Linux
apk add --no-cache gcc make musl-dev zstd-dev
```

### 2. Clone and build

```bash
git clone https://github.com/gurjarchetan/omniagent.git
cd omniagent
make -j$(nproc)       # → ./omniagent  (~104 KB optimised binary)
```

### 3. Start the visualisation stack

```bash
cd viz/
docker compose up -d
```

| Service | URL | Credentials |
|---------|-----|-------------|
| Grafana | http://localhost:3000 | `admin` / `admin` |
| Prometheus | http://localhost:9090 | — |
| Loki | http://localhost:3100 | — |

### 4. Run OmniAgent

```bash
sudo ./omniagent -L localhost -Q 3100 -m 9100
```

- Host and container **metrics** appear in Prometheus within one scrape interval (~15 s).
- **Logs** appear in Loki within one batch flush (~5 s).
- All 12 Grafana dashboards populate automatically.

### One-shot production install

```bash
sudo ./deploy.sh start
# → builds, installs to /usr/local/bin, registers systemd unit, starts on boot
```

### Or use the pre-built Docker image

```bash
# Pull the latest release image from GitHub Container Registry
docker pull ghcr.io/gurjarchetan/omniagent:latest

# Run (host network + PID namespace required for full metrics)
docker run -d --name omniagent \
  --pid host \
  --network host \
  -v /var/run/docker.sock:/var/run/docker.sock \
  -v /var/log:/var/log:ro \
  -v /var/lib/docker/containers:/var/lib/docker/containers:ro \
  -e OAGENT_LOKI_HOST=localhost \
  -e OAGENT_LOKI_PORT=3100 \
  ghcr.io/gurjarchetan/omniagent:latest
```

---

## CLI reference

```
Usage: omniagent [options]

  -L <host>    Loki push endpoint host          (default: localhost)
  -Q <port>    Loki push endpoint port          (default: 3100)
  -m <port>    Prometheus /metrics port         (default: 9100)
  -l <dirs>    Extra log dirs, colon-separated  (inotify watched)
  -i <secs>    procfs scrape interval seconds   (default: 5)
  -d           Enable debug logging
  -h           Print this help
```

**CLI flags always take precedence over environment variables.**

### Examples

```bash
# Standard local stack
sudo ./omniagent -L localhost -Q 3100 -m 9100

# Remote Loki
sudo ./omniagent -L loki.mycompany.internal -Q 3100 -m 9100

# Watch extra application log directories
sudo ./omniagent -L localhost -Q 3100 -m 9100 \
  -l /var/log/nginx:/var/log/myapp:/srv/app/logs

# Faster scrape interval with debug output (dev/testing only)
sudo ./omniagent -L localhost -Q 3100 -m 9100 -i 1 -d

# Metrics only — no log collection (via env var, see below)
OAGENT_ENABLE_LOGS=0 sudo ./omniagent -L localhost -Q 3100 -m 9100
```

---

## Environment variables

All configuration can be supplied via environment variables.
This is the recommended approach for Docker and Kubernetes deployments.
**CLI flags override env vars when both are set.**

### Endpoint configuration

| Variable | Default | Description |
|----------|---------|-------------|
| `OAGENT_LOKI_HOST` | `localhost` | Loki push endpoint host (same as `-L`) |
| `OAGENT_LOKI_PORT` | `3100` | Loki push endpoint port (same as `-Q`) |
| `OAGENT_METRICS_PORT` | `9100` | Prometheus `/metrics` port (same as `-m`) |
| `OAGENT_SCRAPE_INTERVAL` | `5` | procfs scrape interval in seconds (same as `-i`) |
| `OAGENT_LOG_DIRS` | _(empty)_ | Extra colon-separated log dirs to watch (same as `-l`) |
| `OAGENT_DEBUG` | `0` | Set to `1` to enable debug logging (same as `-d`) |

### Feature toggles

| Variable | Default | Description |
|----------|---------|-------------|
| `OAGENT_ENABLE_METRICS` | `1` | Host + container metrics, Prometheus endpoint |
| `OAGENT_ENABLE_LOGS` | `1` | inotify log collection → Loki |
| `OAGENT_ENABLE_OTLP` | `1` | OTLP/HTTP receiver on port `4318` |

Set any of these to `0` to fully disable that component.
No threads are spawned and no resources are allocated for disabled features.

### Log filtering

| Variable | Example value | Description |
|----------|---------------|-------------|
| `OAGENT_LOG_INCLUDE` | `*.log,syslog,auth.log` | Only watch files matching these patterns (empty = all files) |
| `OAGENT_LOG_EXCLUDE` | `*.gz,*.bz2,*.zip,*.1` | Skip files matching these patterns |

Patterns are matched against the **basename** of each file using `fnmatch(3)`.
Both variables accept comma-separated lists.
`OAGENT_LOG_EXCLUDE` is applied after `OAGENT_LOG_INCLUDE`.

---

## Feature toggles

Set a toggle to `0` to completely turn off that subsystem:

```bash
# Metrics only — no log collection, no OTLP receiver
OAGENT_ENABLE_LOGS=0 OAGENT_ENABLE_OTLP=0 sudo ./omniagent -m 9100

# Logs only — no metrics, no Prometheus endpoint
OAGENT_ENABLE_METRICS=0 OAGENT_ENABLE_OTLP=0 \
  sudo ./omniagent -L loki.internal -Q 3100

# OTLP gateway only — no local scraping
OAGENT_ENABLE_METRICS=0 OAGENT_ENABLE_LOGS=0 \
  sudo ./omniagent -L loki.internal -Q 3100 -m 9100
```

In `viz/docker-compose.yml` the toggles are pre-wired as environment variables
so you can flip them without rebuilding the image:

```yaml
environment:
  - OAGENT_ENABLE_METRICS=1
  - OAGENT_ENABLE_LOGS=1
  - OAGENT_ENABLE_OTLP=1
```

---

## Log filtering

By default OmniAgent watches **every regular file** under `/var/log/` and
`/var/lib/docker/containers/`. On a busy host that can include compressed
archives, rotated files, and binary blobs. Use the filter variables to
narrow what gets shipped:

```bash
# Only ship .log files and the bare "syslog" and "auth.log" files
OAGENT_LOG_INCLUDE="*.log,syslog,auth.log"

# Exclude compressed/rotated files regardless of include list
OAGENT_LOG_EXCLUDE="*.gz,*.bz2,*.zip,*.xz,*.tar,*.1,*.2,*.old"

# Watch only nginx access + error logs
OAGENT_LOG_INCLUDE="access.log,error.log" \
  sudo ./omniagent -L localhost -Q 3100 -m 9100 \
  -l /var/log/nginx
```

Filters are applied at file discovery time (startup) and on each `IN_CREATE`
event (new files), so they have zero runtime overhead once the watch table is seeded.

---

## Production install — systemd

`deploy.sh` is a self-contained bash installer that handles the full lifecycle:

```bash
# First-time install
sudo ./deploy.sh start

# With custom config
sudo ./deploy.sh start -L loki.mycompany.internal -Q 3100 -m 9100 -i 15

# Day-to-day operations
sudo ./deploy.sh status      # service status + current config file
sudo ./deploy.sh logs        # tail -f journald output (Ctrl+C to exit)
sudo ./deploy.sh restart     # rebuild from source and restart
sudo ./deploy.sh stop        # stop + disable autostart
sudo ./deploy.sh uninstall   # remove binary + systemd unit + config
```

What `deploy.sh start` does:
1. Detects your distro and installs `build-essential` + `libzstd-dev` if missing
2. Runs `make -j$(nproc)` from the source directory
3. Copies the binary to `/usr/local/bin/omniagent`
4. Writes runtime config to `/etc/omniagent.conf`
5. Writes a hardened systemd unit to `/etc/systemd/system/omniagent.service`
6. Runs `systemctl daemon-reload && systemctl enable --now omniagent`

Tested distros: Ubuntu 20.04+, Debian 11+, CentOS/RHEL 8+, Amazon Linux 2023,
Fedora 38+, Arch Linux, Alpine Linux.

---

## Visualisation stack

Everything lives in `viz/` and is managed by Docker Compose:

```bash
cd viz/

docker compose up -d          # start all services (build omniagent image too)
docker compose up -d --build  # force rebuild of omniagent image
docker compose down           # stop services (keep data volumes)
docker compose down -v        # stop services and wipe all data volumes
docker compose logs -f        # follow logs from all services
docker compose ps             # check health of all services
```

| Service | Port | Purpose |
|---------|------|---------|
| **Grafana** | `3000` | Dashboard UI — default `admin` / `admin` |
| **Prometheus** | `9090` | Metrics TSDB — scrapes OmniAgent on host `:9100/metrics` |
| **Loki** | `3100` | Log storage — receives direct HTTP pushes from OmniAgent |
| **OmniAgent** | `9100` / `4318` | The agent itself, runs in host network + PID namespace |

Prometheus reaches OmniAgent via `host.docker.internal` (automatically mapped to
the host gateway). OmniAgent uses host networking so it can reach Loki at
`localhost:3100` directly.

---

## Grafana dashboards

12 dashboards are auto-provisioned from `viz/grafana/dashboards/` at startup.
No manual import steps required.

| File | Title | Key panels |
|------|-------|------------|
| `00-summary.json` | **Summary** | CPU %, memory %, uptime, container count, log volume, error rate — one-page KPI overview |
| `00-full-host-overview.json` | **Full Host Overview** | 37 panels — every signal on a single scrollable page |
| `01-host-overview.json` | **Host Overview** | CPU, memory, load, uptime, swap — the essential host panel |
| `02-cpu.json` | **CPU Analysis** | Per-mode breakdown, steal, iowait trends, context switches |
| `03-memory.json` | **Memory Analysis** | Used/free/available/buffers/cached/slab, swap, dirty pages |
| `04-processes.json` | **Process Monitor** | (reserved — process metrics are disabled by default) |
| `05-containers.json` | **Container Monitor** | Per-container CPU + memory, rankings, restart count, OOM events |
| `06-logs.json` | **Logs Overview** | Full log stream, volume by severity, error/warn rate |
| `07-errors.json` | **Error & Warn Logs** | Filtered error/warn streams with rate trends |
| `08-network.json` | **Network** | Per-NIC rx/tx bytes, packets, errors, drops |
| `09-disk.json` | **Disk I/O** | Per-disk reads/writes, bytes, I/O utilisation % |
| `10-filesystem.json` | **Filesystem** | Per-mount used/free/utilisation, inode usage |

All dashboards use the auto-provisioned `prometheus` and `loki` datasources
(`uid: prometheus` and `uid: loki`) — no manual datasource wiring needed.

---

## Pipeline architecture

```
┌──────────────────────────────────────────────────────────────────────────────┐
│  OmniAgent process                                                           │
│                                                                              │
│  RECEIVERS                       PROCESSOR          EXPORTERS                │
│  ─────────                       ─────────          ─────────                │
│                                                                              │
│  procfs_receiver ──┐                               ┌─▶ prometheus_exporter  │
│  (Thread 1)        │                               │    → :9100/metrics     │
│                    │   MPMC Ring    Memory          │    (Prometheus pull)   │
│  docker_receiver ──┼──▶ Buffer ───▶ Limiter ──────┤                        │
│  (Thread 2)        │   (4 096 ×    (64 MB cap)     └─▶ zstd_exporter        │
│                    │    slots,                           → POST /loki/...    │
│  inotify_receiver ─┤    lock-free                        (Loki push)         │
│  (Thread 3)        │    Vyukov                                                │
│                    │    MPMC)       batch_processor                           │
│  otlp_http_recv ───┘               (Thread 5)                                │
│  (Thread 4)                        accumulates 1 000 records                 │
│                                    or 5 s — whichever comes first            │
│                                                                              │
│  All records from a fixed slab pool (8 192 × TelemetryRecord ≈ 11.3 MB BSS) │
└──────────────────────────────────────────────────────────────────────────────┘
```

**Thread count:** 7 — procfs · docker · inotify · otlp_http · batch_processor · prometheus_exporter · prometheus_http

**Key design decisions:**

- **Lock-free hot path** — receiver threads write to the ring buffer with C11 `_Atomic` (Vyukov MPMC algorithm).
  No mutex on the critical path between producers and the batch processor.
- **Slab allocator** — `TelemetryRecord` objects come from a fixed pre-allocated pool.
  RSS is deterministic; there is no heap fragmentation.
- **Backpressure** — if the ring buffer fills up (receivers faster than exporter), new records
  are dropped with a counter increment. Receivers are never blocked.
- **OOM protection** — the memory limiter reads `/proc/self/status VmRSS` before passing each
  record to the batch. Above 64 MB, records are dropped until the exporter catches up.
- **Zero-config discovery** — OmniAgent probes `/proc`, `/var/run/docker.sock`, and `/var/log`
  at startup and silently disables any receiver whose prerequisite is missing.

---

## Memory budget

| Component | RSS |
|-----------|-----|
| Record pool (8 192 × TelemetryRecord at ~1 414 B) | ≈ 11.3 MB (BSS — mostly unpaged) |
| Ring buffer (4 096 × RBSlot) | 64 KB |
| Prometheus text buffer | ~1 MB |
| Loki JSON + HTTP buffer | ~2 MB |
| Thread stacks (7 × 512 KB reserved) | ≈ 768 KB (only touched pages mapped) |
| Code + BSS + misc | ≈ 300 KB |
| **Measured RSS (idle, all features on)** | **≈ 7–8 MB** |

The pool is allocated in BSS (static storage), so most of the 11.3 MB capacity
is never paged in unless the ring buffer actually fills to capacity.

---

## Prometheus metrics reference

All metrics are exposed at `http://<host>:9100/metrics` with the prefix `omniagent_`.

### CPU

| Metric | Labels | Type | Description |
|--------|--------|------|-------------|
| `omniagent_system_cpu_usage_percent` | `host`, `cpu`, `mode` | gauge | CPU time % per mode (user/system/iowait/idle/irq/steal/…) |
| `omniagent_system_cpu_context_switches_total` | `host` | counter | Total context switches since boot |
| `omniagent_system_cpu_interrupts_total` | `host` | counter | Total hardware interrupts since boot |
| `omniagent_system_load_average` | `host`, `interval` | gauge | Load average (1m / 5m / 15m) |
| `omniagent_system_uptime_seconds` | `host` | gauge | Seconds since boot |

### Memory

| Metric | Labels | Type | Description |
|--------|--------|------|-------------|
| `omniagent_system_memory_total_bytes` | `host` | gauge | Total physical RAM |
| `omniagent_system_memory_used_bytes` | `host` | gauge | Used = total − free − buffers − cached |
| `omniagent_system_memory_free_bytes` | `host` | gauge | Kernel-reported free |
| `omniagent_system_memory_available_bytes` | `host` | gauge | Available for allocation without swapping |
| `omniagent_system_memory_buffers_bytes` | `host` | gauge | Kernel buffer cache |
| `omniagent_system_memory_cached_bytes` | `host` | gauge | Page cache |
| `omniagent_system_memory_slab_bytes` | `host` | gauge | Kernel slab allocator |
| `omniagent_system_memory_utilization` | `host` | gauge | used / total (0.0 – 1.0) |
| `omniagent_system_swap_total_bytes` | `host` | gauge | Total swap |
| `omniagent_system_swap_used_bytes` | `host` | gauge | Used swap |

### Disk

| Metric | Labels | Type | Description |
|--------|--------|------|-------------|
| `omniagent_system_disk_read_bytes_total` | `host`, `device` | counter | Bytes read from disk |
| `omniagent_system_disk_write_bytes_total` | `host`, `device` | counter | Bytes written to disk |
| `omniagent_system_disk_reads_completed_total` | `host`, `device` | counter | Read operations completed |
| `omniagent_system_disk_writes_completed_total` | `host`, `device` | counter | Write operations completed |
| `omniagent_system_disk_io_time_seconds_total` | `host`, `device` | counter | Time spent on I/O |

### Network

| Metric | Labels | Type | Description |
|--------|--------|------|-------------|
| `omniagent_system_net_receive_bytes_total` | `host`, `interface` | counter | Bytes received |
| `omniagent_system_net_transmit_bytes_total` | `host`, `interface` | counter | Bytes transmitted |
| `omniagent_system_net_receive_packets_total` | `host`, `interface` | counter | Packets received |
| `omniagent_system_net_transmit_packets_total` | `host`, `interface` | counter | Packets transmitted |
| `omniagent_system_net_receive_errors_total` | `host`, `interface` | counter | Receive errors |
| `omniagent_system_net_transmit_errors_total` | `host`, `interface` | counter | Transmit errors |
| `omniagent_system_net_receive_drop_total` | `host`, `interface` | counter | Dropped received packets |
| `omniagent_system_net_transmit_drop_total` | `host`, `interface` | counter | Dropped transmitted packets |

### Filesystem

| Metric | Labels | Type | Description |
|--------|--------|------|-------------|
| `omniagent_system_filesystem_size_bytes` | `host`, `mountpoint`, `fstype` | gauge | Total filesystem capacity |
| `omniagent_system_filesystem_used_bytes` | `host`, `mountpoint`, `fstype` | gauge | Used bytes |
| `omniagent_system_filesystem_free_bytes` | `host`, `mountpoint`, `fstype` | gauge | Free bytes |
| `omniagent_system_filesystem_utilization` | `host`, `mountpoint`, `fstype` | gauge | used / total (0.0 – 1.0) |
| `omniagent_system_filesystem_inodes_total` | `host`, `mountpoint` | gauge | Total inodes |
| `omniagent_system_filesystem_inodes_used` | `host`, `mountpoint` | gauge | Used inodes |

### Containers (Docker)

| Metric | Labels | Type | Description |
|--------|--------|------|-------------|
| `omniagent_container_cpu_utilization` | `host`, `container_name`, `image` | gauge | CPU % across all cores |
| `omniagent_container_cpu_user_percent` | `host`, `container_name`, `image` | gauge | User-space CPU % |
| `omniagent_container_cpu_system_percent` | `host`, `container_name`, `image` | gauge | Kernel-space CPU % |
| `omniagent_container_cpu_throttled_periods_total` | `host`, `container_name`, `image` | counter | Throttled CPU periods |
| `omniagent_container_memory_usage_bytes` | `host`, `container_name`, `image` | gauge | Working set (usage − cache) |
| `omniagent_container_memory_rss_bytes` | `host`, `container_name`, `image` | gauge | Anonymous RSS |
| `omniagent_container_memory_cache_bytes` | `host`, `container_name`, `image` | gauge | Page cache |
| `omniagent_container_memory_limit_bytes` | `host`, `container_name`, `image` | gauge | Memory limit |
| `omniagent_container_memory_utilization` | `host`, `container_name`, `image` | gauge | usage / limit |
| `omniagent_container_network_receive_bytes_total` | `host`, `container_name`, `interface` | counter | Network RX bytes |
| `omniagent_container_network_transmit_bytes_total` | `host`, `container_name`, `interface` | counter | Network TX bytes |
| `omniagent_container_disk_read_bytes_total` | `host`, `container_name` | counter | Block device reads |
| `omniagent_container_disk_write_bytes_total` | `host`, `container_name` | counter | Block device writes |
| `omniagent_container_restarts_total` | `host`, `container_name`, `image` | gauge | Container restart count |
| `omniagent_container_oom_events_total` | `host`, `container_name`, `image` | counter | OOM kill events |

### Agent internals

| Metric | Labels | Type | Description |
|--------|--------|------|-------------|
| `omniagent_pool_used` | — | gauge | Records currently in use in the slab pool |
| `omniagent_pool_capacity` | — | gauge | Total pool capacity |
| `omniagent_records_dropped_total` | — | counter | Records dropped (ring buffer full) |
| `omniagent_memory_limiter_drops_total` | — | counter | Records dropped by memory limiter |
| `omniagent_rss_kb` | — | gauge | OmniAgent's own RSS in KB |

Scrape endpoint: `http://<host>:9100/metrics`

---

## Loki log format

Logs are pushed to `POST http://<loki_host>:<loki_port>/loki/api/v1/push`
as batches of up to 1 000 log lines flushed every 5 seconds.

### Stream labels

| Label | Value | Example |
|-------|-------|---------|
| `job` | `omniagent` | `omniagent` |
| `host` | OS hostname | `prod-web-01` |
| `source` | Absolute file path | `/var/log/syslog` |
| `level` | Auto-detected severity | `error`, `warn`, `info` |

### Severity detection

Severity is detected from keywords in each log line:

| Keywords in line | Severity |
|-----------------|---------|
| `EMERG`, `ALERT`, `CRIT` | fatal |
| `ERROR`, `error` | error |
| `WARN`, `warn` | warn |
| `DEBUG`, `debug` | debug |
| `TRACE`, `trace` | trace |
| _(no keyword)_ | info |

### Example LogQL queries

```logql
# All logs
{job="omniagent"}

# Errors and above only
{job="omniagent", level=~"error|fatal"}

# Logs from a specific file
{job="omniagent", source="/var/log/nginx/error.log"}

# From a specific host
{job="omniagent", host="prod-web-01"}

# Log rate per 5-minute window
rate({job="omniagent"}[5m])

# Error rate
rate({job="omniagent", level="error"}[5m])

# Full-text search
{job="omniagent"} |= "connection refused"

# Count log volume per source
sum by (source) (count_over_time({job="omniagent"}[1h]))
```

---

## Build targets

```bash
make                  # optimised dynamic build, links libzstd (default)
make static           # fully static binary (requires libzstd.a in path)
make debug            # AddressSanitizer + UBSanitizer + LeakSanitizer
make clean            # remove all build artifacts
make install          # install binary to $PREFIX/bin  (default: /usr/local)
make strip            # strip debug symbols from the binary
make size             # print binary size breakdown
```

The `debug` target enables `-fsanitize=address,undefined,leak` and is
useful for development. Do not run in production — it uses ~6× more memory.

### Cross-compilation

```bash
# Build for ARM64 on an x86 host (requires cross-toolchain)
CC=aarch64-linux-gnu-gcc make -j$(nproc)

# Static build for deployment to containers without libzstd
make static
```

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| `http://localhost:9100/metrics` returns connection refused | OmniAgent not running, or wrong port | Check `systemctl status omniagent` or re-run with `-m 9100` |
| Port 9100 not reachable from another host | Firewall | Open TCP 9100: `ufw allow 9100` or `firewall-cmd --add-port=9100/tcp` |
| Loki `connection refused` at startup | Loki not yet running | Run `docker compose up -d` first, then start OmniAgent |
| `docker_receiver: connect: no such file` | Docker socket not found | Run as root, or `sudo usermod -aG docker $USER` + re-login |
| `inotify: Permission denied on /var/lib/docker/containers` | Not root | Run with `sudo`; Docker logs require root |
| Container metrics stuck at zero | Docker socket not mounted | In Docker: add `-v /var/run/docker.sock:/var/run/docker.sock` |
| High RSS (>15 MB) | Pool filling up | Increase scrape interval: `-i 30`; or check for a log file emitting very fast |
| Build fails: `zstd.h: No such file or directory` | Missing dev header | `sudo apt-get install libzstd-dev` |
| Build fails: `_Atomic` not found | GCC too old | Upgrade to GCC ≥ 7; `sudo apt-get install gcc` |
| No logs in Loki | Log collection disabled | Check `OAGENT_ENABLE_LOGS=1`; verify Loki is reachable |
| Too many log series in Loki | All files being watched | Set `OAGENT_LOG_INCLUDE` or `OAGENT_LOG_EXCLUDE` to filter |
| Grafana dashboards show "No data" | Prometheus/Loki not connected | Check datasource UIDs are `prometheus` and `loki` in Grafana |

---

## Requirements

### Runtime
- Linux kernel ≥ 3.10 (inotify, `/proc`, `accept4`, `pipe2`)
- Root or `docker` group membership for Docker socket access
- `libzstd.so` (dynamic build) — typically `libzstd1` or `libzstd`

### Build
- GCC ≥ 7 or Clang ≥ 6 (C11, `_Atomic`, `__atomic_*`)
- GNU Make
- `libzstd-dev` (dynamic build) or `libzstd.a` (static build)

### Visualisation stack
- Docker Engine ≥ 20.10
- Docker Compose v2 (`docker compose` subcommand)

### Optional
- systemd (for `deploy.sh` managed install)

---

## Contributing

Contributions are welcome. Here are a few areas where help would be valuable:

- **New receivers**: FreeBSD / macOS procfs equivalents, eBPF-based metrics
- **New exporters**: Remote Write to Prometheus, direct InfluxDB line protocol
- **Kubernetes support**: DaemonSet deployment, `/proc/<pid>/ns` namespace awareness
- **Packaging**: `.deb`, `.rpm`, Alpine `apk`, Homebrew formula
- **Tests**: Unit tests for the ring buffer, pool allocator, and telemetry serializer
- **Benchmarks**: Throughput measurements against `node_exporter` + `Promtail`

### Development setup

```bash
# Build with sanitizers for development
make debug

# Run with verbose logging
sudo ./omniagent-debug -L localhost -Q 3100 -m 9100 -d

# Check for memory leaks after Ctrl+C
# (LeakSanitizer report printed automatically with ASAN build)
```

### Code style
- C11 standard, no compiler extensions in production code
- `clang-format` with 4-space indentation (`.clang-format` in progress)
- All public functions documented with a `/* Purpose */` comment
- No dynamic allocation on the hot path (only `pool_alloc` / `pool_free`)
- No global mutable state outside of `pipeline.c` globals

### Pull request checklist
- [ ] `make debug` compiles with zero warnings
- [ ] `make` (release) compiles with zero warnings
- [ ] No new heap allocations in receiver hot paths
- [ ] Tested on at least one of: Ubuntu, Debian, RHEL, or Alpine

---

## License

MIT — see [LICENSE](LICENSE).

```
Copyright (c) 2025 OmniAgent Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
```
