/*
 * omniagent/include/omniagent.h
 *
 * Master convenience header — include this in every .c file.
 * Brings in all public interfaces plus standard C11 / POSIX headers.
 */

#ifndef OMNIAGENT_H
#define OMNIAGENT_H

/* ── C11 / POSIX feature macros ─────────────────────────────────────────── */
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE   /* for ppoll, accept4, pipe2, etc.                  */
#endif

/* ── Standard library ───────────────────────────────────────────────────── */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── POSIX / Linux ──────────────────────────────────────────────────────── */
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ── Project headers ────────────────────────────────────────────────────── */
#include "telemetry.h"
#include "ring_buffer.h"
#include "pool.h"
#include "pipeline.h"

/* ── Logging macros ─────────────────────────────────────────────────────── */

#define LOG_INFO(fmt, ...)  \
    fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__)

#define LOG_WARN(fmt, ...)  \
    fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    fprintf(stderr, "[ERROR] %s:%d  " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

#define LOG_DEBUG(fmt, ...) \
    do { if (g_debug_mode) \
        fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__); } while (0)

extern int g_debug_mode;  /* set via -d flag in main.c */

/* ── Utility macros ─────────────────────────────────────────────────────── */

#define ARRAY_LEN(a)   (sizeof(a) / sizeof((a)[0]))

/* Saturating string copy — always NUL-terminates dst. */
#define SAFE_STRNCPY(dst, src, n) \
    do { strncpy((dst), (src), (n) - 1); (dst)[(n) - 1] = '\0'; } while (0)

/*
 * OAGENT_UNUSED — suppress unused-parameter warnings on purpose-built stubs.
 */
#define OAGENT_UNUSED(x) ((void)(x))

/* ── Receiver thread entry-point prototypes ─────────────────────────────── */

void *procfs_receiver_thread(void *arg);
void *docker_receiver_thread(void *arg);
void *inotify_receiver_thread(void *arg);
void *otlp_http_receiver_thread(void *arg);
void *batch_processor_thread(void *arg);
void *prometheus_exporter_thread(void *arg);
void *prometheus_http_thread(void *arg);

#endif /* OMNIAGENT_H */
