/*
 * omniagent/src/main.c
 *
 * OmniAgent entry point.
 *
 * Responsibilities:
 *   1. Parse command-line arguments into AgentConfig.
 *   2. Auto-detect available receivers (Docker socket, inotify support).
 *   3. Initialise the pipeline (pool, ring buffer, export queue).
 *   4. Spawn receiver threads, the batch-processor thread, and the exporter.
 *   5. Install SIGTERM / SIGINT handlers for clean shutdown.
 *   6. Wait for shutdown signal, then join all threads.
 *
 * Usage:
 *   omniagent [options]
 *
 *   -e <host>   Export endpoint host  (default: localhost)
 *   -p <port>   Export endpoint port  (default: 4318)
 *   -t <token>  Auth token / API key
 *   -l <dirs>   Extra log dirs (colon-separated) watched via inotify
 *   -z <level>  Zstd compression level 1–22 (default: 3)
 *   -i <secs>   procfs scrape interval in seconds (default: 5)
 *   -d          Enable debug logging
 *   -h          Print help
 */

#include "omniagent.h"
#include <getopt.h>

/* ── Thread handle array ──────────────────────────────────────────────────── */

#define MAX_THREADS 8

static pthread_t g_threads[MAX_THREADS];
static size_t    g_thread_count = 0;

/* ── Signal handling ─────────────────────────────────────────────────────── */

static void signal_handler(int signo)
{
    /*
     * Signal handlers must be async-signal-safe.
     * atomic_store with relaxed ordering is safe here — the main thread
     * will see the change when it next checks g_running.
     */
    OAGENT_UNUSED(signo);
    atomic_store_explicit(&g_running, 0, memory_order_relaxed);
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    /* Do NOT set SA_RESTART so that blocking syscalls (accept, read) wake up */
    sa.sa_flags = 0;

    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        LOG_ERROR("sigaction(SIGTERM): %s", strerror(errno));
    }
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        LOG_ERROR("sigaction(SIGINT): %s", strerror(errno));
    }
    /* Ignore SIGPIPE — we handle write errors explicitly. */
    signal(SIGPIPE, SIG_IGN);
}

/* ── Auto-detect helpers ─────────────────────────────────────────────────── */

static int probe_docker_socket(void)
{
    struct stat st;
    if (stat("/var/run/docker.sock", &st) == 0
        && (st.st_mode & S_IFSOCK))
    {
        return 1;
    }
    LOG_INFO("docker: /var/run/docker.sock not found — receiver disabled");
    return 0;
}

static int probe_proc(void)
{
    struct stat st;
    if (stat("/proc/stat", &st) == 0) {
        return 1;
    }
    LOG_WARN("procfs: /proc/stat not found — receiver disabled");
    return 0;
}

static int probe_inotify(void)
{
    /* inotify is a Linux kernel feature — always available on Linux 2.6.13+.
     * Just verify that /var/log exists; if not, there's nothing to watch. */
    struct stat st;
    if (stat("/var/log", &st) == 0 && S_ISDIR(st.st_mode)) {
        return 1;
    }
    LOG_WARN("inotify: /var/log not found — receiver disabled");
    return 0;
}

/* ── Spawn helper ────────────────────────────────────────────────────────── */

static int spawn_thread(void *(*fn)(void *), void *arg, const char *name)
{
    if (g_thread_count >= MAX_THREADS) {
        LOG_ERROR("thread limit reached, cannot spawn %s", name);
        return -1;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    /*
     * Reduce stack size from the default 8 MB to 512 KB.
     * Our receiver threads touch only a few KB of stack per call.
     * This is a significant contributor to keeping RSS under 10 MB.
     */
    pthread_attr_setstacksize(&attr, 512 * 1024);

    int rc = pthread_create(&g_threads[g_thread_count], &attr, fn, arg);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        LOG_ERROR("pthread_create(%s): %s", name, strerror(rc));
        return -1;
    }

    LOG_INFO("spawned thread: %s (tid=%zu)", name, g_thread_count);
    g_thread_count++;
    return 0;
}

/* ── Environment variable configuration ─────────────────────────────────── */

