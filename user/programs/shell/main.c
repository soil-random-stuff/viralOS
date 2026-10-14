/*
 * shell: the user's command line.
 *
 *   name args...        run /usr/bin/name (or a path, if it contains '/')
 *   ... > file          stdout to a file, replacing it (>> appends)
 *   ... < file          stdin from a file
 *   ... &               run in the background
 *   "quoted words"      keep spaces inside one argument
 *   ~ or ~/path         the home folder
 *
 * Built in: cd, pwd, jobs, history, help, exit. Everything else is a
 * program. Line editing, history and Tab completion: lineedit.c, complete.c.
 * `shell -c "line"` runs one line and exits with its status (for su and sudo).
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"
#include <paths.h>

#define LINE_MAX  512
#define MAX_ARGS  16
#define MAX_JOBS  16

#define COLOR_HOST  "\x1b[92m"
#define COLOR_PATH  "\x1b[96m"
#define COLOR_ERROR "\x1b[91m"
#define COLOR_DIM   "\x1b[90m"
#define COLOR_RESET "\x1b[0m"

const char *const shell_builtins[] = { "cd", "exit", "help", "history", "jobs", "pwd", NULL };

struct job {
    long pid;
    char name[32];
};

static struct job jobs[MAX_JOBS];

/* --- small helpers --------------------------------------------------------------- */

__attribute__((format(printf, 1, 2)))
static void error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    dprintf(STDERR_FILENO, COLOR_ERROR);
    vdprintf(STDERR_FILENO, fmt, ap);
    dprintf(STDERR_FILENO, COLOR_RESET);
    va_end(ap);
}

void expand_home(const char *in, char *out, size_t size) {
    if (in[0] == '~' && (in[1] == '\0' || in[1] == '/')) {
        strlcpy(out, HOME, size);
        strlcpy(out + strlen(out), in + 1, size - strlen(out));
    } else {
        strlcpy(out, in, size);
    }
}

static void read_hostname(char *buf, size_t size) {
    strlcpy(buf, "viralos", size);
    int fd = open(_PATH_HOSTNAME, O_RDONLY);
    if (fd < 0) {
        return;
    }
    long n = read(fd, buf, size - 1);
    close(fd);
    if (n <= 0) {
        strlcpy(buf, "viralos", size);
        return;
    }
    buf[n] = '\0';
    for (long i = 0; i < n; i++) {
        if (buf[i] == '\n' || buf[i] == ' ') {
            buf[i] = '\0';
            break;
        }
    }
}

static void print_prompt(void) {
    char host[32], cwd[256];
    read_hostname(host, sizeof(host));
    if (getcwd(cwd, sizeof(cwd)) < 0) {
        strlcpy(cwd, "?", sizeof(cwd));
    }
    /* Show the home folder as ~, like most shells. */
    const char *shown = cwd;
    char short_cwd[256];
    size_t home_len = strlen(HOME);
    if (strncmp(cwd, HOME, home_len) == 0 && (cwd[home_len] == '\0' || cwd[home_len] == '/')) {
        short_cwd[0] = '~';
        strlcpy(short_cwd + 1, cwd + home_len, sizeof(short_cwd) - 1);
        shown = short_cwd;
    }
    /* user@host:~$, or # for root. */
    printf(COLOR_HOST "%s@%s" COLOR_RESET ":" COLOR_PATH "%s" COLOR_RESET "%s ", user_name(), host, shown,
           strcmp(user_name(), "root") == 0 ? "#" : "$");
}

/* Next token, splitting on spaces/tabs and honouring double quotes. NULL at the end. */
static char *next_token(char **cursor) {
    char *s = *cursor;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (!*s) {
        return NULL;
    }
    char *start = s, *out = s;
    bool quoted = false;
    while (*s && (quoted || (*s != ' ' && *s != '\t'))) {
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

/* --- jobs ----------------------------------------------------------------------- */

static void add_job(long pid, const char *name) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].pid) {
            jobs[i].pid = pid;
            strlcpy(jobs[i].name, name, sizeof(jobs[i].name));
            return;
        }
    }
}

static const char *job_name(long pid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pid == pid) {
            return jobs[i].name;
        }
    }
    return "?";
}

static void forget_job(long pid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pid == pid) {
            jobs[i].pid = 0;
        }
    }
}

/* Collect background jobs that finished, without blocking. */
static void reap_jobs(void) {
    int status;
    long pid;
    while ((pid = wait(-1, &status, WNOHANG)) > 0) {
        printf(COLOR_DIM "[%ld] done: %s (status %d)" COLOR_RESET "\n", pid, job_name(pid), status);
        forget_job(pid);
    }
}

/* --- built-ins ----------------------------------------------------------------- */

static void print_help(void) {
    printf("Built into the shell: cd [dir], pwd, jobs, history, help, exit [code]\n"
           "Run a program:  name args       (Ctrl+C stops it, Ctrl+D on an empty line exits)\n"
           "                ... > file      output to a file (>> appends), < file for input\n"
           "                ... &           run in the background\n"
           "Editing:        Tab completes commands and paths (twice: list choices),\n"
           "                Up/Down for history, Left/Right/Home/End to move, Ctrl+L clears\n"
           "Programs in " PROGRAM_DIR ":\n ");
    int fd = open(PROGRAM_DIR, O_RDONLY);
    struct os_file_info info;
    for (uint64_t i = 0; fd >= 0 && readdir(fd, i, &info) == 0; i++) {
        printf(" %s", info.name);
    }
    if (fd >= 0) {
        close(fd);
    }
    printf("\n");
}

