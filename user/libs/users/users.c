/* User accounts in /etc/passwd, /etc/shadow and /etc/group (see <users.h>). */

#include <config.h>
#include <image.h>
#include <math.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <users.h>

#include "users_internal.h"

#define ITERATIONS 20000
#define SALT_BYTES 16
#define MAX_FILE   16384

/* --- the files ------------------------------------------------------------------------------------ */

static void copy_field(char *out, size_t size, const char *from, const char *to) {
    size_t n = (size_t)(to - from);
    n = n < size - 1 ? n : size - 1;
    memcpy(out, from, n);
    out[n] = '\0';
}

/* Split a line at ':' into up to `max` fields (start and end of each); returns how many. */
static int split_fields(const char *line, const char **from, const char **to, int max) {
    int n = 0;
    const char *p = line;
    while (n < max) {
        from[n] = p;
        while (*p && *p != ':' && *p != '\n' && *p != '\r') {
            p++;
        }
        to[n++] = p;
        if (*p != ':') {
            break;
        }
        p++;
    }
    return n;
}

static char *read_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    char *buf = malloc(MAX_FILE);
    long len = 0, n;
    while (buf && len < MAX_FILE - 1 && (n = read(fd, buf + len, MAX_FILE - 1 - (size_t)len)) > 0) {
        len += n;
    }
    close(fd);
    if (buf) {
        buf[len] = '\0';
    }
    return buf;
}

/* Each line of a file (without its newline) to fn. */
static void each_line(char *buf, void (*fn)(char *line, void *arg), void *arg) {
    for (char *line = buf; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        if (line[0] && line[0] != '#') {
            fn(line, arg);
        }
        line = nl ? nl + 1 : NULL;
    }
}

struct table {
    struct user_account *all;
    int count, max;
};

/* "name:x:uid:gid:display:home:shell" */
static void passwd_line(char *line, void *arg) {
    struct table *t = arg;
    const char *from[7], *to[7];
    if (t->count == t->max || split_fields(line, from, to, 7) < 7) {
        return;
    }
    struct user_account *a = &t->all[t->count];
    memset(a, 0, sizeof(*a));
    copy_field(a->name, sizeof(a->name), from[0], to[0]);
    a->uid = (int)strtol(from[2], NULL, 10);
    a->gid = (int)strtol(from[3], NULL, 10);
    copy_field(a->display, sizeof(a->display), from[4], to[4]);
    copy_field(a->home, sizeof(a->home), from[5], to[5]);
    copy_field(a->shell, sizeof(a->shell), from[6], to[6]);
    if (users_valid_name(a->name)) {
        t->count++;
    }
}

/* "name:password:changed:..." */
static void shadow_line(char *line, void *arg) {
    struct table *t = arg;
    const char *from[3], *to[3];
    int n = split_fields(line, from, to, 3);
    for (int i = 0; i < t->count && n >= 2; i++) {
        struct user_account *a = &t->all[i];
        if ((size_t)(to[0] - from[0]) == strlen(a->name) && strncmp(from[0], a->name, strlen(a->name)) == 0) {
            copy_field(a->password, sizeof(a->password), from[1], to[1]);
            a->changed = n >= 3 ? strtol(from[2], NULL, 10) : 0;
        }
    }
}

/* Every account (the system's too), with its password and picture. Returns how many. */
static int load_all(struct user_account *all, int max) {
    struct table t = { all, 0, max };
    char *buf = read_file(_PATH_PASSWD);
    if (!buf) {
        return 0;
    }
    each_line(buf, passwd_line, &t);
    free(buf);
    if ((buf = read_file(_PATH_SHADOW)) != NULL) {
        each_line(buf, shadow_line, &t);
        free(buf);
    }
    for (int i = 0; i < t.count; i++) {
        all[i].picture[0] = '\0';
        if (all[i].uid >= USERS_FIRST_UID) {
            config_get(dde_config_path(all[i].name), "Picture", all[i].picture, sizeof(all[i].picture));
        }
    }
    return t.count;
}

static bool is_person(const struct user_account *a) {
    return a->uid >= USERS_FIRST_UID && a->uid < 65534;
}

