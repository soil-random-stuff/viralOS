/*
 * The apps the desktop knows about: one launcher file per app in /usr/share/applications, in the
 * freedesktop.org .desktop format, named after the program ("divine-files.desktop"):
 *
 *     [Desktop Entry]
 *     Type=Application
 *     Name=File Manager
 *     Comment=Manage files on your computer.
 *     Icon=/usr/share/dde-res/Icons/Apps/Files.png
 *     Exec=/usr/bin/divine-files
 *
 * Exec is the program and its arguments (quotes group words). The apps in the user's Pinned= list
 * (DDE's settings, by launcher name without ".desktop") always have a place on the panel; the
 * others only while they run. Every app is in the apps menu, in file name order. An app without an
 * icon (or whose icon can't be loaded) gets a coloured tile with its initial.
 */

#include <config.h>
#include <image.h>
#include <stdlib.h>
#include <string.h>

#include "desktop.h"
#include <paths.h>

#define APPS_DIR      _PATH_APPLICATIONS
#define ICON_SIZE     36    /* on the panel */
#define ICON_LARGE    44    /* in the apps menu */
#define LAUNCHER_EXT  ".desktop"

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
    return s;
}

/* A value, without the quotes around it if it has them. */
static char *unquote(char *v) {
    size_t n = strlen(v);
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
        v[n - 1] = '\0';
        return v + 1;
    }
    return v;
}

/*
 * Split a command line into words; "double quotes" group words with spaces.
 * Writes into `buf`; returns the word count (at most max - 1, NULL-terminated).
 */
static int split_words(const char *line, char *buf, size_t size, const char **argv, int max) {
    int argc = 0;
    size_t used = 0;
    const char *p = line;
    while (*p && argc < max - 1) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        argv[argc++] = buf + used;
        bool quoted = false;
        while (*p && (quoted || (*p != ' ' && *p != '\t')) && used < size - 1) {
            if (*p == '"') {
                quoted = !quoted;
            } else {
                buf[used++] = *p;
            }
            p++;
        }
        buf[used++] = '\0';
        if (used >= size) {
            break;
        }
    }
    argv[argc] = NULL;
    return argc;
}

/* A rounded tile in a colour picked from the name, with the initial on it. */
static struct gfx_surface *letter_icon(struct desktop *d, const char *name, int size) {
    static const gfx_color tiles[] = {
        GFX_RGB(0x2F, 0x6F, 0xEB), GFX_RGB(0x8E, 0x5B, 0xE8), GFX_RGB(0x14, 0xA3, 0x8B),
        GFX_RGB(0xE8, 0x7B, 0x2C), GFX_RGB(0xD9, 0x46, 0x6B),
    };
    struct gfx_surface *s = gfx_surface_create(size, size);
    if (!s) {
        return NULL;
    }
    unsigned hash = 0;
    for (const char *p = name; *p; p++) {
        hash = hash * 31 + (unsigned char)*p;
    }
    float inset = size * 0.08f;
    gfx_fill_rounded_rect(s, gfx_rect((int)inset, (int)inset, size - 2 * (int)inset, size - 2 * (int)inset),
                          size * 0.24f, tiles[hash % (sizeof(tiles) / sizeof(tiles[0]))]);
    char initial[2] = { name[0] ? name[0] : '?', '\0' };
    if (d->bold) {
        float font = size * 0.5f;
        float w = ttf_text_width(d->bold, font, initial);
        ttf_draw_text(s, d->bold, font, (size - w) / 2, size * 0.5f + font * 0.36f,
                      GFX_RGB(255, 255, 255), initial);
    }
    return s;
}

static void load_icons(struct desktop *d, struct app *a, const char *path) {
    struct gfx_surface *img = path[0] ? image_load_png(path, NULL) : NULL;
    if (img) {
        a->icon = gfx_surface_scaled(img, ICON_SIZE, ICON_SIZE);
        a->icon_large = gfx_surface_scaled(img, ICON_LARGE, ICON_LARGE);
        gfx_surface_destroy(img);
    } else if (path[0]) {
        dprintf(STDERR_FILENO, "desktop: cannot load icon %s\n", path);
    }
    if (!a->icon) {
        a->icon = letter_icon(d, a->name, ICON_SIZE);
    }
    if (!a->icon_large) {
        a->icon_large = letter_icon(d, a->name, ICON_LARGE);
    }
}

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static void load_launcher(struct desktop *d, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return;
    }
    char text[2048];
    long len = 0, n;
    while (len < (long)sizeof(text) - 1 && (n = read(fd, text + len, sizeof(text) - 1 - len)) > 0) {
        len += n;
    }
    close(fd);
    text[len] = '\0';

    struct app *a = &d->apps[d->app_count];
    memset(a, 0, sizeof(*a));
    char icon[128] = "";
    for (char *line = text, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        line = trim(line);
        char *eq = strchr(line, '=');
        if (!line[0] || line[0] == '#' || !eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim(line), *value = unquote(trim(eq + 1));
        if (strcmp(key, "Name") == 0) {
            strlcpy(a->name, value, sizeof(a->name));
        } else if (strcmp(key, "Comment") == 0 || strcmp(key, "Desc") == 0) {
            strlcpy(a->desc, value, sizeof(a->desc));
        } else if (strcmp(key, "Icon") == 0) {
            strlcpy(icon, value, sizeof(icon));
        } else if (strcmp(key, "Exec") == 0) {
            strlcpy(a->exec, value, sizeof(a->exec));
        }
    }
    /* The program is the first word of Exec. */
    char words[sizeof(a->exec)];
    const char *argv[2];
    if (!a->name[0] || split_words(a->exec, words, sizeof(words), argv, 2) < 1) {
        dprintf(STDERR_FILENO, "desktop: %s: needs Name and Exec\n", path);
        return;
    }
    strlcpy(a->path, argv[0], sizeof(a->path));
    strlcpy(a->launcher, path, sizeof(a->launcher));
    load_icons(d, a, icon);
    d->app_count++;
}