static int last_status;   /* the last command's exit status (what `shell -c` returns) */

static bool run_builtin(int argc, char **argv) {
    last_status = 0;
    const char *cmd = argv[0];
    if (strcmp(cmd, "cd") == 0) {
        const char *dir = argc > 1 ? argv[1] : HOME;
        int err = chdir(dir);
        if (err < 0) {
            error("cd: %s: %s\n", dir, strerror(err));
            last_status = 1;
        }
    } else if (strcmp(cmd, "pwd") == 0) {
        char cwd[256];
        getcwd(cwd, sizeof(cwd));
        printf("%s\n", cwd);
    } else if (strcmp(cmd, "exit") == 0) {
        exit(argc > 1 ? atoi(argv[1]) : 0);
    } else if (strcmp(cmd, "jobs") == 0) {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (jobs[i].pid) {
                printf("[%ld] running: %s\n", jobs[i].pid, jobs[i].name);
            }
        }
    } else if (strcmp(cmd, "history") == 0) {
        history_print();
    } else if (strcmp(cmd, "help") == 0) {
        print_help();
    } else {
        return false;
    }
    return true;
}

/* --- running programs ------------------------------------------------------------ */

static void run_line(char *line) {
    char *argv[MAX_ARGS + 1];
    static char expanded[MAX_ARGS][256];   /* storage for ~ expansions */
    int argc = 0;
    char in_path[OS_PATH_MAX] = "", out_path[OS_PATH_MAX] = "";
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
                error("syntax error: missing file name after '%c'\n", tok[0]);
                return;
            }
            if (is_out) {
                expand_home(target, out_path, sizeof(out_path));
                append = dbl;
            } else {
                expand_home(target, in_path, sizeof(in_path));
            }
        } else if (strcmp(tok, "&") == 0) {
            background = true;
        } else if (argc < MAX_ARGS) {
            expand_home(tok, expanded[argc], sizeof(expanded[argc]));
            argv[argc] = expanded[argc];
            argc++;
        } else {
            error("too many arguments (max %d)\n", MAX_ARGS);
            return;
        }
    }
    if (argc == 0) {
        return;
    }
    argv[argc] = NULL;

    if (!in_path[0] && !out_path[0] && !background && run_builtin(argc, argv)) {
        return;
    }

    char path[OS_PATH_MAX];
    if (strchr(argv[0], '/')) {
        strlcpy(path, argv[0], sizeof(path));
    } else {
        strlcpy(path, PROGRAM_DIR, sizeof(path));
        strlcpy(path + strlen(PROGRAM_DIR), argv[0], sizeof(path) - strlen(PROGRAM_DIR));
    }

    struct os_spawn args = {
        .path = path,
        .argv = (const char *const *)argv,
        .fds = { OS_SPAWN_INHERIT, OS_SPAWN_INHERIT, OS_SPAWN_INHERIT },
    };
    if (in_path[0]) {
        args.fds[0] = open(in_path, O_RDONLY);
        if (args.fds[0] < 0) {
            error("%s: %s\n", in_path, strerror(args.fds[0]));
            return;
        }
    }
    if (out_path[0]) {
        args.fds[1] = open(out_path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
        if (args.fds[1] < 0) {
            error("%s: %s\n", out_path, strerror(args.fds[1]));
            if (in_path[0]) {
                close(args.fds[0]);
            }
            return;
        }
    }

    long pid = spawn(&args);
    /* The child has its own references to the redirected files now. */
    if (in_path[0]) {
        close(args.fds[0]);
    }
    if (out_path[0]) {
        close(args.fds[1]);
    }

    if (pid < 0) {
        last_status = pid == -ENOENT ? 127 : 126;
        if (pid == -ENOENT && !strchr(argv[0], '/')) {
            error("%s: command not found\n", argv[0]);
        } else {
            error("%s: %s\n", argv[0], strerror((int)pid));
        }
        return;
    }

    if (background) {
        add_job(pid, argv[0]);
        printf(COLOR_DIM "[%ld] %s running in the background" COLOR_RESET "\n", pid, argv[0]);
        return;
    }

    set_foreground(pid);   /* Ctrl+C now stops the child, not us */
    int status = 0;
    wait(pid, &status, 0);
    set_foreground(0);
    last_status = status;
    if (status != 0 && status != 130) {   /* 130 = stopped with Ctrl+C, already shown as ^C */
        printf(COLOR_DIM "[exit status %d]" COLOR_RESET "\n", status);
    }
}

int main(int argc, char **argv) {
    /* shell -c "command": run it and exit with its status (su -c, sudo -s). */
    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        char line[LINE_MAX];
        strlcpy(line, argv[2], sizeof(line));
        run_line(line);
        return last_status;
    }
    chdir(HOME);
    printf("Type 'help' for commands; Tab completes. Programs live in " PROGRAM_DIR ".\n");

    char line[LINE_MAX];
    for (;;) {
        reap_jobs();
        if (edit_line(line, sizeof(line), print_prompt, shell_complete) < 0) {
            printf("exit\n");   /* Ctrl+D on an empty line */
            return 0;
        }
        run_line(line);
    }
}
