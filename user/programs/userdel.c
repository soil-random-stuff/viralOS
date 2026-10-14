/*
 * userdel: remove an account (as root: sudo userdel ...).
 *
 *   userdel [-r] name
 *
 * The account goes from /etc/passwd, /etc/shadow and /etc/group (its own group, and its place in
 * the others). Its home folder stays, unless -r: then that goes too.
 */

#include <fileops.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <users.h>

int main(int argc, char **argv) {
    bool remove_home = false;
    const char *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            remove_home = true;
        } else if (argv[i][0] == '-' || name) {
            name = NULL;
            break;
        } else {
            name = argv[i];
        }
    }
    if (!name) {
        printf("usage: userdel [-r] name\n");
        return 2;
    }
    if (strcmp(user_name(), "root") != 0) {
        printf("userdel: Permission denied (try: sudo userdel ...)\n");
        return 1;
    }
    if (strcmp(name, "root") == 0) {
        printf("userdel: root can't be removed\n");
        return 8;
    }
    static struct user_account who;
    if (!users_find(name, &who)) {
        printf("userdel: user '%s' does not exist\n", name);
        return 6;
    }
    int r = users_remove(name);
    if (r < 0) {
        printf("userdel: can't remove '%s': %s\n", name, strerror(r));
        return 1;
    }
    if (remove_home && who.home[0] && strcmp(who.home, "/") != 0) {
        r = delete_tree(who.home);
        if (r < 0) {
            printf("userdel: %s: %s\n", who.home, strerror(r));
            return 12;
        }
    }
    return 0;
}
