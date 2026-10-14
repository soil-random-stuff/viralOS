/*
 * sudo: run a command as another user (root unless -u says otherwise).
 *
 *   sudo command [args...]     as root
 *   sudo -u user command...    as `user`
 *   sudo -s                    a shell as root (or -u's user)
 *   sudo -v                    just ask for the password (and remember it)
 *   sudo -k                    forget the password: the next sudo asks again
 *
 * Who may: root, and the members of the "sudo" group (/etc/group). They type their own password,
 * which is remembered for 15 minutes (a timestamp in /run/sudo). Accounts without a password
 * aren't asked.
 *
 * viralOS doesn't enforce file permissions yet, so this decides who a program runs for (its
 * home, its settings, whoami), not what it may touch.
 */

#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <users.h>

#define STAMP_DIR    _PATH_RUN "/sudo"
#define REMEMBER_MS  (15 * 60 * 1000)
#define TRIES        3

static void usage(void) {
    printf("usage: sudo [-u user] command [args...]\n"
           "       sudo [-u user] -s\n"
           "       sudo -v | -k\n");
    exit(1);
}

static void stamp_path(const char *user, char *out, size_t size) {
    snprintf(out, size, STAMP_DIR "/%s", user);
}

/* Did `user` type their password in the last 15 minutes? */
static bool remembered(const char *user) {
    char path[OS_PATH_MAX], text[32];
    stamp_path(user, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    long n = read(fd, text, sizeof(text) - 1);
    close(fd);
    text[n > 0 ? n : 0] = '\0';
    uint64_t when = (uint64_t)strtol(text, NULL, 10);
    uint64_t now = uptime_ms();
    return n > 0 && when <= now && now - when < REMEMBER_MS;
}

static void remember(const char *user) {
    char path[OS_PATH_MAX];
    mkdir(_PATH_RUN);
    mkdir(STAMP_DIR);
    stamp_path(user, path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        dprintf(fd, "%llu\n", (unsigned long long)uptime_ms());
        close(fd);
    }
}

static void forget(const char *user) {
    char path[OS_PATH_MAX];
    stamp_path(user, path, sizeof(path));
    unlink(path);
}

/* Ask `me` for their own password. */
static bool authenticate(const char *me) {
    static struct user_account a;
    if (!users_find(me, &a)) {
        printf("sudo: you (%s) don't have an account in " _PATH_PASSWD "\n", me);
        return false;
    }
    if (!users_has_password(&a)) {
        return true;
    }
    char prompt[64];
    snprintf(prompt, sizeof(prompt), "[sudo] password for %s: ", me);
    for (int i = 0; i < TRIES; i++) {
        char *pw = getpass(prompt);
        if (!pw) {
            return false;
        }
        bool ok = users_check_password(&a, pw);
        memset(pw, 0, strlen(pw));
        if (ok) {
            return true;
        }
        if (i < TRIES - 1) {
            printf("Sorry, try again.\n");
        }
    }
    printf("sudo: %d incorrect password attempts\n", TRIES);
    return false;
}

int main(int argc, char **argv) {
    const char *target = "root";
    bool shell = false, validate = false, kill = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            target = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-i") == 0) {
            shell = true;
        } else if (strcmp(argv[i], "-v") == 0) {
            validate = true;
        } else if (strcmp(argv[i], "-k") == 0) {
            kill = true;
        } else if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else {
            usage();
        }
    }
    char me[OS_USER_MAX];
    strlcpy(me, user_name(), sizeof(me));
    if (kill) {
        forget(me);
        if (i == argc && !shell && !validate) {
            return 0;
        }
    }
    if (i == argc && !shell && !validate) {
        usage();
    }

    if (strcmp(me, "root") != 0) {
        if (!users_in_group(me, USERS_ADMIN_GROUP)) {
            printf("%s is not allowed to use sudo: only members of the '" USERS_ADMIN_GROUP "' group are.\n", me);
            return 1;
        }
        if (!remembered(me)) {
            if (!authenticate(me)) {
                return 1;
            }
            remember(me);
        }
    }
    if (validate) {
        return 0;
    }

    static struct user_account who;
    if (!users_find(target, &who)) {
        printf("sudo: unknown user %s\n", target);
        return 1;
    }

    /* The command: a program in /usr/bin (or a path), or the user's shell. */
    char path[OS_PATH_MAX];
    const char *args[64];
    int n = 0;
    if (shell && i == argc) {
        strlcpy(path, who.shell[0] ? who.shell : _PATH_SHELL, sizeof(path));
        args[n++] = "shell";
    } else if (shell) {
        /* sudo -s command...: the shell runs it as one line. */
        static char line[1024];
        line[0] = '\0';
        for (int k = i; k < argc; k++) {
            strlcat(line, k > i ? " " : "", sizeof(line));
            strlcat(line, argv[k], sizeof(line));
        }
        strlcpy(path, who.shell[0] ? who.shell : _PATH_SHELL, sizeof(path));
        args[n++] = "shell";
        args[n++] = "-c";
        args[n++] = line;
    } else {
        if (strchr(argv[i], '/')) {
            strlcpy(path, argv[i], sizeof(path));
        } else {
            snprintf(path, sizeof(path), _PATH_BIN "/%s", argv[i]);
        }
        for (int k = i; k < argc && n < 63; k++) {
            args[n++] = argv[k];
        }
    }
    args[n] = NULL;

    int err = set_user(target);
    if (err < 0) {
        printf("sudo: can't become %s: %s\n", target, strerror(err));
        return 1;
    }
    struct os_spawn sp = {
        .path = path,
        .argv = args,
        .fds = { OS_SPAWN_INHERIT, OS_SPAWN_INHERIT, OS_SPAWN_INHERIT },
    };
    long pid = spawn(&sp);
    if (pid < 0) {
        printf("sudo: %s: %s\n", args[0], pid == -ENOENT ? "command not found" : strerror((int)pid));
        return 1;
    }
    /* Ctrl+C goes to the command; an interactive shell handles it itself. */
    set_foreground(shell && i == argc ? 0 : pid);
    int status = 0;
    wait(pid, &status, 0);
    return status;
}