int users_load(struct user_account *out, int max) {
    struct user_account *all = malloc(sizeof(*all) * USERS_MAX);
    int n = all ? load_all(all, USERS_MAX) : 0, count = 0;
    for (int i = 0; i < n && count < max; i++) {
        if (is_person(&all[i])) {
            out[count++] = all[i];
        }
    }
    free(all);
    return count;
}

bool users_find(const char *name, struct user_account *out) {
    struct user_account *all = malloc(sizeof(*all) * USERS_MAX);
    int n = all ? load_all(all, USERS_MAX) : 0;
    bool found = false;
    for (int i = 0; i < n && !found; i++) {
        if (strcmp(all[i].name, name) == 0) {
            *out = all[i];
            found = true;
        }
    }
    free(all);
    return found;
}

const char *session_display_name(void) {
    static char display[64];
    struct user_account *a = malloc(sizeof(*a));
    if (a && users_find(user_name(), a) && a->display[0]) {
        strlcpy(display, a->display, sizeof(display));
    } else {
        strlcpy(display, user_name(), sizeof(display));
    }
    free(a);
    return display;
}

bool users_valid_name(const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n >= OS_USER_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        return fd;
    }
    size_t len = strlen(text);
    long w = write(fd, text, len);
    close(fd);
    return w == (long)len ? 0 : -EIO;
}

/* --- groups -------------------------------------------------------------------------------------- */

#define GROUPS_MAX 64

/* "name:x:gid:members" */
static void group_line(char *line, void *arg) {
    struct { struct user_group *all; int count, max; } *t = arg;
    const char *from[4], *to[4];
    int n = split_fields(line, from, to, 4);
    if (t->count == t->max || n < 3) {
        return;
    }
    struct user_group *g = &t->all[t->count];
    memset(g, 0, sizeof(*g));
    copy_field(g->name, sizeof(g->name), from[0], to[0]);
    g->gid = (int)strtol(from[2], NULL, 10);
    if (n == 4) {
        copy_field(g->members, sizeof(g->members), from[3], to[3]);
    }
    t->count += g->name[0] != '\0';
}

int users_groups_load(struct user_group *out, int max) {
    struct { struct user_group *all; int count, max; } t = { out, 0, max };
    char *buf = read_file(_PATH_GROUP);
    if (buf) {
        each_line(buf, group_line, &t);
        free(buf);
    }
    return t.count;
}

/* Is `user` one of the comma-separated `members`? */
static bool listed(const char *members, const char *user) {
    size_t n = strlen(user);
    for (const char *p = members; *p;) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && strncmp(p, user, n) == 0) {
            return true;
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return false;
}

