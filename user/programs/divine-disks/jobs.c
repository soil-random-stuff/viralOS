/*
 * Jobs: what a button does, as steps run one after the other while the
 * window stays responsive (job_poll from the event loop). Programs run with
 * their output in a log file; if one fails, the end of its output is what
 * the user sees.
 */

#include "disks.h"

#include <fstab.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <paths.h>

#define LOG_PATH   _PATH_TMP "/.disks-log"
#define EMPTY_PATH _PATH_TMP "/.disks-empty"
#define MOUNTS     _PATH_FSTAB

static struct job job;
static bool running;
static int current;
static long pid;
static uint64_t step_started;
static char status_text[160];

void job_begin(const char *title) {
    memset(&job, 0, sizeof(job));
    snprintf(job.title, sizeof(job.title), "%s", title);
    job.disk = -1;
}

struct job *job_current(void) {
    return &job;
}

void job_run(const char *tool, ...) {
    if (job.count >= JOB_STEPS) {
        return;
    }
    struct step *s = &job.steps[job.count++];
    memset(s, 0, sizeof(*s));
    s->kind = STEP_RUN;
    snprintf(s->args[s->argc++], sizeof(s->args[0]), _PATH_BIN "/%s", tool);
    va_list ap;
    va_start(ap, tool);
    for (const char *a; s->argc < 16 && (a = va_arg(ap, const char *)) != NULL;) {
        snprintf(s->args[s->argc++], sizeof(s->args[0]), "%s", a);
    }
    va_end(ap);
}

void job_step(enum step_kind kind, const char *a, const char *b) {
    if (job.count >= JOB_STEPS) {
        return;
    }
    struct step *s = &job.steps[job.count++];
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    snprintf(s->args[0], sizeof(s->args[0]), "%s", a ? a : "");
    snprintf(s->args[1], sizeof(s->args[1]), "%s", b ? b : "");
}

void job_optional(void) {
    if (job.count) {
        job.steps[job.count - 1].optional = true;
    }
}

bool job_busy(void) {
    return running;
}

const char *job_status(void) {
    return status_text;
}

/* The last few lines a program printed (its error, usually). */
static void log_tail(char *out, size_t size) {
    out[0] = '\0';
    int fd = open(LOG_PATH, O_RDONLY);
    if (fd < 0) {
        return;
    }
    static char buf[4096];
    struct os_file_info info;
    long start = stat(LOG_PATH, &info) == 0 && info.size > sizeof(buf) - 1 ? (long)(info.size - (sizeof(buf) - 1)) : 0;
    seek(fd, start, OS_SEEK_SET);
    long n = 0, r;
    while (n < (long)sizeof(buf) - 1 && (r = read(fd, buf + n, sizeof(buf) - 1 - (size_t)n)) > 0) {
        n += r;
    }
    close(fd);
    buf[n > 0 ? n : 0] = '\0';
    /* Keep the last 6 non-empty lines. */
    int lines = 0;
    char *p = buf + strlen(buf);
    while (p > buf && (p[-1] == '\n' || p[-1] == ' ')) {
        *--p = '\0';
    }
    char *from = buf;
    for (char *c = p; c > buf; c--) {
        if (c[-1] == '\n' && ++lines == 6) {
            from = c;
            break;
        }
    }
    snprintf(out, size, "%s", from);
}

static int start_program(struct step *s) {
    int out = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    int in = open(EMPTY_PATH, O_RDONLY | O_CREAT);
    if (out < 0 || in < 0) {
        if (out >= 0) {
            close(out);
        }
        if (in >= 0) {
            close(in);
        }
        return -EIO;
    }
    const char *argv[17];
    for (int i = 0; i < s->argc; i++) {
        argv[i] = s->args[i];
    }
    argv[s->argc] = NULL;
    struct os_spawn sp = { .path = s->args[0], .argv = argv, .fds = { in, out, out } };
    pid = spawn(&sp);
    close(in);
    close(out);
    return pid < 0 ? (int)pid : 0;
}

static const char *step_name(const struct step *s) {
    static char buf[160];
    switch (s->kind) {
    case STEP_RUN:      snprintf(buf, sizeof(buf), "Running %s", s->args[0] + strlen(_PATH_BIN "/")); break;
    case STEP_UNMOUNT:  snprintf(buf, sizeof(buf), "Unmounting %s", s->args[0]); break;
    case STEP_MOUNT:    snprintf(buf, sizeof(buf), "Mounting %s", s->args[0]); break;
    case STEP_RESCAN:   snprintf(buf, sizeof(buf), "Reading the partition table of %s", s->args[0]); break;
    case STEP_WAIT_DEVICE: snprintf(buf, sizeof(buf), "Waiting for %s", s->args[0]); break;
    case STEP_WRITE_TABLE: snprintf(buf, sizeof(buf), "Writing the partition table"); break;
    case STEP_WIPE:     snprintf(buf, sizeof(buf), "Erasing the partition table"); break;
    case STEP_SAVE_MOUNT_OPTIONS: snprintf(buf, sizeof(buf), "Saving the mount options"); break;
    }
    return buf;
}

