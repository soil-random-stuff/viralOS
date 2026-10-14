/*
 * System Monitor: what's running and how busy the computer is (like Task Manager).
 *
 *   Processes    every program: its name (the app's, with its icon, for apps), user, status, CPU,
 *                memory and PID. Click a column to sort by it (again: the other way). Select one
 *                and End task (or Delete, or right click) to stop it.
 *   Performance  CPU, memory and each network adapter, with the last minute as a graph.
 *
 * It looks again every second. CPU use is out of all the CPUs together (100% = every core busy),
 * like Task Manager: from the CPU time each program used (os_proc_info.cpu_ms) and the time the
 * CPUs were idle (os_sysinfo.idle_ms).
 */

#include <config.h>
#include <gui.h>
#include <image.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PROCS     128
#define MAX_APPS      48
#define MAX_NETS      4
#define HISTORY       60          /* seconds of graph */
#define TICK_MS       1000

#define TABS_H        48
#define HEADER_H      32
#define ROW_H         30
#define FOOTER_H      52
#define SIDE_W        250
#define ICON          18
#define PAD           16

#define WHITE         GFX_RGB(255, 255, 255)
#define BACKGROUND    GFX_RGB(0xF7, 0xF7, 0xF9)
#define INK           GFX_RGB(0x1C, 0x1C, 0x22)
#define INK_SOFT      GFX_RGB(0x6A, 0x6A, 0x74)
#define INK_FAINT     GFX_RGB(0xB0, 0xB0, 0xB8)
#define LINE          GFX_RGB(0xEA, 0xEA, 0xEE)
#define HOVER         GFX_RGB(0xF0, 0xF1, 0xF5)
#define SELECTED      GFX_RGB(0xDD, 0xE8, 0xFC)
#define ACCENT        GFX_RGB(0x2F, 0x6F, 0xEB)
#define CONTROL       GFX_RGB(0xEC, 0xEC, 0xF0)
#define CONTROL_HOT   GFX_RGB(0xE2, 0xE2, 0xE8)
#define DANGER        GFX_RGB(0xE0, 0x3E, 0x3E)
#define CPU_COLOR     GFX_RGB(0x2F, 0x6F, 0xEB)
#define MEM_COLOR     GFX_RGB(0x8E, 0x5B, 0xE8)
#define NET_COLOR     GFX_RGB(0xE8, 0x7B, 0x2C)

enum tab { TAB_PROCESSES, TAB_PERFORMANCE };
enum column { COL_NAME, COL_USER, COL_STATUS, COL_CPU, COL_MEMORY, COL_PID, COL_COUNT };
enum { MENU_END_TASK = 1 };

static const char *const column_titles[COL_COUNT] = { "Name", "User", "Status", "CPU", "Memory", "PID" };

struct proc {
    int64_t pid, parent;
    char name[32];               /* the program's */
    char shown[48];              /* its app's name, or the program's */
    char user[OS_USER_MAX];
    uint32_t state;
    uint64_t cpu_ms, memory;
    float cpu;                   /* % of all the CPUs, over the last second */
    struct gfx_surface *icon;    /* (the app's; not owned) */
};

struct app {
    char program[32];            /* Exec's program name */
    char name[48];
    struct gfx_surface *icon;
};

struct net {
    char name[16], model[64];
    bool up;
    uint32_t address;
    uint64_t rx_bytes, tx_bytes;
    float rx[HISTORY], tx[HISTORY];   /* kbit/s */
};

static struct {
    struct gui *g;
    struct gui_window *w;
    struct ttf_font *ui, *bold;
    enum tab tab;

    /* What's running. */
    struct proc procs[MAX_PROCS];
    int count;
    int64_t selected;            /* a pid, or 0 */
    enum column sort;
    bool descending;
    int scroll, hover_row, hover_control;
    bool pressing_end;

    /* The apps, for names and icons. */
    struct app apps[MAX_APPS];
    int app_count;

    /* How busy. */
    struct os_sysinfo sys, prev_sys;
    bool have_prev;
    float cpu[HISTORY], mem[HISTORY];   /* % */
    struct net nets[MAX_NETS];
    int net_count;
    int perf_item;               /* 0 CPU, 1 memory, 2+ networks */
    uint64_t last_tick;
} m;

/* Controls the mouse can be over (besides rows). */
enum control { CTRL_NONE = -1, CTRL_TAB_PROCESSES, CTRL_TAB_PERFORMANCE, CTRL_END_TASK,
               CTRL_HEADER /* + column */, CTRL_PERF_ITEM = CTRL_HEADER + COL_COUNT /* + item */ };

/* --- apps: names and icons ------------------------------------------------------------------------ */