static void config_apply_env(void)
{
    const char *v;

    /* Endpoint / port configuration */
    if ((v = getenv("OAGENT_LOKI_HOST")) && v[0])
        SAFE_STRNCPY(g_config.loki_host, v, sizeof(g_config.loki_host));
    if ((v = getenv("OAGENT_LOKI_PORT"))) {
        long p = strtol(v, NULL, 10);
        if (p > 0 && p <= 65535) g_config.loki_port = (int)p;
    }
    if ((v = getenv("OAGENT_METRICS_PORT"))) {
        long p = strtol(v, NULL, 10);
        if (p > 0 && p <= 65535) g_config.metrics_port = (int)p;
    }
    if ((v = getenv("OAGENT_SCRAPE_INTERVAL"))) {
        long s = strtol(v, NULL, 10);
        if (s >= 1 && s <= 3600) g_config.procfs_interval_s = (int)s;
    }

    /* Extra log directories */
    if ((v = getenv("OAGENT_LOG_DIRS")) && v[0])
        SAFE_STRNCPY(g_config.extra_log_dirs, v, sizeof(g_config.extra_log_dirs));

    /* Feature toggles — set env var to "0" to disable */
    if ((v = getenv("OAGENT_ENABLE_METRICS")) && strcmp(v, "0") == 0)
        g_config.enable_metrics = 0;
    if ((v = getenv("OAGENT_ENABLE_LOGS")) && strcmp(v, "0") == 0)
        g_config.enable_logs = 0;
    if ((v = getenv("OAGENT_ENABLE_OTLP")) && strcmp(v, "0") == 0)
        g_config.enable_otlp = 0;

    /* Log file name filters (fnmatch patterns, comma-separated) */
    if ((v = getenv("OAGENT_LOG_INCLUDE")) && v[0])
        SAFE_STRNCPY(g_config.log_include, v, sizeof(g_config.log_include));
    if ((v = getenv("OAGENT_LOG_EXCLUDE")) && v[0])
        SAFE_STRNCPY(g_config.log_exclude, v, sizeof(g_config.log_exclude));

    /* Debug mode */
    if ((v = getenv("OAGENT_DEBUG")) && strcmp(v, "1") == 0)
        g_debug_mode = 1;
}

