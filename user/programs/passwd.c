/*
 * passwd: change a password.
 *
 *   passwd            your own (asks for the current one first)
 *   passwd user       someone else's (as root: sudo passwd user)
 *   passwd -d user    remove it: logging in won't ask for one (root)
 *   passwd -l user    lock the account: it can't log in (root)
 *   passwd -u user    unlock it (root)
 *   passwd -S [user]  its status: P (has a password), NP (none) or L (locked), and when it changed
 *
 * Passwords are kept hashed (PBKDF2-SHA256) in /etc/shadow.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <users.h>

static void usage(void) {
    printf("usage: passwd [-d | -l | -u | -S] [user]\n");
}

static void wipe(char *s) {
    if (s) {
        memset(s, 0, strlen(s));
    }
}

static int status(const struct user_account *a) {
    const char *state = users_is_locked(a) ? "L" : users_has_password(a) ? "P" : "NP";
    char date[16] = "-";
    if (a->changed > 0) {
        time_t t = (time_t)a->changed * 86400;
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(date, sizeof(date), "%02d/%02d/%04d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900);
    }
    printf("%s %s %s 0 99999 7 -1\n", a->name, state, date);
    return 0;
}

int main(int argc, char **argv) {
    char op = 0;
    const char *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] && !argv[i][2] && strchr("dluS", argv[i][1]) && !op) {
            op = argv[i][1];
        } else if (argv[i][0] == '-' || name) {
            usage();
            return 2;
        } else {
            name = argv[i];
        }
    }
    char me[OS_USER_MAX];
    strlcpy(me, user_name(), sizeof(me));
    bool root = strcmp(me, "root") == 0;
    if (!name) {
        name = me;
    }
    static struct user_account a;
    if (!users_find(name, &a)) {
        printf("passwd: user '%s' does not exist\n", name);
        return 1;
    }
    if (!root && (strcmp(name, me) != 0 || (op && op != 'S'))) {
        printf("passwd: You may not view or modify password information for %s (try: sudo passwd ...)\n", name);
        return 1;
    }

    int r = 0;
    switch (op) {
    case 'S':
        return status(&a);
    case 'd':
        r = users_set_password(name, "");
        break;
    case 'l':
        r = users_set_locked(name, true);
        break;
    case 'u':
        if (users_is_locked(&a) && strcmp(a.password, "!") == 0) {
            printf("passwd: unlocking %s would leave it without a password: use passwd -d %s for that\n",
                   name, name);
            return 3;
        }
        r = users_set_locked(name, false);
        break;
    default: {
        printf("Changing password for %s.\n", name);
        if (!root && users_has_password(&a) && !users_is_locked(&a)) {
            char *current = getpass("Current password: ");
            bool ok = current && users_check_password(&a, current);
            wipe(current);
            if (!ok) {
                printf("passwd: Authentication token manipulation error\npasswd: password unchanged\n");
                return 10;
            }
        }
        char first[128];
        char *pw = getpass("New password: ");
        if (!pw) {
            printf("passwd: password unchanged\n");
            return 10;
        }
        if (!pw[0]) {
            printf("No password has been supplied.\npasswd: password unchanged\n");
            return 10;
        }
        strlcpy(first, pw, sizeof(first));
        wipe(pw);
        pw = getpass("Retype new password: ");
        bool same = pw && strcmp(pw, first) == 0;
        wipe(pw);
        if (!same) {
            wipe(first);
            printf("Sorry, passwords do not match.\npasswd: password unchanged\n");
            return 10;
        }
        r = users_set_password(name, first);
        wipe(first);
        break;
    }
    }
    if (r < 0) {
        printf("passwd: %s\n", strerror(r));
        return 1;
    }
    printf("passwd: %s\n", op == 'd' ? "password removed" : op == 'l' ? "account locked"
                         : op == 'u' ? "account unlocked" : "password updated successfully");
    return 0;
}
