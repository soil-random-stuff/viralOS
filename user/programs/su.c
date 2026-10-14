/*
 * su: become another user (root unless named): a shell running as them, after typing *their*
 * password (root isn't asked).
 *
 *   su [user]                 a shell as `user`, in the same folder
 *   su - [user], su -l        ... starting in their home folder
 *   su [user] -c "command"    run one command as them
 *
 * root's account is locked on viralOS (it has no password), so `su` to root fails: use sudo, or
 * give root a password with `sudo passwd root`.
 */

#include <paths.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <users.h>

static void usage(void) {
    printf("usage: su [-] [-l] [user] [-c command]\n");
}

int main(int argc, char **argv) {
    const char *target = "root", *command = NULL;
    bool login = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0 || strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--login") == 0) {
            login = true;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            command = argv[++i];
        } else if (argv[i][0] == '-') {
            usage();
            return 1;
        } else {
            target = argv[i];
        }
    }

    static struct user_account who;
    if (!users_find(target, &who)) {
        printf("su: user %s does not exist\n", target);
        return 1;
    }
    if (strcmp(user_name(), "root") != 0 && users_has_password(&who)) {
        char *pw = getpass("Password: ");
        if (!pw) {
            return 1;
        }
        bool ok = users_check_password(&who, pw);
        memset(pw, 0, strlen(pw));
        if (!ok) {
            printf("su: Authentication failure\n");
            return 1;
        }
    }

    int err = set_user(target);
    if (err < 0) {
        printf("su: can't become %s: %s\n", target, strerror(err));
        return 1;
    }
    if (login) {
        chdir(home_dir());
    }
    const char *shell = who.shell[0] ? who.shell : _PATH_SHELL;
    const char *args[] = { "shell", command ? "-c" : NULL, command, NULL };
    struct os_spawn sp = {
        .path = shell,
        .argv = args,
        .fds = { OS_SPAWN_INHERIT, OS_SPAWN_INHERIT, OS_SPAWN_INHERIT },
    };
    long pid = spawn(&sp);
    if (pid < 0) {
        printf("su: %s: %s\n", shell, strerror((int)pid));
        return 1;
    }
    /* A command gets Ctrl+C; an interactive shell handles it itself (its line editor, its programs). */
    set_foreground(command ? pid : 0);
    int status = 0;
    wait(pid, &status, 0);
    return status;
}