static void load_apps(void) {
    int fd = open(_PATH_APPLICATIONS, O_RDONLY);
    struct os_file_info info;
    for (uint64_t i = 0; fd >= 0 && readdir(fd, i, &info) == 0 && m.app_count < MAX_APPS; i++) {
        size_t n = strlen(info.name);
        if (info.type != OS_FILE || n < 9 || strcmp(info.name + n - 8, ".desktop") != 0) {
            continue;
        }
        char path[OS_PATH_MAX], exec[OS_PATH_MAX], icon[OS_PATH_MAX];
        struct app *a = &m.apps[m.app_count];
        snprintf(path, sizeof(path), _PATH_APPLICATIONS "/%s", info.name);
        if (!config_get(path, "Exec", exec, sizeof(exec)) || !config_get(path, "Name", a->name, sizeof(a->name))) {
            continue;
        }
        char *space = strchr(exec, ' ');
        if (space) {
            *space = '\0';
        }
        const char *slash = strrchr(exec, '/');
        strlcpy(a->program, slash ? slash + 1 : exec, sizeof(a->program));
        a->icon = NULL;
        if (config_get(path, "Icon", icon, sizeof(icon))) {
            struct gfx_surface *img = image_load(icon, NULL);
            if (img) {
                a->icon = gfx_surface_scaled(img, ICON, ICON);
                gfx_surface_destroy(img);
            }
        }
        bool duplicate = false;
        for (int k = 0; k < m.app_count; k++) {
            duplicate |= strcmp(m.apps[k].program, a->program) == 0;
        }
        if (duplicate) {
            gfx_surface_destroy(a->icon);
            continue;
        }
        m.app_count++;
    }
    if (fd >= 0) {
        close(fd);
    }
}

/* The system's own programs, which have no launcher: what to call them, and their icon. */
static const struct { const char *program, *name, *icon; } system_programs[] = {
    { "divine-desktop", "Desktop (DDE)", _PATH_DDE_RES "/Icons/Apps/AppsMenu.png" },
    { "divine-login", "Login screen", _PATH_DDE_RES "/Icons/Misc/DefaultPfp.png" },
    { "shell", "Shell", _PATH_DDE_RES "/Icons/Apps/Terminal.png" },
    { "playsound", "Sound player", _PATH_DDE_RES "/Icons/Status/VolumeHigh.png" },
    { "ntfsd", "NTFS drive server", _PATH_DDE_RES "/Icons/FilesFolders/ExternalStorage.png" },
    { "ext4d", "ext4 drive server", _PATH_DDE_RES "/Icons/FilesFolders/ExternalStorage.png" },
};

static void load_system_programs(void) {
    for (size_t i = 0; i < sizeof(system_programs) / sizeof(system_programs[0]) && m.app_count < MAX_APPS; i++) {
        struct app *a = &m.apps[m.app_count++];
        strlcpy(a->program, system_programs[i].program, sizeof(a->program));
        strlcpy(a->name, system_programs[i].name, sizeof(a->name));
        struct gfx_surface *img = image_load(system_programs[i].icon, NULL);
        a->icon = img ? gfx_surface_scaled(img, ICON, ICON) : NULL;
        gfx_surface_destroy(img);
    }
}

static const struct app *app_for(const char *program) {
    for (int i = 0; i < m.app_count; i++) {
        if (strcmp(m.apps[i].program, program) == 0) {
            return &m.apps[i];
        }
    }
    return NULL;
}

/* --- sampling ------------------------------------------------------------------------------------- */

static void push(float *history, float value) {
    memmove(history, history + 1, sizeof(float) * (HISTORY - 1));
    history[HISTORY - 1] = value;
}

static int compare(const void *pa, const void *pb) {
    const struct proc *a = pa, *b = pb;
    int r = 0;
    switch (m.sort) {
    case COL_NAME:   r = strcmp(a->shown, b->shown); break;
    case COL_USER:   r = strcmp(a->user, b->user); break;
    case COL_STATUS: r = (int)a->state - (int)b->state; break;
    case COL_CPU:    r = a->cpu < b->cpu ? -1 : a->cpu > b->cpu; break;
    case COL_MEMORY: r = a->memory < b->memory ? -1 : a->memory > b->memory; break;
    default:         break;
    }
    if (r == 0) {
        r = a->pid < b->pid ? -1 : a->pid > b->pid;
    }
    return m.descending ? -r : r;
}