/* Start step `current`; returns 0, 1 (finished already) or -errno. */
static int begin_step(void) {
    struct step *s = &job.steps[current];
    snprintf(status_text, sizeof(status_text), "%s...", step_name(s));
    step_started = uptime_ms();
    pid = 0;
    switch (s->kind) {
    case STEP_RUN:
        return start_program(s);
    case STEP_UNMOUNT: {
        int err = unmount(s->args[0]);
        return err == -ENOENT ? 1 : err < 0 ? err : 1;
    }
    case STEP_MOUNT: {
        int err = mount_device(s->args[0], s->args[1][0] ? s->args[1] : NULL);
        return err < 0 ? err : 1;
    }
    case STEP_RESCAN: {
        int err = rescan(s->args[0]);
        return err < 0 ? err : 1;
    }
    case STEP_WAIT_DEVICE:
        return 0;
    case STEP_WRITE_TABLE: {
        int err = job.disk >= 0 ? pt_write(&disks[job.disk], &job.table) : -EINVAL;
        return err < 0 ? err : 1;
    }
    case STEP_WIPE: {
        int err = job.disk >= 0 ? pt_wipe(&disks[job.disk]) : -EINVAL;
        return err < 0 ? err : 1;
    }
    case STEP_SAVE_MOUNT_OPTIONS: {
        int err = mount_options_set(job.mount_uuid, job.mount_path, job.mount_auto, job.mount_rw);
        return err < 0 ? err : 1;
    }
    }
    return -EINVAL;
}

void job_start(void) {
    running = job.count > 0;
    current = 0;
    status_text[0] = '\0';
}

bool job_poll(bool *ok, char *message, size_t size) {
    if (!running) {
        return false;
    }
    for (;;) {
        struct step *s = &job.steps[current];
        int result;
        if (step_started == 0 || status_text[0] == '\0') {
            result = begin_step();
        } else if (s->kind == STEP_RUN) {
            int code = 0;
            long r = wait(pid, &code, WNOHANG);
            if (r == 0) {
                return false;   /* still running */
            }
            result = r < 0 ? (int)r : code == 0 ? 1 : -1000 - code;
        } else if (s->kind == STEP_WAIT_DEVICE) {
            char path[64];
            struct os_file_info info;
            snprintf(path, sizeof(path), _PATH_DEV "/%s", s->args[0]);
            if (stat(path, &info) == 0) {
                result = 1;
            } else if (uptime_ms() - step_started > 6000) {
                result = -ENODEV;
            } else {
                return false;
            }
        } else {
            result = 1;
        }
        if (result == 0) {
            return false;   /* started; check again later */
        }
        if (result < 0 && !s->optional) {
            running = false;
            step_started = 0;
            status_text[0] = '\0';
            *ok = false;
            if (result <= -1000) {
                char tail[1024];
                log_tail(tail, sizeof(tail));
                snprintf(message, size, "%s failed (exit status %d).\n%s", s->args[0] + strlen(_PATH_BIN "/"),
                         -1000 - result, tail);
            } else {
                snprintf(message, size, "%s: %s.", step_name(s), strerror(result));
            }
            return true;
        }
        /* Next step. */
        current++;
        step_started = 0;
        status_text[0] = '\0';
        if (current >= job.count) {
            running = false;
            *ok = true;
            message[0] = '\0';
            return true;
        }
    }
}

/* --- mount options ------------------------------------------------------------------------------------ */

static char *read_mounts(void) {
    static char buf[8192];
    int fd = open(MOUNTS, O_RDONLY);
    long n = 0, r;
    if (fd >= 0) {
        while (n < (long)sizeof(buf) - 1 && (r = read(fd, buf + n, sizeof(buf) - 1 - (size_t)n)) > 0) {
            n += r;
        }
        close(fd);
    }
    buf[n > 0 ? n : 0] = '\0';
    return buf;
}

bool mount_options_get(const char *uuid, char *path, size_t size, bool *automatic, bool *rw) {
    if (!uuid[0]) {
        return false;
    }
    char *text = read_mounts();
    static struct fstab_entry e;
    for (char *line = text; line && *line;) {
        char *end = strchr(line, '\n');
        if (end) {
            *end = '\0';
        }
        if (fstab_parse(line, &e) && strcmp(e.uuid, uuid) == 0) {
            strlcpy(path, e.path, size);
            *automatic = !e.noauto;
            *rw = e.rw;
            return true;
        }
        line = end ? end + 1 : NULL;
    }
    return false;
}

/* Does this fstab line (up to its end) belong to the filesystem with this UUID? */
static bool line_is(const char *line, size_t len, const char *uuid) {
    char copy[1200];
    static struct fstab_entry e;
    if (len >= sizeof(copy)) {
        return false;
    }
    memcpy(copy, line, len);
    copy[len] = '\0';
    return fstab_parse(copy, &e) && strcmp(e.uuid, uuid) == 0;
}

int mount_options_set(const char *uuid, const char *path, bool automatic, bool rw) {
    if (!uuid[0]) {
        return -EINVAL;
    }
    char *text = read_mounts();
    static char out[8192];
    size_t o = 0;
    if (!text[0]) {
        o += (size_t)snprintf(out, sizeof(out), "# Where drives get mounted (Disks > Edit Mount Options).\n"
                                                "# <filesystem>\t<mount point>\t<type>\t<options>\t<dump>\t<pass>\n");
    }
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (!line_is(line, len, uuid) && len && o + len + 2 < sizeof(out)) {
            memcpy(out + o, line, len);
            o += len;
            out[o++] = '\n';
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }
    size_t n = fstab_format(out + o, sizeof(out) - o, uuid, path, !automatic, rw);
    if (n == 0) {
        return -ENOSPC;
    }
    o += n;
    int fd = open(MOUNTS, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        return fd;
    }
    long w = write(fd, out, o);
    close(fd);
    return w == (long)o ? 0 : -EIO;
}
