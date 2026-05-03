/*
 * omniagent/src/inotify_receiver.c
 *
 * Log Tailer — uses the Linux inotify API to watch log files for changes.
 * When a watched file is modified (IN_MODIFY event), we seek to the last
 * known offset and read only the new bytes, emitting each complete line
 * as a LogRecord into the pipeline.
 *
 * Directories watched by default:
 *   /var/log/                         — system logs (syslog, kern.log, etc.)
 *   /var/lib/docker/containers/       — Docker container stdout/stderr
 *
 * Additional directories can be supplied via -l on the command line
 * (colon-separated paths in g_config.extra_log_dirs).
 *
 * Design notes
 * ────────────
 * • inotify_add_watch() with IN_MODIFY | IN_CREATE is called on each
 *   watched directory.  New files created after startup are picked up
 *   automatically via IN_CREATE events.
 *
 * • We track per-file read offsets in a small table (MAX_WATCHED_FILES).
 *   On IN_MODIFY we open the file, fseek to the saved offset, read new
 *   data, and update the offset.
 *
 * • Rotated files (IN_DELETE_SELF / IN_MOVE_SELF) are detected and the
 *   tracking entry is removed so we re-open on the next CREATE.
 *
 * • We intentionally limit lines to OAGENT_LOG_BODY_MAX to keep records
 *   in the fixed pool.  Longer lines are truncated with a suffix "...[truncated]".
 *
 * • The inotify fd is placed in non-blocking mode and we use poll() with
 *   a timeout so that the thread can respond to the g_running flag promptly.
 */

#include "omniagent.h"
#include <sys/inotify.h>
#include <poll.h>
#include <dirent.h>
#include <fnmatch.h>

/* ── Constants ───────────────────────────────────────────────────────────── */

#define MAX_WATCHED_DIRS    16
#define MAX_WATCHED_FILES  256
#define INOTIFY_EVENT_BUF  (4096 * (sizeof(struct inotify_event) + NAME_MAX + 1))
#define POLL_TIMEOUT_MS    500

/* ── File tracking table ─────────────────────────────────────────────────── */

typedef struct {
    int     wd;             /* inotify watch descriptor of the parent dir   */
    char    path[PATH_MAX]; /* absolute path to the file                    */
    off_t   offset;         /* last read position                           */
    int     active;         /* 1 = slot in use                              */
} FileTrack;

static FileTrack s_files[MAX_WATCHED_FILES];
static int       s_inotify_fd = -1;

/* ── Find or create a FileTrack entry ───────────────────────────────────── */

static FileTrack *track_find(const char *path)
{
    for (int i = 0; i < MAX_WATCHED_FILES; i++) {
        if (s_files[i].active && strcmp(s_files[i].path, path) == 0) {
            return &s_files[i];
        }
    }
    return NULL;
}

static FileTrack *track_alloc(int wd, const char *path)
{
    for (int i = 0; i < MAX_WATCHED_FILES; i++) {
        if (!s_files[i].active) {
            s_files[i].wd     = wd;
            s_files[i].offset = 0;
            s_files[i].active = 1;
            SAFE_STRNCPY(s_files[i].path, path, PATH_MAX);
            return &s_files[i];
        }
    }
    LOG_WARN("inotify: file tracking table full (MAX_WATCHED_FILES=%d)",
             MAX_WATCHED_FILES);
    return NULL;
}

static void track_remove(const char *path)
{
    for (int i = 0; i < MAX_WATCHED_FILES; i++) {
        if (s_files[i].active && strcmp(s_files[i].path, path) == 0) {
            memset(&s_files[i], 0, sizeof(s_files[i]));
            return;
        }
    }
}

/* ── Detect log severity from line prefix ────────────────────────────────── */

static LogSeverity detect_severity(const char *line)
{
    /* Syslog / journald patterns */
    if (strstr(line, "EMERG") || strstr(line, "emerg")) return LOG_SEVERITY_FATAL;
    if (strstr(line, "ALERT") || strstr(line, "CRIT"))  return LOG_SEVERITY_FATAL;
    if (strstr(line, "ERROR") || strstr(line, "error")) return LOG_SEVERITY_ERROR;
    if (strstr(line, "WARN")  || strstr(line, "warn"))  return LOG_SEVERITY_WARN;
    if (strstr(line, "DEBUG") || strstr(line, "debug")) return LOG_SEVERITY_DEBUG;
    if (strstr(line, "TRACE") || strstr(line, "trace")) return LOG_SEVERITY_TRACE;
    return LOG_SEVERITY_INFO;
}

/* ── Read new lines from a modified file ─────────────────────────────────── */