static void sample(void) {
    static struct proc prev[MAX_PROCS];
    int prev_count = m.count;
    memcpy(prev, m.procs, sizeof(prev[0]) * (size_t)prev_count);

    m.prev_sys = m.sys;
    sysinfo(&m.sys);
    uint64_t elapsed = m.have_prev ? m.sys.uptime_ms - m.prev_sys.uptime_ms : 0;
    uint32_t cpus = m.sys.cpus ? m.sys.cpus : 1;

    struct os_proc_info info;
    m.count = 0;
    for (uint64_t i = 0; procinfo(i, &info) == 0 && m.count < MAX_PROCS; i++) {
        if (info.state == OS_PROC_ZOMBIE) {
            continue;
        }
        struct proc *p = &m.procs[m.count++];
        memset(p, 0, sizeof(*p));
        p->pid = info.pid;
        p->parent = info.parent;
        strlcpy(p->name, info.name, sizeof(p->name));
        strlcpy(p->user, info.user, sizeof(p->user));
        p->state = info.state;
        p->cpu_ms = info.cpu_ms;
        p->memory = info.memory;
        const struct app *a = app_for(p->name);
        strlcpy(p->shown, a ? a->name : p->name, sizeof(p->shown));
        p->icon = a ? a->icon : NULL;
        for (int k = 0; k < prev_count && elapsed; k++) {
            if (prev[k].pid == p->pid) {
                uint64_t used = p->cpu_ms >= prev[k].cpu_ms ? p->cpu_ms - prev[k].cpu_ms : 0;
                p->cpu = 100.0f * (float)used / (float)(elapsed * cpus);
                p->cpu = p->cpu > 100 ? 100 : p->cpu;
                break;
            }
        }
    }
    qsort(m.procs, (size_t)m.count, sizeof(m.procs[0]), compare);

    /* The whole computer. */
    float cpu = 0;
    if (m.have_prev && elapsed) {
        uint64_t idle = m.sys.idle_ms >= m.prev_sys.idle_ms ? m.sys.idle_ms - m.prev_sys.idle_ms : 0;
        cpu = 100.0f * (1.0f - (float)idle / (float)(elapsed * cpus));
        cpu = cpu < 0 ? 0 : cpu > 100 ? 100 : cpu;
    }
    push(m.cpu, cpu);
    uint64_t used = m.sys.memory_total - m.sys.memory_free;
    float mem = m.sys.memory_total ? 100.0f * (float)used / (float)m.sys.memory_total : 0;
    if (!m.have_prev) {
        for (int i = 0; i < HISTORY; i++) {
            m.mem[i] = mem;   /* (memory doesn't start from nothing: a flat line, not a jump) */
        }
    }
    push(m.mem, mem);

    struct os_netif_info ni;
    int n = 0;
    for (int i = 0; netif_info(i, &ni) == 0 && n < MAX_NETS; i++) {
        if (strcmp(ni.name, "lo") == 0) {
            continue;
        }
        struct net *t = &m.nets[n];
        bool same = n < m.net_count && strcmp(t->name, ni.name) == 0;
        if (!same) {
            memset(t, 0, sizeof(*t));
            strlcpy(t->name, ni.name, sizeof(t->name));
        }
        strlcpy(t->model, ni.model, sizeof(t->model));
        t->up = ni.state == OS_NETIF_CONNECTED;
        t->address = ni.address;
        float rx = 0, tx = 0;
        if (same && elapsed) {
            rx = (float)(ni.rx_bytes - t->rx_bytes) * 8.0f / (float)elapsed;   /* bits/ms = kbit/s */
            tx = (float)(ni.tx_bytes - t->tx_bytes) * 8.0f / (float)elapsed;
        }
        push(t->rx, rx);
        push(t->tx, tx);
        t->rx_bytes = ni.rx_bytes;
        t->tx_bytes = ni.tx_bytes;
        n++;
    }
    m.net_count = n;
    if (m.perf_item >= 2 + m.net_count) {
        m.perf_item = 0;
    }
    m.have_prev = true;
}

/* --- text ----------------------------------------------------------------------------------------- */