/* Take `user` out of `members` (in place). */
static void unlist(char *members, const char *user) {
    char out[sizeof(((struct user_group *)0)->members)] = "";
    size_t n = strlen(user);
    for (char *p = members; *p;) {
        char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len && !(len == n && strncmp(p, user, n) == 0)) {
            if (out[0]) {
                strlcat(out, ",", sizeof(out));
            }
            size_t o = strlen(out);
            if (o + len < sizeof(out)) {
                memcpy(out + o, p, len);
                out[o + len] = '\0';
            }
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    strlcpy(members, out, sizeof(out));
}

static int save_groups(const struct user_group *all, int count) {
    char *text = malloc(MAX_FILE);
    if (!text) {
        return -ENOMEM;
    }
    size_t o = 0;
    text[0] = '\0';
    for (int i = 0; i < count && o < MAX_FILE; i++) {
        o += (size_t)snprintf(text + o, MAX_FILE - o, "%s:x:%d:%s\n", all[i].name, all[i].gid, all[i].members);
    }
    int r = write_file(_PATH_GROUP, text);
    free(text);
    return r;
}

bool users_in_group(const char *user, const char *group) {
    struct user_group *all = malloc(sizeof(*all) * GROUPS_MAX);
    int n = all ? users_groups_load(all, GROUPS_MAX) : 0;
    bool in = false;
    for (int i = 0; i < n && !in; i++) {
        in = strcmp(all[i].name, group) == 0 && listed(all[i].members, user);
    }
    free(all);
    if (!in) {
        /* Their own group (the gid in /etc/passwd). */
        struct user_account *a = malloc(sizeof(*a));
        if (a && users_find(user, a)) {
            struct user_group *g = malloc(sizeof(*g) * GROUPS_MAX);
            int m = g ? users_groups_load(g, GROUPS_MAX) : 0;
            for (int i = 0; i < m && !in; i++) {
                in = g[i].gid == a->gid && strcmp(g[i].name, group) == 0;
            }
            free(g);
        }
        free(a);
    }
    return in;
}

static int change_group(const char *group, const char *user, bool add) {
    struct user_account *a = malloc(sizeof(*a));
    bool exists = a && users_find(user, a);
    free(a);
    if (!exists) {
        return -ENOENT;
    }
    struct user_group *all = malloc(sizeof(*all) * GROUPS_MAX);
    if (!all) {
        return -ENOMEM;
    }
    int n = users_groups_load(all, GROUPS_MAX), r = -ENOENT;
    for (int i = 0; i < n; i++) {
        if (strcmp(all[i].name, group) != 0) {
            continue;
        }
        bool in = listed(all[i].members, user);
        if (add && in) {
            r = -EEXIST;
        } else if (!add && !in) {
            r = -ENOENT;
        } else {
            if (add) {
                if (strlen(all[i].members) + strlen(user) + 2 > sizeof(all[i].members)) {
                    r = -ENOSPC;
                    break;
                }
                if (all[i].members[0]) {
                    strlcat(all[i].members, ",", sizeof(all[i].members));
                }
                strlcat(all[i].members, user, sizeof(all[i].members));
            } else {
                unlist(all[i].members, user);
            }
            r = save_groups(all, n);
        }
        break;
    }
    free(all);
    return r;
}

int users_group_add(const char *group, const char *user) {
    return change_group(group, user, true);
}

int users_group_remove(const char *group, const char *user) {
    return change_group(group, user, false);
}

/* --- saving accounts -------------------------------------------------------------------------------- */

/* Rewrite /etc/passwd and /etc/shadow from `all`; and /etc/group, which gets a group for each
   account that's missing one, and loses the group and the memberships of the account that went
   (`removed`). */
static int save(const struct user_account *all, int count, const char *removed) {
    char *passwd = malloc(MAX_FILE), *shadow = malloc(MAX_FILE);
    struct user_group *groups = malloc(sizeof(*groups) * GROUPS_MAX);
    int r = -ENOMEM;
    if (passwd && shadow && groups) {
        size_t p = 0, s = 0;
        for (int i = 0; i < count; i++) {
            const struct user_account *a = &all[i];
            p += (size_t)snprintf(passwd + p, MAX_FILE - p, "%s:x:%d:%d:%s:%s:%s\n", a->name, a->uid, a->gid,
                                  a->display, a->home, a->shell);
            s += (size_t)snprintf(shadow + s, MAX_FILE - s, "%s:%s:%ld:0:99999:7:::\n", a->name, a->password,
                                  a->changed);
        }
        int n = users_groups_load(groups, GROUPS_MAX);
        for (int i = 0; removed && i < n; i++) {
            if (strcmp(groups[i].name, removed) == 0) {
                memmove(&groups[i], &groups[i + 1], sizeof(groups[0]) * (size_t)(n - i - 1));
                n--;
                i--;
            } else {
                unlist(groups[i].members, removed);
            }
        }
        for (int i = 0; i < count; i++) {
            bool has = false;
            for (int j = 0; j < n && !has; j++) {
                has = strcmp(groups[j].name, all[i].name) == 0;
            }
            if (!has && is_person(&all[i]) && n < GROUPS_MAX) {
                memset(&groups[n], 0, sizeof(groups[n]));
                strlcpy(groups[n].name, all[i].name, sizeof(groups[n].name));
                groups[n].gid = all[i].gid;
                n++;
            }
        }
        r = write_file(_PATH_PASSWD, passwd);
        if (r == 0) {
            r = write_file(_PATH_SHADOW, shadow);
        }
        if (r == 0) {
            r = save_groups(groups, n);
        }
    }
    free(passwd);
    free(shadow);
    free(groups);
    return r;
}

/* --- passwords ------------------------------------------------------------------------------------ */

static void to_hex(const uint8_t *d, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = digits[d[i] >> 4];
        out[i * 2 + 1] = digits[d[i] & 15];
    }
    out[n * 2] = '\0';
}

static int from_hex(const char *s, uint8_t *out, size_t max) {
    size_t n = 0;
    while (s[0] && s[1] && n < max) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) {
                return -1;
            }
            v = v * 16 + d;
        }
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return (int)n;
}

