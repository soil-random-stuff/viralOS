/* Who the program runs for, and their folders (see SYS_USER in <viralos.h>). */

#include <paths.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "syscall.h"

#define FALLBACK_USER "user"   /* programs started before anyone logged in (the text shell) */

static char name[OS_USER_MAX];
static char home[OS_PATH_MAX], desktop[OS_PATH_MAX];

/* See <unistd.h>. */
void user_home(const char *user, char *out, size_t size) {
    static char text[8192];
    snprintf(out, size, _PATH_HOME "/%s", user);
    int fd = open(_PATH_PASSWD, O_RDONLY);
    if (fd < 0) {
        return;
    }
    long n = read(fd, text, sizeof(text) - 1);
    close(fd);
    text[n > 0 ? n : 0] = '\0';
    size_t len = strlen(user);
    for (char *line = text; line && *line;) {
        char *end = strchr(line, '\n');
        if (end) {
            *end = '\0';
        }
        if (strncmp(line, user, len) == 0 && line[len] == ':') {
            /* name:x:uid:gid:display:home:shell */
            char *field = line;
            for (int i = 0; i < 5 && field; i++) {
                field = strchr(field, ':');
                field = field ? field + 1 : NULL;
            }
            char *stop = field ? strchr(field, ':') : NULL;
            if (stop && stop > field) {
                *stop = '\0';
                strlcpy(out, field, size);
            }
            return;
        }
        line = end ? end + 1 : NULL;
    }
}

static void refresh(void) {
    if (syscall3(SYS_USER, OS_USER_GET, (long)name, 0) < 0 || !name[0]) {
        strlcpy(name, FALLBACK_USER, sizeof(name));
    }
    user_home(name, home, sizeof(home));
    snprintf(desktop, sizeof(desktop), "%s/Desktop", home);
}

const char *user_name(void) {
    if (!name[0]) {
        refresh();
    }
    return name;
}

const char *home_dir(void) {
    if (!name[0]) {
        refresh();
    }
    return home;
}

const char *desktop_dir(void) {
    if (!name[0]) {
        refresh();
    }
    return desktop;
}

int set_user(const char *user) {
    int r = (int)syscall3(SYS_USER, OS_USER_SET, (long)user, 0);
    if (r == 0) {
        refresh();
    }
    return r;
}

void make_home_dirs(const char *user) {
    static const char *const folders[] = {
        "", "/Desktop", "/Documents", "/Downloads", "/Music", "/Pictures", "/.config", "/" _PATH_DDE_USER_DIR,
    };
    char base[OS_PATH_MAX];
    user_home(user, base, sizeof(base));
    for (size_t i = 0; i < sizeof(folders) / sizeof(folders[0]); i++) {
        char path[OS_PATH_MAX];
        struct os_file_info info;
        snprintf(path, sizeof(path), "%s%s", base, folders[i]);
        if (stat(path, &info) < 0) {
            mkdir(path);
        }
    }
}