static void bytes_text(uint64_t bytes, char *out, size_t size) {
    if (bytes >= 1024ull * 1024 * 1024) {
        snprintf(out, size, "%.1f GB", (double)bytes / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024 * 1024) {
        snprintf(out, size, "%.1f MB", (double)bytes / (1024.0 * 1024));
    } else {
        snprintf(out, size, "%lu KB", (unsigned long)(bytes / 1024));
    }
}

static void rate_text(float kbps, char *out, size_t size) {
    if (kbps >= 1000) {
        snprintf(out, size, "%.1f Mbps", (double)kbps / 1000);
    } else {
        snprintf(out, size, "%.0f Kbps", (double)kbps);
    }
}

static const char *state_text(uint32_t state) {
    switch (state) {
    case OS_PROC_RUNNING:  return "Running";
    case OS_PROC_SLEEPING: return "Sleeping";
    case OS_PROC_BLOCKED:  return "Waiting";
    default:               return "Exited";
    }
}

static void uptime_text(uint64_t ms, char *out, size_t size) {
    uint64_t s = ms / 1000;
    snprintf(out, size, "%lu:%02lu:%02lu:%02lu", (unsigned long)(s / 86400), (unsigned long)(s / 3600 % 24),
             (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
}

static void ip_text(uint32_t a, char *out, size_t size) {
    const uint8_t *b = (const uint8_t *)&a;   /* (network byte order) */
    if (!a) {
        strlcpy(out, "-", size);
    } else {
        snprintf(out, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    }
}

static void text_right(struct gfx_surface *s, struct ttf_font *f, float size, float right, float y, gfx_color c,
                       const char *t) {
    ttf_draw_text(s, f, size, right - ttf_text_width(f, size, t), y, c, t);
}

static void text_clipped(struct gfx_surface *s, struct ttf_font *f, float size, int x, int y, int max,
                         gfx_color color, const char *t) {
    struct gfx_rect old = s->clip;
    gfx_set_clip(s, gfx_rect_intersect(old, gfx_rect(x, y - 20, max, 28)));
    ttf_draw_text(s, f, size, (float)x, (float)y, color, t);
    gfx_set_clip(s, old);
}

/* --- layout --------------------------------------------------------------------------------------- */

static struct gfx_rect tab_rect(int i) {
    return gfx_rect(PAD + i * 118, 9, 112, 30);
}

static struct gfx_rect end_rect(void) {
    struct gfx_surface *s = gui_window_surface(m.w);
    return gfx_rect(s->width - PAD - 100, s->height - FOOTER_H + (FOOTER_H - 32) / 2, 100, 32);
}

static struct gfx_rect list_rect(void) {
    struct gfx_surface *s = gui_window_surface(m.w);
    return gfx_rect(0, TABS_H + HEADER_H, s->width, s->height - TABS_H - HEADER_H - FOOTER_H);
}

/* Column c's left and right edges (the numbers are right-aligned). */
static void column_span(int c, int *left, int *right) {
    struct gfx_surface *s = gui_window_surface(m.w);
    int pid_w = 70, mem_w = 100, cpu_w = 70, status_w = 100, user_w = 110;
    int r = s->width - PAD;
    int edges[COL_COUNT + 1];
    edges[COL_COUNT] = r;
    edges[COL_PID] = r - pid_w;
    edges[COL_MEMORY] = edges[COL_PID] - mem_w;
    edges[COL_CPU] = edges[COL_MEMORY] - cpu_w;
    edges[COL_STATUS] = edges[COL_CPU] - status_w;
    edges[COL_USER] = edges[COL_STATUS] - user_w;
    edges[COL_NAME] = PAD;
    *left = edges[c];
    *right = edges[c + 1];
}

static int max_scroll(void) {
    int h = m.count * ROW_H - list_rect().h;
    return h > 0 ? h : 0;
}

static struct gfx_rect perf_item_rect(int i) {
    return gfx_rect(10, TABS_H + 10 + i * 66, SIDE_W - 20, 60);
}

static int selected_index(void) {
    for (int i = 0; i < m.count; i++) {
        if (m.procs[i].pid == m.selected) {
            return i;
        }
    }
    return -1;
}

/* --- drawing -------------------------------------------------------------------------------------- */

static void draw_button(struct gfx_surface *s, struct gfx_rect r, const char *label, bool enabled, bool hot,
                        gfx_color ink) {
    gfx_fill_rounded_rect(s, r, 7, !enabled ? GFX_RGB(0xF1, 0xF1, 0xF4) : hot ? CONTROL_HOT : CONTROL);
    float w = ttf_text_width(m.ui, 13, label);
    ttf_draw_text(s, m.ui, 13, r.x + (r.w - w) / 2, r.y + r.h / 2.0f + 5, enabled ? ink : INK_FAINT, label);
}

/* A graph of `n` values (oldest first) in r, `top` being the top of the scale. */
static void draw_graph(struct gfx_surface *s, struct gfx_rect r, const float *v, int n, float top, gfx_color color,
                       bool grid) {
    gfx_fill_rect(s, r, WHITE);
    if (grid) {
        for (int i = 1; i < 4; i++) {
            gfx_fill_rect(s, gfx_rect(r.x, r.y + r.h * i / 4, r.w, 1), LINE);
        }
        for (int i = 1; i < 6; i++) {
            gfx_fill_rect(s, gfx_rect(r.x + r.w * i / 6, r.y, 1, r.h), LINE);
        }
    }
    gfx_color fill = GFX_RGBA((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF, 38);
    float step = (float)(r.w - 1) / (float)(n - 1);
    for (int x = 0; x < r.w; x++) {
        float at = x / step;
        int i = (int)at;
        float t = at - i;
        float val = i + 1 < n ? v[i] * (1 - t) + v[i + 1] * t : v[n - 1];
        int h = (int)(val / top * (r.h - 2) + 0.5f);
        h = h < 0 ? 0 : h > r.h ? r.h : h;
        if (h > 0) {
            gfx_fill_rect(s, gfx_rect(r.x + x, r.y + r.h - h, 1, h), fill);
        }
    }
    for (int i = 0; i + 1 < n; i++) {
        float y0 = r.y + r.h - 1 - v[i] / top * (r.h - 2), y1 = r.y + r.h - 1 - v[i + 1] / top * (r.h - 2);
        gfx_draw_line(s, r.x + i * step, y0, r.x + (i + 1) * step, y1, 1.5f, color);
    }
    gfx_stroke_rounded_rect(s, r, 0, 1, color);
}

static float graph_top(const float *a, const float *b) {
    float top = 64;   /* kbit/s: at least this, so a quiet line isn't all noise */
    for (int i = 0; i < HISTORY; i++) {
        top = a[i] > top ? a[i] : top;
        top = b && b[i] > top ? b[i] : top;
    }
    return top * 1.15f;
}

static void draw_processes(struct gfx_surface *s) {
    /* The column titles. */
    gfx_fill_rect(s, gfx_rect(0, TABS_H, s->width, HEADER_H), WHITE);
    gfx_fill_rect(s, gfx_rect(0, TABS_H + HEADER_H - 1, s->width, 1), LINE);
    for (int c = 0; c < COL_COUNT; c++) {
        int l, r;
        column_span(c, &l, &r);
        bool numeric = c == COL_CPU || c == COL_MEMORY || c == COL_PID;
        char title[32];
        snprintf(title, sizeof(title), "%s%s", column_titles[c],
                 m.sort == (enum column)c ? (m.descending ? " \xE2\x96\xBE" : " \xE2\x96\xB4") : "");
        if (m.hover_control == CTRL_HEADER + c) {
            gfx_fill_rect(s, gfx_rect(l - 6, TABS_H, r - l + 6, HEADER_H - 1), HOVER);
        }
        struct ttf_font *f = m.sort == (enum column)c ? m.bold : m.ui;
        if (numeric) {
            text_right(s, f, 12.5f, r - 8, TABS_H + 21, INK_SOFT, title);
        } else {
            ttf_draw_text(s, f, 12.5f, l, TABS_H + 21, INK_SOFT, title);
        }
    }

    /* The rows. */
    struct gfx_rect list = list_rect();
    gfx_fill_rect(s, list, WHITE);
    gfx_set_clip(s, list);
    for (int i = 0; i < m.count; i++) {
        int y = list.y + i * ROW_H - m.scroll;
        if (y + ROW_H <= list.y || y >= list.y + list.h) {
            continue;
        }
        const struct proc *p = &m.procs[i];
        struct gfx_rect row = gfx_rect(0, y, s->width, ROW_H);
        if (p->pid == m.selected) {
            gfx_fill_rect(s, row, SELECTED);
        } else if (i == m.hover_row) {
            gfx_fill_rect(s, row, HOVER);
        }
        int l, r;
        float base = y + ROW_H / 2.0f + 5;
        column_span(COL_NAME, &l, &r);
        if (p->icon) {
            gfx_blit(s, l, y + (ROW_H - ICON) / 2, p->icon, NULL);
        } else {
            struct gfx_rect t = gfx_rect(l + 1, y + (ROW_H - ICON) / 2 + 1, ICON - 2, ICON - 2);
            gfx_fill_rounded_rect(s, t, 4, GFX_RGB(0xD8, 0xDA, 0xE2));
            char initial[2] = { p->name[0] >= 'a' && p->name[0] <= 'z' ? (char)(p->name[0] - 32) : p->name[0], 0 };
            float iw = ttf_text_width(m.bold, 10, initial);
            ttf_draw_text(s, m.bold, 10, t.x + (t.w - iw) / 2, t.y + 12, INK_SOFT, initial);
        }
        text_clipped(s, m.ui, 13, l + ICON + 10, (int)base, r - l - ICON - 16, INK, p->shown);
        column_span(COL_USER, &l, &r);
        text_clipped(s, m.ui, 13, l, (int)base, r - l - 8, INK_SOFT, p->user[0] ? p->user : "system");
        column_span(COL_STATUS, &l, &r);
        ttf_draw_text(s, m.ui, 13, l, base, INK_SOFT, state_text(p->state));
        char t[32];
        column_span(COL_CPU, &l, &r);
        snprintf(t, sizeof(t), "%.1f%%", (double)p->cpu);
        if (p->cpu >= 0.05f) {
            float heat = p->cpu > 50 ? 1 : p->cpu / 50;
            gfx_fill_rect(s, gfx_rect(l, y + 1, r - l, ROW_H - 2),
                          GFX_RGBA(0xF5, 0xB0, 0x2B, (uint32_t)(30 + heat * 90)));
        }
        text_right(s, m.ui, 13, r - 8, base, INK, t);
        column_span(COL_MEMORY, &l, &r);
        bytes_text(p->memory, t, sizeof(t));
        text_right(s, m.ui, 13, r - 8, base, INK, t);
        column_span(COL_PID, &l, &r);
        snprintf(t, sizeof(t), "%ld", (long)p->pid);
        text_right(s, m.ui, 13, r - 8, base, INK_SOFT, t);
    }
    gfx_reset_clip(s);
    if (max_scroll() > 0) {
        int track = list.h - 8;
        int thumb = track * list.h / (m.count * ROW_H);
        thumb = thumb < 24 ? 24 : thumb;
        int ty = list.y + 4 + (track - thumb) * m.scroll / max_scroll();
        gfx_fill_rounded_rect(s, gfx_rect(s->width - 9, ty, 5, thumb), 2.5f, GFX_RGBA(0, 0, 0, 70));
    }

    /* The footer. */
    struct gfx_rect foot = gfx_rect(0, s->height - FOOTER_H, s->width, FOOTER_H);
    gfx_fill_rect(s, foot, BACKGROUND);
    gfx_fill_rect(s, gfx_rect(0, foot.y, s->width, 1), LINE);
    char summary[96];
    snprintf(summary, sizeof(summary), "%d processes", m.count);
    ttf_draw_text(s, m.ui, 13, PAD, foot.y + FOOTER_H / 2.0f + 5, INK_SOFT, summary);
    bool can_end = selected_index() >= 0;
    draw_button(s, end_rect(), "End task", can_end, m.hover_control == CTRL_END_TASK, DANGER);
}

static void draw_side_item(struct gfx_surface *s, int i, const char *title, const char *value, const float *values,
                           const float *values2, float top, gfx_color color) {
    struct gfx_rect r = perf_item_rect(i);
    if (m.perf_item == i) {
        gfx_fill_rounded_rect(s, r, 8, SELECTED);
    } else if (m.hover_control == CTRL_PERF_ITEM + i) {
        gfx_fill_rounded_rect(s, r, 8, HOVER);
    }
    struct gfx_rect g = gfx_rect(r.x + 8, r.y + 10, 64, 40);
    draw_graph(s, g, values, HISTORY, top, color, false);
    if (values2) {
        float step = (float)(g.w - 1) / (HISTORY - 1);
        for (int k = 0; k + 1 < HISTORY; k++) {
            gfx_draw_line(s, g.x + k * step, g.y + g.h - 1 - values2[k] / top * (g.h - 2), g.x + (k + 1) * step,
                          g.y + g.h - 1 - values2[k + 1] / top * (g.h - 2), 1, GFX_RGBA(0, 0, 0, 90));
        }
    }
    text_clipped(s, m.bold, 13.5f, g.x + g.w + 12, r.y + 26, r.w - g.w - 28, INK, title);
    text_clipped(s, m.ui, 12, g.x + g.w + 12, r.y + 44, r.w - g.w - 28, INK_SOFT, value);
}

static void draw_stat(struct gfx_surface *s, int x, int y, const char *label, const char *value) {
    ttf_draw_text(s, m.ui, 12, x, y, INK_SOFT, label);
    ttf_draw_text(s, m.bold, 17, x, y + 24, INK, value);
}

static void draw_performance(struct gfx_surface *s) {
    gfx_fill_rect(s, gfx_rect(0, TABS_H, SIDE_W, s->height - TABS_H), BACKGROUND);
    gfx_fill_rect(s, gfx_rect(SIDE_W, TABS_H, s->width - SIDE_W, s->height - TABS_H), WHITE);
    gfx_fill_rect(s, gfx_rect(SIDE_W, TABS_H, 1, s->height - TABS_H), LINE);

    char a[64], b[64], c2[64];
    uint64_t used = m.sys.memory_total - m.sys.memory_free;
    snprintf(a, sizeof(a), "%.0f%%", (double)m.cpu[HISTORY - 1]);
    draw_side_item(s, 0, "CPU", a, m.cpu, NULL, 100, CPU_COLOR);
    bytes_text(used, b, sizeof(b));
    bytes_text(m.sys.memory_total, c2, sizeof(c2));
    snprintf(a, sizeof(a), "%s / %s (%.0f%%)", b, c2, (double)m.mem[HISTORY - 1]);
    draw_side_item(s, 1, "Memory", a, m.mem, NULL, 100, MEM_COLOR);
    for (int i = 0; i < m.net_count; i++) {
        struct net *t = &m.nets[i];
        char title[40];
        snprintf(title, sizeof(title), "Ethernet %s", t->name);
        if (t->up) {
            rate_text(t->tx[HISTORY - 1], b, sizeof(b));
            rate_text(t->rx[HISTORY - 1], c2, sizeof(c2));
            snprintf(a, sizeof(a), "S: %s  R: %s", b, c2);
        } else {
            strlcpy(a, "Not connected", sizeof(a));
        }
        draw_side_item(s, 2 + i, title, a, t->rx, t->tx, graph_top(t->rx, t->tx), NET_COLOR);
    }

    /* The chosen one, big. */
    int x = SIDE_W + 24, y = TABS_H + 40, w = s->width - x - 24;
    int gh = s->height - y - 160;
    gh = gh < 80 ? 80 : gh;
    struct gfx_rect graph = gfx_rect(x, y + 34, w, gh);
    int sy = graph.y + graph.h + 34, col = w / 4;
    char t1[48], t2[48], t3[48], t4[48];
    if (m.perf_item == 0) {
        ttf_draw_text(s, m.bold, 22, x, y, INK, "CPU");
        snprintf(t1, sizeof(t1), "x86_64, %u CPU%s", m.sys.cpus, m.sys.cpus == 1 ? "" : "s");
        text_right(s, m.ui, 13, x + w, y, INK_SOFT, t1);
        ttf_draw_text(s, m.ui, 12, x, y + 26, INK_SOFT, "% use over 60 seconds");
        draw_graph(s, graph, m.cpu, HISTORY, 100, CPU_COLOR, true);
        snprintf(t1, sizeof(t1), "%.0f%%", (double)m.cpu[HISTORY - 1]);
        snprintf(t2, sizeof(t2), "%d", m.count);
        uptime_text(m.sys.uptime_ms, t3, sizeof(t3));
        snprintf(t4, sizeof(t4), "%u", m.sys.cpus);
        draw_stat(s, x, sy, "Utilization", t1);
        draw_stat(s, x + col, sy, "Processes", t2);
        draw_stat(s, x + col * 2, sy, "Up time", t3);
        draw_stat(s, x + col * 3, sy, "Cores", t4);
    } else if (m.perf_item == 1) {
        ttf_draw_text(s, m.bold, 22, x, y, INK, "Memory");
        bytes_text(m.sys.memory_total, t1, sizeof(t1));
        text_right(s, m.ui, 13, x + w, y, INK_SOFT, t1);
        ttf_draw_text(s, m.ui, 12, x, y + 26, INK_SOFT, "Memory in use");
        draw_graph(s, graph, m.mem, HISTORY, 100, MEM_COLOR, true);
        bytes_text(used, t1, sizeof(t1));
        bytes_text(m.sys.memory_free, t2, sizeof(t2));
        bytes_text(m.sys.memory_total, t3, sizeof(t3));
        bytes_text(m.sys.kernel_heap_used, t4, sizeof(t4));
        draw_stat(s, x, sy, "In use", t1);
        draw_stat(s, x + col, sy, "Available", t2);
        draw_stat(s, x + col * 2, sy, "Total", t3);
        draw_stat(s, x + col * 3, sy, "Kernel heap", t4);
    } else {
        struct net *t = &m.nets[m.perf_item - 2];
        snprintf(t1, sizeof(t1), "Ethernet (%s)", t->name);
        ttf_draw_text(s, m.bold, 22, x, y, INK, t1);
        text_clipped(s, m.ui, 13, x + w / 2, y, w / 2, INK_SOFT, t->model);
        ttf_draw_text(s, m.ui, 12, x, y + 26, INK_SOFT, "Throughput (orange: received, grey: sent)");
        float top = graph_top(t->rx, t->tx);
        draw_graph(s, graph, t->rx, HISTORY, top, NET_COLOR, true);
        float step = (float)(graph.w - 1) / (HISTORY - 1);
        for (int k = 0; k + 1 < HISTORY; k++) {
            gfx_draw_line(s, graph.x + k * step, graph.y + graph.h - 1 - t->tx[k] / top * (graph.h - 2),
                          graph.x + (k + 1) * step, graph.y + graph.h - 1 - t->tx[k + 1] / top * (graph.h - 2), 1.5f,
                          GFX_RGBA(0, 0, 0, 110));
        }
        rate_text(t->tx[HISTORY - 1], t1, sizeof(t1));
        rate_text(t->rx[HISTORY - 1], t2, sizeof(t2));
        ip_text(t->address, t3, sizeof(t3));
        char total[48], sent[24], got[24];
        bytes_text(t->tx_bytes, sent, sizeof(sent));
        bytes_text(t->rx_bytes, got, sizeof(got));
        snprintf(total, sizeof(total), "%s / %s", sent, got);
        draw_stat(s, x, sy, "Send", t1);
        draw_stat(s, x + col, sy, "Receive", t2);
        draw_stat(s, x + col * 2, sy, "IPv4 address", t3);
        draw_stat(s, x + col * 3, sy, "Sent / received", total);
    }
}

static void draw(void) {
    struct gfx_surface *s = gui_window_surface(m.w);
    gfx_clear(s, BACKGROUND);
    /* The tabs, and a summary on the right. */
    static const char *const names[2] = { "Processes", "Performance" };
    for (int i = 0; i < 2; i++) {
        struct gfx_rect r = tab_rect(i);
        bool on = (int)m.tab == i;
        if (on) {
            gfx_fill_rounded_rect(s, r, 8, WHITE);
            gfx_stroke_rounded_rect(s, r, 8, 1, LINE);
        } else if (m.hover_control == CTRL_TAB_PROCESSES + i) {
            gfx_fill_rounded_rect(s, r, 8, HOVER);
        }
        float w = ttf_text_width(on ? m.bold : m.ui, 13, names[i]);
        ttf_draw_text(s, on ? m.bold : m.ui, 13, r.x + (r.w - w) / 2, r.y + 20, on ? INK : INK_SOFT, names[i]);
    }
    char summary[96];
    snprintf(summary, sizeof(summary), "CPU %.0f%%   Memory %.0f%%", (double)m.cpu[HISTORY - 1],
             (double)m.mem[HISTORY - 1]);
    text_right(s, m.ui, 13, s->width - PAD, 29, INK_SOFT, summary);
    gfx_fill_rect(s, gfx_rect(0, TABS_H - 1, s->width, 1), LINE);

    if (m.tab == TAB_PROCESSES) {
        draw_processes(s);
    } else {
        draw_performance(s);
    }
    gui_window_invalidate(m.w, NULL);
}

/* --- actions -------------------------------------------------------------------------------------- */

static void end_task(void) {
    int i = selected_index();
    if (i < 0) {
        return;
    }
    const struct proc *p = &m.procs[i];
    char title[96], message[256];
    const char *warning = NULL;
    if (strcmp(p->name, "divine-desktop") == 0) {
        warning = "The desktop runs your session: ending it closes every app and logs you out.";
    } else if (strcmp(p->name, "divine-login") == 0) {
        warning = "The login screen keeps the session going: ending it logs you out and leaves the text shell.";
    } else if (p->parent == 0 && strcmp(p->name, "shell") != 0) {
        warning = "It was started by the system: ending it may stop something viralOS needs.";
    }
    if (warning) {
        snprintf(title, sizeof(title), "End %s?", p->shown);
        snprintf(message, sizeof(message), "%s", warning);
        if (!gui_confirm(m.g, m.w, GUI_ICON_WARNING, title, message, "End task", true)) {
            return;
        }
    }
    int r = kill(p->pid);
    if (r < 0 && r != -ENOENT) {
        snprintf(message, sizeof(message), "%s couldn't be stopped: %s.", p->shown, strerror(r));
        gui_message(m.g, m.w, GUI_ICON_ERROR, "Couldn't end the task", message);
        return;
    }
    m.selected = 0;
    sample();
}

static int row_at(int x, int y) {
    (void)x;
    struct gfx_rect list = list_rect();
    if (!gfx_rect_contains(list, x, y)) {
        return -1;
    }
    int i = (y - list.y + m.scroll) / ROW_H;
    return i < m.count ? i : -1;
}

static int control_at(int x, int y) {
    for (int i = 0; i < 2; i++) {
        if (gfx_rect_contains(tab_rect(i), x, y)) {
            return CTRL_TAB_PROCESSES + i;
        }
    }
    if (m.tab == TAB_PROCESSES) {
        if (gfx_rect_contains(end_rect(), x, y)) {
            return CTRL_END_TASK;
        }
        if (y >= TABS_H && y < TABS_H + HEADER_H) {
            for (int c = 0; c < COL_COUNT; c++) {
                int l, r;
                column_span(c, &l, &r);
                if (x >= l - 6 && x < r) {
                    return CTRL_HEADER + c;
                }
            }
        }
    } else {
        for (int i = 0; i < 2 + m.net_count; i++) {
            if (gfx_rect_contains(perf_item_rect(i), x, y)) {
                return CTRL_PERF_ITEM + i;
            }
        }
    }
    return CTRL_NONE;
}

static void click(const struct gui_event *ev) {
    int c = control_at(ev->x, ev->y);
    if (c == CTRL_TAB_PROCESSES || c == CTRL_TAB_PERFORMANCE) {
        m.tab = c == CTRL_TAB_PROCESSES ? TAB_PROCESSES : TAB_PERFORMANCE;
    } else if (c == CTRL_END_TASK) {
        end_task();
    } else if (c >= CTRL_HEADER && c < CTRL_HEADER + COL_COUNT) {
        enum column col = (enum column)(c - CTRL_HEADER);
        if (m.sort == col) {
            m.descending = !m.descending;
        } else {
            m.sort = col;
            m.descending = col == COL_CPU || col == COL_MEMORY;   /* the biggest first */
        }
        qsort(m.procs, (size_t)m.count, sizeof(m.procs[0]), compare);
    } else if (c >= CTRL_PERF_ITEM) {
        m.perf_item = c - CTRL_PERF_ITEM;
    } else if (m.tab == TAB_PROCESSES) {
        int row = row_at(ev->x, ev->y);
        m.selected = row >= 0 ? m.procs[row].pid : 0;
        if (row >= 0 && ev->button == OS_MOUSE_RIGHT) {
            struct gui_menu_item items[] = { { "End task", MENU_END_TASK, false, false } };
            gui_show_menu(m.g, ev->screen_x, ev->screen_y, items, 1, m.w);
        }
    }
}

static void scroll_to(int s) {
    m.scroll = s < 0 ? 0 : s > max_scroll() ? max_scroll() : s;
}

static bool key(const struct gui_event *ev) {
    if (m.tab != TAB_PROCESSES) {
        return false;
    }
    int i = selected_index();
    if (ev->key == OS_KEY_DELETE) {
        end_task();
        return true;
    }
    if (ev->key == OS_KEY_DOWN || ev->key == OS_KEY_UP) {
        i = ev->key == OS_KEY_DOWN ? (i + 1 < m.count ? i + 1 : m.count - 1) : (i > 0 ? i - 1 : 0);
        if (i >= 0 && i < m.count) {
            m.selected = m.procs[i].pid;
            struct gfx_rect list = list_rect();
            if (i * ROW_H < m.scroll) {
                scroll_to(i * ROW_H);
            } else if ((i + 1) * ROW_H > m.scroll + list.h) {
                scroll_to((i + 1) * ROW_H - list.h);
            }
        }
        return true;
    }
    return false;
}

int main(int argc, char **argv) {
    m.g = gui_open();
    if (!m.g) {
        return 1;
    }
    m.ui = gui_font(m.g, GUI_FONT_REGULAR);
    m.bold = gui_font(m.g, GUI_FONT_BOLD);
    if (!m.ui || !m.bold) {
        return 1;
    }
    if (argc > 1 && strcmp(argv[1], "performance") == 0) {
        m.tab = TAB_PERFORMANCE;
    }
    m.w = gui_window_create(m.g, "System Monitor", -1, -1, 780, 540);
    if (!m.w) {
        return 1;
    }
    m.sort = COL_CPU;
    m.descending = true;
    m.hover_row = -1;
    m.hover_control = CTRL_NONE;
    load_apps();
    load_system_programs();
    sample();
    m.last_tick = uptime_ms();
    draw();

    struct gui_event ev;
    for (;;) {
        uint64_t now = uptime_ms();
        int wait = now - m.last_tick >= TICK_MS ? 0 : (int)(TICK_MS - (now - m.last_tick));
        if (!gui_wait_event(m.g, &ev, wait)) {
            if (uptime_ms() - m.last_tick >= TICK_MS) {
                m.last_tick = uptime_ms();
                sample();
                scroll_to(m.scroll);
                draw();
            }
            continue;
        }
        bool redraw = true;
        switch (ev.type) {
        case GUI_EVENT_QUIT:
        case GUI_EVENT_CLOSE:
            gui_close(m.g);
            return 0;
        case GUI_EVENT_RESIZE:
            scroll_to(m.scroll);
            break;
        case GUI_EVENT_MOUSE_MOVE: {
            int row = m.tab == TAB_PROCESSES ? row_at(ev.x, ev.y) : -1;
            int c = control_at(ev.x, ev.y);
            redraw = row != m.hover_row || c != m.hover_control;
            m.hover_row = row;
            m.hover_control = c;
            break;
        }
        case GUI_EVENT_MOUSE_DOWN:
            click(&ev);
            break;
        case GUI_EVENT_SCROLL:
            scroll_to(m.scroll + ev.wheel * ROW_H * 2);
            break;
        case GUI_EVENT_KEY:
            redraw = key(&ev);
            break;
        case GUI_EVENT_MENU:
            if (ev.menu_id == MENU_END_TASK) {
                end_task();
            }
            break;
        default:
            redraw = false;
            break;
        }
        if (redraw) {
            draw();
        }
    }
}