static void read_new_lines(FileTrack *ft)
{
    FILE *f = fopen(ft->path, "r");
    if (!f) {
        /* File may have been rotated/deleted between the event and open */
        track_remove(ft->path);
        return;
    }

    /* Seek to the last known offset */
    if (fseeko(f, ft->offset, SEEK_SET) != 0) {
        /* File was truncated (e.g., log rotation with truncate) — reset */
        ft->offset = 0;
        rewind(f);
    }

    char line[OAGENT_LOG_BODY_MAX + 32];  /* +32 for truncation marker room */

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);

        /* Strip trailing newline */
        if (len > 0 && line[len - 1] == '\n') {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        /* Emit as a LogRecord */
        TelemetryRecord *rec = pool_alloc(&g_pool);
        if (!rec) {
            LOG_DEBUG("inotify: pool exhausted, dropping log line");
            continue;
        }

        rec->type = TELEM_LOG;
        rec->log.timestamp_ns = telem_now_ns();
        rec->log.severity     = detect_severity(line);

        /* Truncate body if needed */
        if (len >= OAGENT_LOG_BODY_MAX) {
            len = OAGENT_LOG_BODY_MAX - 16;
            SAFE_STRNCPY(rec->log.body, line, len + 1);
            strncat(rec->log.body, "...[truncated]",
                    OAGENT_LOG_BODY_MAX - len - 1);
        } else {
            SAFE_STRNCPY(rec->log.body, line, OAGENT_LOG_BODY_MAX);
        }

        /* Source is the file path (trimmed to fit) */
        SAFE_STRNCPY(rec->log.source, ft->path, OAGENT_SOURCE_MAX);

        /* Resource: host */
        char hostname[64] = "unknown";
        gethostname(hostname, sizeof(hostname) - 1);
        SAFE_STRNCPY(rec->host_name,    hostname,    OAGENT_RESOURCE_MAX);
        SAFE_STRNCPY(rec->service_name, "omniagent", OAGENT_RESOURCE_MAX);

        if (!rb_enqueue(&g_recv_queue, rec)) {
            pool_free(&g_pool, rec);
            LOG_DEBUG("inotify: ring buffer full, dropped log line");
        }
    }

    ft->offset = ftello(f);
    fclose(f);
}
/* ── Log file filter ────────────────────────────────────────────────────── */

/*
 * Returns 1 if the file should be watched, 0 if it should be skipped.
 * Applies OAGENT_LOG_INCLUDE and OAGENT_LOG_EXCLUDE patterns (fnmatch,
 * matched against the basename of the path).
 */
static int file_matches_filter(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    /* Include filter: must match at least one pattern (if set) */
    if (g_config.log_include[0]) {
        char buf[sizeof(g_config.log_include)];
        SAFE_STRNCPY(buf, g_config.log_include, sizeof(buf));
        int matched = 0;
        char *tok = strtok(buf, ",");
        while (tok) {
            if (fnmatch(tok, base, 0) == 0) { matched = 1; break; }
            tok = strtok(NULL, ",");
        }
        if (!matched) return 0;
    }

    /* Exclude filter: skip if matches any pattern */
    if (g_config.log_exclude[0]) {
        char buf[sizeof(g_config.log_exclude)];
        SAFE_STRNCPY(buf, g_config.log_exclude, sizeof(buf));
        char *tok = strtok(buf, ",");
        while (tok) {
            if (fnmatch(tok, base, 0) == 0) return 0;
            tok = strtok(NULL, ",");
        }
    }

    return 1;
}
/* ── Resolve wd → directory path ─────────────────────────────────────────── */

typedef struct {
    int  wd;
    char dir[PATH_MAX];
} WdMap;

static WdMap   s_wd_map[MAX_WATCHED_DIRS];
static int     s_wd_count = 0;

static const char *wd_to_dir(int wd)
{
    for (int i = 0; i < s_wd_count; i++) {
        if (s_wd_map[i].wd == wd) return s_wd_map[i].dir;
    }
    return NULL;
}

/* ── Add a directory watch + seed existing files ─────────────────────────── */