void apps_load(struct desktop *d) {
    int fd = open(APPS_DIR, O_RDONLY);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "desktop: no %s folder\n", APPS_DIR);
        return;
    }
    struct os_file_info info;
    for (uint64_t i = 0; readdir(fd, i, &info) == 0 && d->app_count < MAX_APPS; i++) {
        if (info.type == OS_FILE && ends_with(info.name, LAUNCHER_EXT)) {
            char path[160];
            snprintf(path, sizeof(path), APPS_DIR "/%s", info.name);
            load_launcher(d, path);
        }
    }
    close(fd);
    apps_reload_pinned(d);
}

/* The launcher's name without ".desktop": what Pinned= lists. */
static void launcher_id(const char *launcher, char *out, size_t size) {
    const char *slash = strrchr(launcher, '/');
    strlcpy(out, slash ? slash + 1 : launcher, size);
    size_t n = strlen(out), m = strlen(LAUNCHER_EXT);
    if (n > m && strcmp(out + n - m, LAUNCHER_EXT) == 0) {
        out[n - m] = '\0';
    }
}

void apps_reload_pinned(struct desktop *d) {
    char list[1024] = "";
    config_get(DDE_CONFIG, "Pinned", list, sizeof(list));
    for (int i = 0; i < d->app_count; i++) {
        char id[64];
        launcher_id(d->apps[i].launcher, id, sizeof(id));
        d->apps[i].pinned = config_list_has(list, id);
    }
}

/* Icon for a panel entry: the app's, or a letter tile for programs not in the list. */
struct gfx_surface *app_icon(struct desktop *d, int app, const char *name, bool large) {
    if (app >= 0) {
        return large ? d->apps[app].icon_large : d->apps[app].icon;
    }
    /* Made on demand and kept: few programs run outside the list. */
    static struct { char name[32]; struct gfx_surface *icon; } cache[16];
    for (int i = 0; i < 16; i++) {
        if (cache[i].icon && strcmp(cache[i].name, name) == 0) {
            return cache[i].icon;
        }
    }
    for (int i = 0; i < 16; i++) {
        if (!cache[i].icon) {
            strlcpy(cache[i].name, name, sizeof(cache[i].name));
            cache[i].icon = letter_icon(d, name, ICON_SIZE);
            return cache[i].icon;
        }
    }
    return cache[0].icon;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int launch(struct desktop *d, int app, const char *extra) {
    if (app < 0 || app >= d->app_count) {
        return -EINVAL;
    }
    char words[sizeof(d->apps[app].exec)];
    const char *argv[17];
    int argc = split_words(d->apps[app].exec, words, sizeof(words), argv, 15);
    if (extra && argc > 0) {
        argv[argc++] = extra;
        argv[argc] = NULL;
    }
    argv[0] = base_name(argv[0]);   /* programs see their own name, not the path */
    long pid = spawn_simple(d->apps[app].path, argv);
    if (pid < 0) {
        dprintf(STDERR_FILENO, "desktop: cannot start %s: %s\n", d->apps[app].path, strerror((int)pid));
        return (int)pid;
    }
    for (int i = 0; i < MAX_LAUNCHED; i++) {
        if (!d->launched[i].pid) {
            d->launched[i].pid = pid;
            d->launched[i].app = app;
            break;
        }
    }
    return (int)pid;
}

int app_launch(struct desktop *d, int app) {
    return launch(d, app, NULL);
}

int app_launch_path(struct desktop *d, const char *path, const char *arg) {
    for (int i = 0; i < d->app_count; i++) {
        if (strcmp(d->apps[i].path, path) == 0) {
            return launch(d, i, arg);
        }
    }
    return -ENOENT;
}

/* Which app a connecting program is: one we started, or one with a known program name. */
int app_for_pid(struct desktop *d, int64_t pid, const char *name) {
    for (int i = 0; i < MAX_LAUNCHED; i++) {
        if (d->launched[i].pid && d->launched[i].pid == pid) {
            return d->launched[i].app;
        }
    }
    for (int i = 0; i < d->app_count; i++) {
        if (strcmp(base_name(d->apps[i].path), name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Collect the exit codes of apps we started (otherwise they'd stay zombies). */
void apps_reap(struct desktop *d) {
    int status;
    long pid;
    while ((pid = wait(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_LAUNCHED; i++) {
            if (d->launched[i].pid == pid) {
                d->launched[i].pid = 0;
            }
        }
    }
}