void users_sha256_hex(const void *data, size_t len, char *out) {
    struct sha256 c;
    uint8_t d[32];
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, d);
    to_hex(d, 32, out);
}

/* A salt: there's no random device yet, so hash what varies (the TSC, the time, the pid). */
static void random_salt(uint8_t out[SALT_BYTES]) {
    struct {
        uint64_t tsc, us, pid, time;
        uint8_t stack;
    } mix;
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    mix.tsc = (uint64_t)hi << 32 | lo;
    mix.us = uptime_us();
    mix.pid = (uint64_t)getpid();
    mix.time = wall_time();
    mix.stack = (uint8_t)(uintptr_t)&mix;
    struct sha256 c;
    uint8_t d[32];
    sha256_init(&c);
    sha256_update(&c, &mix, sizeof(mix));
    sha256_final(&c, d);
    memcpy(out, d, SALT_BYTES);
}

static void hash_password(const char *password, char *out, size_t size) {
    if (!password[0]) {
        out[0] = '\0';
        return;
    }
    uint8_t salt[SALT_BYTES], key[32];
    random_salt(salt);
    pbkdf2_sha256(password, strlen(password), salt, SALT_BYTES, ITERATIONS, key);
    char salt_hex[SALT_BYTES * 2 + 1], key_hex[65];
    to_hex(salt, SALT_BYTES, salt_hex);
    to_hex(key, 32, key_hex);
    snprintf(out, size, "pbkdf2-sha256$%d$%s$%s", ITERATIONS, salt_hex, key_hex);
}

bool users_has_password(const struct user_account *a) {
    return a->password[0] != '\0';
}

bool users_is_locked(const struct user_account *a) {
    return a->password[0] == '!';
}

bool users_check_password(const struct user_account *a, const char *password) {
    if (!users_has_password(a)) {
        return true;
    }
    /* pbkdf2-sha256$<iterations>$<salt>$<key> */
    const char *p = a->password;
    if (strncmp(p, "pbkdf2-sha256$", 14) != 0) {
        return false;
    }
    p += 14;
    long iterations = strtol(p, (char **)&p, 10);
    if (*p != '$' || iterations < 1 || iterations > 10000000) {
        return false;
    }
    char salt_hex[80];
    const char *dollar = strchr(p + 1, '$');
    if (!dollar || (size_t)(dollar - p - 1) >= sizeof(salt_hex)) {
        return false;
    }
    copy_field(salt_hex, sizeof(salt_hex), p + 1, dollar);
    uint8_t salt[40], want[32], got[32];
    int salt_len = from_hex(salt_hex, salt, sizeof(salt));
    if (salt_len <= 0 || from_hex(dollar + 1, want, sizeof(want)) != 32) {
        return false;
    }
    pbkdf2_sha256(password, strlen(password), salt, (size_t)salt_len, (uint32_t)iterations, got);
    uint8_t diff = 0;   /* (compare every byte: no early exit) */
    for (int i = 0; i < 32; i++) {
        diff |= got[i] ^ want[i];
    }
    return diff == 0;
}

/* --- changes ------------------------------------------------------------------------------------- */

static long today(void) {
    return (long)(wall_time() / 86400);
}

static int change(const char *name, int (*fn)(struct user_account *a, const void *arg), const void *arg) {
    struct user_account *all = malloc(sizeof(*all) * USERS_MAX);
    if (!all) {
        return -ENOMEM;
    }
    int n = load_all(all, USERS_MAX), r = -ENOENT;
    for (int i = 0; i < n; i++) {
        if (strcmp(all[i].name, name) == 0) {
            r = fn(&all[i], arg);
            r = r < 0 ? r : save(all, n, NULL);
            break;
        }
    }
    free(all);
    return r;
}

static int set_password(struct user_account *a, const void *arg) {
    hash_password(arg, a->password, sizeof(a->password));
    a->changed = today();
    return 0;
}

static int set_locked(struct user_account *a, const void *arg) {
    bool lock = *(const bool *)arg;
    if (lock && a->password[0] != '!') {
        memmove(a->password + 1, a->password, sizeof(a->password) - 1);
        a->password[0] = '!';
        a->password[sizeof(a->password) - 1] = '\0';
    } else if (!lock && a->password[0] == '!') {
        memmove(a->password, a->password + 1, sizeof(a->password) - 1);
    }
    return 0;
}

