/*
 * Running user programs from the shell:
 *
 *   name args...        run /usr/bin/name (or a path, if it contains '/')
 *   ... > file          stdout (and stderr) to a file, replacing it
 *   ... >> file         stdout appended to a file
 *   ... < file          stdin from a file
 *   ... &               run in the background
 *   "quoted words"      keep spaces inside one argument
 */

#include <stdbool.h>
#include <stddef.h>

#include "shell/commands.h"
#include "fs/vfs.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "proc/file.h"
#include "proc/process.h"
#include <paths.h>

#define PROGRAM_DIR _PATH_BIN "/"

/* Next token from *cursor, splitting on spaces and honouring double quotes. NULL at the end. */
static char *next_token(char **cursor) {
    char *s = *cursor;
    while (*s == ' ') {
        s++;
    }
    if (!*s) {
        return NULL;
    }
    char *start = s, *out = s;
    bool quoted = false;
    while (*s && (quoted || *s != ' ')) {
        if (*s == '"') {
            quoted = !quoted;
            s++;
            continue;
        }
        *out++ = *s++;
    }
    if (*s) {
        s++;
    }
    *out = '\0';
    *cursor = s;
    return start;
}

static bool has_slash(const char *s) {
    for (; *s; s++) {
        if (*s == '/') {
            return true;
        }
    }
    return false;
}

static struct file *open_redirect(const char *path, int flags) {
    int err;
    struct file *f = file_open(shell_cwd, path, flags, &err);
    if (!f) {
        kprintf("%s: %s\n", path, strerror(err));
    }
    return f;
}

void run_program(char *line) {
    char *argv[PROCESS_MAX_ARGS];
    int argc = 0;
    const char *in_path = NULL, *out_path = NULL;
    bool append = false, background = false;

    char *cursor = line, *tok;
    while ((tok = next_token(&cursor))) {
        if (tok[0] == '>' || tok[0] == '<') {
            bool is_out = tok[0] == '>';
            bool dbl = is_out && tok[1] == '>';
            char *target = tok + (dbl ? 2 : 1);
            if (!*target) {
                target = next_token(&cursor);
            }
            if (!target) {
                kprintf("syntax error: missing file name after '%c'\n", tok[0]);
                return;
            }
            if (is_out) {
                out_path = target;
                append = dbl;
            } else {
                in_path = target;
            }
        } else if (strcmp(tok, "&") == 0) {
            background = true;
        } else if (argc < PROCESS_MAX_ARGS) {
            argv[argc++] = tok;
        } else {
            kprintf("too many arguments (max %d)\n", PROCESS_MAX_ARGS);
            return;
        }
    }
    if (argc == 0) {
        return;
    }

    char path[VFS_PATH_MAX];
    if (has_slash(argv[0])) {
        strlcpy(path, argv[0], sizeof(path));
    } else {
        strlcpy(path, PROGRAM_DIR, sizeof(path));
        strlcpy(path + strlen(PROGRAM_DIR), argv[0], sizeof(path) - strlen(PROGRAM_DIR));
    }

    struct spawn_options opts = { .cwd = shell_cwd, .detached = background };
    if (in_path && !(opts.files[0] = open_redirect(in_path, O_RDONLY))) {
        return;
    }
    if (out_path) {
        int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
        if (!(opts.files[1] = open_redirect(out_path, flags))) {
            if (opts.files[0]) {
                file_put(opts.files[0]);
            }
            return;
        }
    }

    int err;
    struct process *p = process_spawn(path, argc, argv, &opts, &err);
    /* The process took its own references to these. */
    for (int i = 0; i < 3; i++) {
        if (opts.files[i]) {
            file_put(opts.files[i]);
        }
    }

    if (!p) {
        if (err == -ENOENT && !has_slash(argv[0])) {
            kprintf("%s: command not found\n", argv[0]);
        } else {
            kprintf("%s: %s\n", argv[0], strerror(err));
        }
        return;
    }

    if (background) {
        kprintf("[%lu] %s running in the background\n", opts.pid, argv[0]);
        return;
    }
    process_set_foreground(p);
    int code = process_wait(p);
    process_set_foreground(NULL);
    if (code != 0) {
        kprintf("[exit status %d]\n", code);
    }
}