/* ── Usage ───────────────────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stdout,
        "OmniAgent — ultra-lightweight observability daemon\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "  -L <host>    Loki endpoint host          (default: localhost)\n"
        "  -Q <port>    Loki endpoint port          (default: 3100)\n"
        "  -m <port>    Prometheus metrics port     (default: 9100)\n"
        "  -l <dirs>    Extra log dirs (colon-separated) for inotify\n"
        "  -i <secs>    procfs scrape interval s    (default: 5)\n"
        "  -d           Enable debug logging\n"
        "  -h           Print this help\n"
        "\n"
        "Environment variables (CLI flags take precedence):\n"
        "  OAGENT_LOKI_HOST        Loki host                (same as -L)\n"
        "  OAGENT_LOKI_PORT        Loki port                (same as -Q)\n"
        "  OAGENT_METRICS_PORT     Prometheus port          (same as -m)\n"
        "  OAGENT_SCRAPE_INTERVAL  procfs interval seconds  (same as -i)\n"
        "  OAGENT_LOG_DIRS         Extra log dirs           (same as -l)\n"
        "  OAGENT_DEBUG            1 = enable debug mode    (same as -d)\n"
        "\n"
        "Feature toggles (set to 0 to disable, default is 1=on):\n"
        "  OAGENT_ENABLE_METRICS   Host + container metrics + /metrics\n"
        "  OAGENT_ENABLE_LOGS      Log file collection → Loki\n"
        "  OAGENT_ENABLE_OTLP      OTLP/HTTP receiver on :4318\n"
        "\n"
        "Log filtering (applied to file basename, fnmatch patterns):\n"
        "  OAGENT_LOG_INCLUDE      Comma-sep patterns to include\n"
        "                          (empty = all files, e.g. *.log,syslog)\n"
        "  OAGENT_LOG_EXCLUDE      Comma-sep patterns to exclude\n"
        "                          (e.g. *.gz,*.bz2,*.zip,*.1)\n",
        prog);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    /* ── Defaults ── */
    memset(&g_config, 0, sizeof(g_config));
    SAFE_STRNCPY(g_config.loki_host, "localhost",
                 sizeof(g_config.loki_host));
    g_config.loki_port         = 3100;
    g_config.metrics_port      = 9100;
    g_config.procfs_interval_s = 5;
    g_config.enable_metrics    = 1;
    g_config.enable_logs       = 1;
    g_config.enable_otlp       = 1;

    /* ── Apply environment variables (CLI args override below) ── */
    config_apply_env();

    /* ── Parse CLI ── */
    int opt;
    while ((opt = getopt(argc, argv, "L:Q:m:l:i:dh")) != -1) {
        switch (opt) {
        case 'L':
            SAFE_STRNCPY(g_config.loki_host, optarg,
                         sizeof(g_config.loki_host));
            break;
        case 'Q': {
            long v = strtol(optarg, NULL, 10);
            if (v <= 0 || v > 65535) {
                fprintf(stderr, "Invalid Loki port: %s\n", optarg);
                return EXIT_FAILURE;
            }
            g_config.loki_port = (int)v;
            break;
        }
        case 'm': {
            long v = strtol(optarg, NULL, 10);
            if (v <= 0 || v > 65535) {
                fprintf(stderr, "Invalid metrics port: %s\n", optarg);
                return EXIT_FAILURE;
            }
            g_config.metrics_port = (int)v;
            break;
        }
        case 'l':
            SAFE_STRNCPY(g_config.extra_log_dirs, optarg,
                         sizeof(g_config.extra_log_dirs));
            break;
        case 'i': {
            long v = strtol(optarg, NULL, 10);
            if (v < 1 || v > 3600) {
                fprintf(stderr, "Scrape interval must be 1-3600 seconds\n");
                return EXIT_FAILURE;
            }
            g_config.procfs_interval_s = (int)v;
            break;
        }
        case 'd':
            g_debug_mode = 1;
            break;
        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    LOG_INFO("OmniAgent starting — metrics=:%d loki=%s:%d",
             g_config.metrics_port,
             g_config.loki_host[0] ? g_config.loki_host : "(disabled)",
             g_config.loki_port);

    /* ── Signal handlers ── */
    install_signal_handlers();

    /* ── Pipeline initialisation ── */
    if (pipeline_init() != 0) {
        LOG_ERROR("pipeline_init failed");
        return EXIT_FAILURE;
    }

    /* ── Auto-detect receivers, then apply feature flags ── */
    g_config.enable_procfs    = g_config.enable_metrics ? probe_proc()          : 0;
    g_config.enable_docker    = g_config.enable_metrics ? probe_docker_socket() : 0;
    g_config.enable_inotify   = g_config.enable_logs    ? probe_inotify()       : 0;
    g_config.enable_otlp_http = g_config.enable_otlp    ? 1                     : 0;

    LOG_INFO("features: metrics=%d logs=%d otlp=%d",
             g_config.enable_metrics, g_config.enable_logs, g_config.enable_otlp);

    /* ── Spawn receiver threads ── */
    if (g_config.enable_procfs) {
        if (spawn_thread(procfs_receiver_thread, NULL, "procfs_receiver") != 0)
            return EXIT_FAILURE;
    }
    if (g_config.enable_docker) {
        if (spawn_thread(docker_receiver_thread, NULL, "docker_receiver") != 0)
            return EXIT_FAILURE;
    }
    if (g_config.enable_inotify) {
        if (spawn_thread(inotify_receiver_thread, NULL, "inotify_receiver") != 0)
            return EXIT_FAILURE;
    }
    if (g_config.enable_otlp_http) {
        if (spawn_thread(otlp_http_receiver_thread, NULL, "otlp_http_receiver") != 0)
            return EXIT_FAILURE;
    }

    /* ── Spawn processing / export threads ── */
    if (spawn_thread(batch_processor_thread, NULL, "batch_processor") != 0)
        return EXIT_FAILURE;
    if (g_config.enable_metrics) {
        if (spawn_thread(prometheus_exporter_thread, NULL, "prometheus_exporter") != 0)
            return EXIT_FAILURE;
        if (spawn_thread(prometheus_http_thread, NULL, "prometheus_http") != 0)
            return EXIT_FAILURE;
    }

    LOG_INFO("all threads running — PID %d, waiting for SIGTERM/SIGINT",
             (int)getpid());

    /* ── Main loop: wait for shutdown signal ── */
    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        /* Sleep in 100 ms increments so signal wakes us quickly. */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L };
        nanosleep(&ts, NULL);
    }

    LOG_INFO("shutdown signal received");

    /* ── Graceful shutdown ── */
    pipeline_shutdown(g_threads, g_thread_count);

    LOG_INFO("OmniAgent exited cleanly");
    return EXIT_SUCCESS;
}