static int set_display(struct user_account *a, const void *arg) {
    const char *d = arg;
    if (strchr(d, ':') || strchr(d, '\n')) {
        return -EINVAL;
    }
    strlcpy(a->display, d, sizeof(a->display));
    return 0;
}

int users_set_password(const char *name, const char *password) {
    return change(name, set_password, password);
}

int users_set_locked(const char *name, bool locked) {
    return change(name, set_locked, &locked);
}

int users_set_display(const char *name, const char *display) {
    return change(name, set_display, display);
}

int users_set_picture(const char *name, const char *path) {
    struct user_account *a = malloc(sizeof(*a));
    bool found = a && users_find(name, a);
    free(a);
    if (!found) {
        return -ENOENT;
    }
    return config_set(dde_config_path(name), "Picture", path);
}

int users_add(const char *name, const char *display, const char *password) {
    if (!users_valid_name(name) || !display || strchr(display, ':') || strchr(display, '\n')) {
        return -EINVAL;
    }
    struct user_account *all = malloc(sizeof(*all) * USERS_MAX);
    if (!all) {
        return -ENOMEM;
    }
    int n = load_all(all, USERS_MAX), uid = USERS_FIRST_UID, r = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(all[i].name, name) == 0) {
            r = -EEXIST;
        }
        if (is_person(&all[i]) && all[i].uid >= uid) {
            uid = all[i].uid + 1;
        }
    }
    if (r == 0 && n == USERS_MAX) {
        r = -ENOSPC;
    }
    if (r == 0) {
        struct user_account *a = &all[n];
        memset(a, 0, sizeof(*a));
        strlcpy(a->name, name, sizeof(a->name));
        strlcpy(a->display, display[0] ? display : name, sizeof(a->display));
        a->uid = a->gid = uid;
        snprintf(a->home, sizeof(a->home), _PATH_HOME "/%s", name);
        strlcpy(a->shell, _PATH_SHELL, sizeof(a->shell));
        if (password) {
            hash_password(password, a->password, sizeof(a->password));
        } else {
            strlcpy(a->password, "!", sizeof(a->password));   /* locked until it gets a password */
        }
        a->changed = today();
        r = save(all, n + 1, NULL);
    }
    free(all);
    if (r == 0) {
        make_home_dirs(name);
    }
    return r;
}

int users_remove(const char *name) {
    struct user_account *all = malloc(sizeof(*all) * USERS_MAX);
    if (!all) {
        return -ENOMEM;
    }
    int n = load_all(all, USERS_MAX), r = -ENOENT;
    for (int i = 0; i < n; i++) {
        if (strcmp(all[i].name, name) == 0) {
            memmove(&all[i], &all[i + 1], sizeof(all[0]) * (size_t)(n - i - 1));
            r = save(all, n - 1, name);
            break;
        }
    }
    free(all);
    return r;
}

/* --- pictures ------------------------------------------------------------------------------------ */

struct gfx_surface *users_picture(const struct user_account *a, int size) {
    struct gfx_surface *img = image_load(a && a->picture[0] ? a->picture : DEFAULT_PICTURE, NULL);
    if (!img && a && a->picture[0]) {
        img = image_load(DEFAULT_PICTURE, NULL);
    }
    if (!img) {
        return NULL;
    }
    /* Cover a square, then cut a circle out of it. */
    struct gfx_surface *s = gfx_surface_create(size, size);
    if (s) {
        int side = img->width < img->height ? img->width : img->height;
        struct gfx_rect from = gfx_rect((img->width - side) / 2, (img->height - side) / 2, side, side);
        gfx_draw_scaled(s, gfx_rect(0, 0, size, size), img, &from);
        float r = size / 2.0f;
        for (int y = 0; y < size; y++) {
            uint32_t *row = gfx_row(s, y);
            for (int x = 0; x < size; x++) {
                float dx = x + 0.5f - r, dy = y + 0.5f - r;
                float cover = r - (float)sqrt(dx * dx + dy * dy) + 0.5f;
                if (cover < 1) {
                    row[x] = cover <= 0 ? 0 : gfx_scale(row[x], (uint32_t)(cover * 255));
                }
            }
        }
    }
    gfx_surface_destroy(img);
    return s;
}
