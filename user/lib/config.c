/* Key=value settings files: see <config.h>. */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_FILE (64 * 1024)
#define DDE_SETTINGS_NAME "settings.conf"

const char *dde_config_path(const char *user) {
    static char mine[OS_PATH_MAX], theirs[OS_PATH_MAX];
    if (!user) {
        snprintf(mine, sizeof(mine), "%s/" _PATH_DDE_USER_DIR "/" DDE_SETTINGS_NAME, home_dir());
        return mine;
    }
    char home[OS_PATH_MAX];
    user_home(user, home, sizeof(home));
    snprintf(theirs, sizeof(theirs), "%s/" _PATH_DDE_USER_DIR "/" DDE_SETTINGS_NAME, home);
    return theirs;
}

const char *dde_desktop_layout_path(void) {
    static char path[OS_PATH_MAX];
    snprintf(path, sizeof(path), "%s/" _PATH_DDE_USER_DIR "/desktop", home_dir());
    return path;
}

/* A user's DDE settings: what they haven't set comes from the system's defaults. */
static bool is_user_dde_config(const char *path) {
    const char *tail = "/" _PATH_DDE_USER_DIR "/" DDE_SETTINGS_NAME;
    size_t n = strlen(path), m = strlen(tail);
    return n > m && strcmp(path + n - m, tail) == 0;
}

/* Make the folders above `path` (the file itself not). */
static void make_parents(const char *path) {
    char dir[OS_PATH_MAX];
    strlcpy(dir, path, sizeof(dir));
    for (char *c = dir + 1; *c; c++) {
        if (*c == '/') {
            *c = '\0';
            mkdir(dir);   /* (fails harmlessly if it's there) */
            *c = '/';
        }
    }
}

static char *read_all(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    char *buf = malloc(MAX_FILE + 1);
    size_t len = 0;
    if (buf) {
        long n;
        while (len < MAX_FILE && (n = read(fd, buf + len, MAX_FILE - len)) > 0) {
            len += (size_t)n;
        }
        buf[len] = '\0';
    }
    close(fd);
    return buf;
}

/* Does this line set `key`? Then where its value starts. */
static const char *value_of(const char *line, const char *key) {
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t n = strlen(key);
    if (strncmp(line, key, n) != 0) {
        return NULL;
    }
    line += n;
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    return *line == '=' ? line + 1 : NULL;
}

static bool get_from(const char *path, const char *key, char *out, size_t size) {
    char *buf = read_all(path);
    if (!buf) {
        return false;
    }
    bool found = false;
    for (char *line = buf; line && *line && !found;) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        const char *v = value_of(line, key);
        if (v) {
            while (*v == ' ' || *v == '\t') {
                v++;
            }
            strlcpy(out, v, size);
            size_t len = strlen(out);
            while (len && (out[len - 1] == '\r' || out[len - 1] == ' ' || out[len - 1] == '\t')) {
                out[--len] = '\0';
            }
            found = true;
        }
        line = nl ? nl + 1 : NULL;
    }
    free(buf);
    return found;
}

bool config_get(const char *path, const char *key, char *out, size_t size) {
    return get_from(path, key, out, size) || (is_user_dde_config(path) && get_from(_PATH_DDE_DEFAULTS, key, out, size));
}

long config_get_int(const char *path, const char *key, long fallback) {
    char text[32];
    if (!config_get(path, key, text, sizeof(text)) || !text[0]) {
        return fallback;
    }
    char *end;
    long v = strtol(text, &end, 10);
    return *end ? fallback : v;
}

int config_set(const char *path, const char *key, const char *value) {
    char *old = read_all(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd == -ENOENT) {
        make_parents(path);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    }
    if (fd < 0) {
        free(old);
        return fd;
    }
    bool written = false;
    for (char *line = old; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        if (value_of(line, key)) {
            if (!written) {
                dprintf(fd, "%s=%s\n", key, value);
                written = true;
            }
        } else {
            dprintf(fd, "%s\n", line);
        }
        line = nl ? nl + 1 : NULL;
    }
    if (!written) {
        dprintf(fd, "%s=%s\n", key, value);
    }
    close(fd);
    free(old);
    return 0;
}

bool config_list_has(const char *list, const char *item) {
    size_t n = strlen(item);
    for (const char *p = list; n && *p;) {
        while (*p == ',' || *p == ' ') {
            p++;
        }
        const char *end = p;
        while (*end && *end != ',') {
            end++;
        }
        const char *last = end;
        while (last > p && last[-1] == ' ') {
            last--;
        }
        if ((size_t)(last - p) == n && strncmp(p, item, n) == 0) {
            return true;
        }
        p = end;
    }
    return false;
}

int config_set_int(const char *path, const char *key, long value) {
    char text[24];
    snprintf(text, sizeof(text), "%ld", value);
    return config_set(path, key, text);
}
