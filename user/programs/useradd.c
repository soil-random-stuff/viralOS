/*
 * useradd: make an account (as root: sudo useradd ...).
 *
 *   useradd [-c "Display Name"] [-G group,group] [-m] name
 *
 * It gets the next free uid from 1000, a group of its own, /home/<name> with the usual folders,
 * and /usr/bin/shell. Its password is locked until `passwd name` gives it one (until then it
 * can't log in). -G adds it to other groups, e.g. -G sudo to let it use sudo. -m is accepted
 * (the home folder is always made).
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <users.h>

#define MAX_GROUPS 64

static void usage(void) {
    printf("usage: useradd [-c \"Display Name\"] [-G group,group] [-m] name\n");
}

/* The next comma-separated item of *p (cut out in place), or NULL. */
static char *next_item(char **p) {
    while (**p == ',') {
        (*p)++;
    }
    if (!**p) {
        return NULL;
    }
    char *item = *p, *comma = strchr(item, ',');
    if (comma) {
        *comma = '\0';
        *p = comma + 1;
    } else {
        *p = item + strlen(item);
    }
    return item;
}

/* Is there a group of this name? */
static bool group_exists(const char *name) {
    static struct user_group all[MAX_GROUPS];
    int n = users_groups_load(all, MAX_GROUPS);
    for (int i = 0; i < n; i++) {
        if (strcmp(all[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

int main(int argc, char **argv) {
    const char *display = NULL, *name = NULL;
    char groups[256] = "";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            display = argv[++i];
        } else if (strcmp(argv[i], "-G") == 0 && i + 1 < argc) {
            strlcpy(groups, argv[++i], sizeof(groups));
        } else if (strcmp(argv[i], "-m") == 0) {
            /* (always) */
        } else if (argv[i][0] == '-' || name) {
            usage();
            return 2;
        } else {
            name = argv[i];
        }
    }
    if (!name) {
        usage();
        return 2;
    }
    if (strcmp(user_name(), "root") != 0) {
        printf("useradd: Permission denied (try: sudo useradd ...)\n");
        return 1;
    }
    if (!users_valid_name(name)) {
        printf("useradd: invalid user name '%s': use lowercase letters, digits, - and _\n", name);
        return 3;
    }
    /* Check the groups first: nothing is made if one is wrong. */
    char check[256], *cursor = check;
    strlcpy(check, groups, sizeof(check));
    for (char *g; (g = next_item(&cursor)) != NULL;) {
        if (!group_exists(g)) {
            printf("useradd: group '%s' does not exist\n", g);
            return 6;
        }
    }

    int r = users_add(name, display ? display : name, NULL);
    if (r == -EEXIST) {
        printf("useradd: user '%s' already exists\n", name);
        return 9;
    }
    if (r < 0) {
        printf("useradd: can't add '%s': %s\n", name, strerror(r));
        return 1;
    }
    cursor = groups;
    for (char *g; (g = next_item(&cursor)) != NULL;) {
        r = users_group_add(g, name);
        if (r < 0 && r != -EEXIST) {
            printf("useradd: can't add '%s' to group '%s': %s\n", name, g, strerror(r));
        }
    }
    printf("Added %s. Give them a password to log in: passwd %s\n", name, name);
    return 0;
}