static void watch_directory(const char *dir_path)
{
    if (s_wd_count >= MAX_WATCHED_DIRS) {
        LOG_WARN("inotify: too many directories (max %d)", MAX_WATCHED_DIRS);
        return;
    }

    int wd = inotify_add_watch(s_inotify_fd, dir_path,
                               IN_MODIFY | IN_CREATE |
                               IN_DELETE | IN_MOVED_FROM);
    if (wd < 0) {
        if (errno == ENOENT) {
            LOG_INFO("inotify: directory not found, skipping: %s", dir_path);
        } else {
            LOG_WARN("inotify: inotify_add_watch(%s): %s",
                     dir_path, strerror(errno));
        }
        return;
    }

    s_wd_map[s_wd_count].wd = wd;
    SAFE_STRNCPY(s_wd_map[s_wd_count].dir, dir_path, PATH_MAX);
    s_wd_count++;

    LOG_INFO("inotify: watching %s (wd=%d)", dir_path, wd);

    /*
     * Seed the tracking table with files already present in the directory.
     * We do NOT read existing content on startup — only new lines written
     * after the agent starts are emitted.  This prevents flooding the
     * pipeline with historical data.
     */
    DIR *d = opendir(dir_path);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, de->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        if (!file_matches_filter(full_path)) continue;
        FileTrack *ft = track_alloc(wd, full_path);
        if (ft) {
            /* Start at the current end of file — don't re-read old lines. */
            ft->offset = st.st_size;
        }
    }
    closedir(d);
}

/* ── Process a single inotify event ─────────────────────────────────────── */

static void handle_inotify_event(const struct inotify_event *ev)
{
    /* Skip events without a filename (e.g., on the watched file itself) */
    if (!(ev->mask & IN_ISDIR) && ev->len > 0) {
        const char *dir = wd_to_dir(ev->wd);
        if (!dir) return;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, ev->name);

        if (ev->mask & IN_CREATE) {
            /* New file — add to tracking table at offset 0 */
            struct stat st;
            if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
                if (!track_find(full_path) && file_matches_filter(full_path)) {
                    track_alloc(ev->wd, full_path);
                    LOG_DEBUG("inotify: new file: %s", full_path);
                }
            }
        }

        if (ev->mask & IN_MODIFY) {
            FileTrack *ft = track_find(full_path);
            if (!ft && file_matches_filter(full_path)) {
                /* We saw a modify before a create (possible on some kernels) */
                ft = track_alloc(ev->wd, full_path);
            }
            if (ft) {
                read_new_lines(ft);
            }
        }

        if ((ev->mask & IN_DELETE) || (ev->mask & IN_MOVED_FROM)) {
            track_remove(full_path);
            LOG_DEBUG("inotify: file removed: %s", full_path);
        }
    }
}

/* ── Receiver thread ─────────────────────────────────────────────────────── */

void *inotify_receiver_thread(void *arg)
{
    OAGENT_UNUSED(arg);

    s_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (s_inotify_fd < 0) {
        LOG_ERROR("inotify: inotify_init1: %s", strerror(errno));
        return NULL;
    }

    memset(s_files,  0, sizeof(s_files));
    memset(s_wd_map, 0, sizeof(s_wd_map));

    /* Default watched directories */
    watch_directory("/var/log");
    watch_directory("/var/lib/docker/containers");

    /* Extra directories from CLI */
    if (g_config.extra_log_dirs[0]) {
        char dirs_copy[sizeof(g_config.extra_log_dirs)];
        SAFE_STRNCPY(dirs_copy, g_config.extra_log_dirs, sizeof(dirs_copy));
        char *token = strtok(dirs_copy, ":");
        while (token) {
            watch_directory(token);
            token = strtok(NULL, ":");
        }
    }

    /* I/O event buffer — sized for many events at once */
    char *event_buf = malloc(INOTIFY_EVENT_BUF);
    if (!event_buf) {
        LOG_ERROR("inotify: cannot allocate event buffer");
        close(s_inotify_fd);
        return NULL;
    }

    LOG_INFO("inotify_receiver: started, watching %d directories", s_wd_count);

    struct pollfd pfd = {
        .fd     = s_inotify_fd,
        .events = POLLIN,
    };

    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        int ready = poll(&pfd, 1, POLL_TIMEOUT_MS);

        if (ready < 0) {
            if (errno == EINTR) continue;  /* signal interrupt — check g_running */
            LOG_ERROR("inotify: poll: %s", strerror(errno));
            break;
        }

        if (ready == 0) continue;  /* timeout — loop back and check g_running */

        if (!(pfd.revents & POLLIN)) continue;

        /* Read as many events as available in one call */
        ssize_t n = read(s_inotify_fd, event_buf, INOTIFY_EVENT_BUF);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            LOG_ERROR("inotify: read events: %s", strerror(errno));
            break;
        }

        /* Walk the event buffer — events are variable-length structs */
        char *ptr = event_buf;
        char *end = event_buf + n;
        while (ptr < end) {
            struct inotify_event *ev = (struct inotify_event *)ptr;
            handle_inotify_event(ev);
            ptr += sizeof(struct inotify_event) + ev->len;
        }
    }

    free(event_buf);
    close(s_inotify_fd);
    s_inotify_fd = -1;

    LOG_INFO("inotify_receiver: exiting");
    return NULL;
}
