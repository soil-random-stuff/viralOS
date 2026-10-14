/*
 * Icons on the desktop: whatever is in ~/Desktop, on a grid.
 *
 * Each icon has a cell. Normally icons stay where they're put (dragging
 * them moves them; they snap to the grid). A new icon takes the first free
 * cell, top to bottom, left to right; when the desktop is full there's no
 * free cell, so it lands on the last one, on top of what's there.
 * With "Auto arrange icons" on, they're kept packed in name order instead,
 * and when that's more than fits, the extra ones are pushed off to the
 * right, past the edge of the screen.
 *
 * Click to select (Ctrl / Shift for more), drag on the wallpaper for a
 * selection rectangle, double-click to open. Right click on the wallpaper:
 * new folder, new empty file, copy, cut, paste, icon size, auto arrange.
 * Right click on icons: open, cut, copy, rename, delete. Dragging icons
 * (onto folders, other windows) is in drag.c.
 *
 * The folder is checked once a second, so changes made by other programs
 * show up by themselves. Positions and settings are kept in
 * ~/.config/dde/desktop.
 */

#include <config.h>
#include <image.h>
#include <stdlib.h>
#include <string.h>

#include "desktop.h"

#define SETTINGS      DDE_DESKTOP_LAYOUT
#define DOUBLE_CLICK  450
#define DRAG_START    5
#define MARGIN        10
#define APP_CACHE     32
#define MAX_PLACES    256

enum {
    DM_NEW_FOLDER = 1, DM_NEW_FILE, DM_PASTE, DM_SMALL, DM_MEDIUM, DM_LARGE, DM_AUTO, DM_REFRESH,
    IM_OPEN = 20, IM_OPEN_EDITOR, IM_CUT, IM_COPY, IM_RENAME, IM_DELETE,
};

static const struct {
    int icon, cell_w, cell_h;
    float font;
} sizes[3] = {
    { 32, 84, 80, 11.5f },
    { 48, 96, 102, 12.0f },
    { 72, 124, 132, 13.0f },
};

/* App launcher icons, made at the current size. */
static struct {
    char path[FILE_PATH_MAX];
    struct gfx_surface *image;
} app_cache[APP_CACHE];
static int app_cache_len;

/* Where each icon was put, by name (kept across reloads and reboots of the desktop). */
static struct {
    char name[OS_NAME_MAX];
    int col, row;
} places[MAX_PLACES];
static int place_count;

/* Where the wallpaper's menu was opened: new folders/files go there. */
static int menu_x, menu_y;

/* --- the grid ------------------------------------------------------------------------------ */

static void grid_size(struct desktop *d, int *cols, int *rows) {
    struct gfx_rect area = work_area(d);
    *cols = (area.w - 2 * MARGIN) / sizes[d->icon_size].cell_w;
    *rows = (area.h - 2 * MARGIN) / sizes[d->icon_size].cell_h;
    *cols = *cols < 1 ? 1 : *cols;
    *rows = *rows < 1 ? 1 : *rows;
}

static struct gfx_rect cell_rect(struct desktop *d, int col, int row) {
    struct gfx_rect area = work_area(d);
    int cw = sizes[d->icon_size].cell_w, ch = sizes[d->icon_size].cell_h;
    return gfx_rect(area.x + MARGIN + col * cw, area.y + MARGIN + row * ch, cw, ch);
}

/* The cell under a screen point (clamped to the grid). */
static void cell_at(struct desktop *d, int x, int y, int *col, int *row) {
    struct gfx_rect area = work_area(d);
    int cols, rows;
    grid_size(d, &cols, &rows);
    int c = (x - area.x - MARGIN) / sizes[d->icon_size].cell_w;
    int r = (y - area.y - MARGIN) / sizes[d->icon_size].cell_h;
    *col = c < 0 ? 0 : c >= cols ? cols - 1 : c;
    *row = r < 0 ? 0 : r >= rows ? rows - 1 : r;
}

static int find_place(const char *name) {
    for (int i = 0; i < place_count; i++) {
        if (strcmp(places[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void set_place(const char *name, int col, int row) {
    int i = find_place(name);
    if (i < 0) {
        if (place_count == MAX_PLACES) {
            return;
        }
        i = place_count++;
        strlcpy(places[i].name, name, sizeof(places[i].name));
    }
    places[i].col = col;
    places[i].row = row;
}

void icons_renamed(struct desktop *d, const char *old_name, const char *new_name) {
    (void)d;
    int i = find_place(old_name);
    if (i >= 0) {
        strlcpy(places[i].name, new_name, sizeof(places[i].name));
    }
}

/* --- settings ----------------------------------------------------------------------------------- */

static void save_settings(struct desktop *d) {
    int fd = open(SETTINGS, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        return;
    }
    char line[OS_NAME_MAX + 48];
    int n = snprintf(line, sizeof(line), "IconSize=%d\nAutoArrange=%d\n", d->icon_size, d->auto_arrange);
    write(fd, line, (size_t)n);
    /* Only the places of icons that still exist. */
    for (int i = 0; i < d->icon_count; i++) {
        n = snprintf(line, sizeof(line), "Icon=%d,%d,%s\n", d->icons[i].col, d->icons[i].row,
                     d->icons[i].info.name);
        write(fd, line, (size_t)n);
    }
    close(fd);
}

static void load_settings(struct desktop *d) {
    d->icon_size = 1;
    d->auto_arrange = false;
    int fd = open(SETTINGS, O_RDONLY);
    if (fd < 0) {
        return;
    }
    static char text[MAX_PLACES * (OS_NAME_MAX + 16) + 64];
    long len = 0, n;
    while (len < (long)sizeof(text) - 1 && (n = read(fd, text + len, sizeof(text) - 1 - len)) > 0) {
        len += n;
    }
    close(fd);
    text[len > 0 ? len : 0] = '\0';
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        if (strncmp(line, "IconSize=", 9) == 0 && line[9] >= '0' && line[9] <= '2') {
            d->icon_size = line[9] - '0';
        } else if (strncmp(line, "AutoArrange=", 12) == 0) {
            d->auto_arrange = line[12] == '1';
        } else if (strncmp(line, "Icon=", 5) == 0) {
            char *p = line + 5;
            int col = (int)strtol(p, &p, 10);
            if (*p++ != ',') {
                continue;
            }
            int row = (int)strtol(p, &p, 10);
            if (*p++ != ',' || !*p) {
                continue;
            }
            set_place(p, col, row);
        }
    }
}

/* --- looks -------------------------------------------------------------------------------- */

static void clear_images(struct desktop *d) {
    for (int i = 0; i < FILE_ICON_COUNT; i++) {
        gfx_surface_destroy(d->file_icons[i]);
        d->file_icons[i] = NULL;
    }
    for (int i = 0; i < app_cache_len; i++) {
        gfx_surface_destroy(app_cache[i].image);
    }
    app_cache_len = 0;
}

static struct gfx_surface *load_scaled(const char *path, int size) {
    struct gfx_surface *img = image_load_png(path, NULL);
    if (!img) {
        return NULL;
    }
    struct gfx_surface *s = gfx_surface_scaled(img, size, size);
    gfx_surface_destroy(img);
    return s;
}

static void load_images(struct desktop *d) {
    clear_images(d);
    for (int i = 0; i < FILE_ICON_COUNT; i++) {
        d->file_icons[i] = load_scaled(file_icon_path((enum file_icon)i), sizes[d->icon_size].icon);
    }
}

static struct gfx_surface *app_image(struct desktop *d, const char *icon_path) {
    for (int i = 0; i < app_cache_len; i++) {
        if (strcmp(app_cache[i].path, icon_path) == 0) {
            return app_cache[i].image;
        }
    }
    if (app_cache_len == APP_CACHE) {
        return NULL;
    }
    struct gfx_surface *s = load_scaled(icon_path, sizes[d->icon_size].icon);
    if (s) {
        strlcpy(app_cache[app_cache_len].path, icon_path, FILE_PATH_MAX);
        app_cache[app_cache_len++].image = s;
    }
    return s;
}

/* Everything the icons cover (for redrawing). */
static struct gfx_rect icons_bounds(struct desktop *d) {
    struct gfx_rect b = { 0, 0, 0, 0 };
    for (int i = 0; i < d->icon_count; i++) {
        b = gfx_rect_empty(b) ? d->icons[i].rect : gfx_rect_union(b, d->icons[i].rect);
    }
    return b;
}

/* Give every icon a cell (see the top of the file), and save the result. */
static void layout(struct desktop *d) {
    int cols, rows;
    grid_size(d, &cols, &rows);
    if (d->auto_arrange) {
        /* Packed in order; past the last column they go off the right edge. */
        for (int i = 0; i < d->icon_count; i++) {
            d->icons[i].col = i / rows;
            d->icons[i].row = i % rows;
        }
    } else {
        bool *taken = calloc((size_t)(cols * rows), sizeof(bool));
        bool placed[MAX_ICONS] = { false };
        /* Icons that have a place keep it (unless two claim the same cell). */
        for (int i = 0; i < d->icon_count && taken; i++) {
            int p = find_place(d->icons[i].info.name);
            if (p < 0 || places[p].col >= cols || places[p].row >= rows || places[p].col < 0 ||
                places[p].row < 0 || taken[places[p].col * rows + places[p].row]) {
                continue;
            }
            d->icons[i].col = places[p].col;
            d->icons[i].row = places[p].row;
            taken[places[p].col * rows + places[p].row] = true;
            placed[i] = true;
        }
        /* New ones: the first free cell; none free (desktop full): the last cell, on top. */
        for (int i = 0; i < d->icon_count; i++) {
            if (placed[i]) {
                continue;
            }
            int cell = cols * rows - 1;
            for (int k = 0; taken && k < cols * rows; k++) {
                if (!taken[k]) {
                    cell = k;
                    break;
                }
            }
            if (taken) {
                taken[cell] = true;
            }
            d->icons[i].col = cell / rows;
            d->icons[i].row = cell % rows;
        }
        free(taken);
    }
    for (int i = 0; i < d->icon_count; i++) {
        d->icons[i].rect = cell_rect(d, d->icons[i].col, d->icons[i].row);
        set_place(d->icons[i].info.name, d->icons[i].col, d->icons[i].row);
    }
    save_settings(d);
}

/* --- reading the folder ----------------------------------------------------------------------- */

static int compare(const void *a, const void *b) {
    const struct desk_icon *x = a, *y = b;
    if (x->info.type != y->info.type) {
        return x->info.type == OS_DIRECTORY ? -1 : 1;
    }
    return name_compare(x->label, y->label);
}

/* A cheap fingerprint of the folder: names, sizes and times. */
static uint32_t folder_signature(void) {
    int fd = open(DESKTOP_DIR, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    uint32_t h = 2166136261u;
    struct os_file_info info;
    for (uint64_t i = 0; readdir(fd, i, &info) == 0; i++) {
        for (const char *p = info.name; *p; p++) {
            h = (h ^ (uint8_t)*p) * 16777619u;
        }
        h = (h ^ (uint32_t)info.mtime ^ (uint32_t)info.size ^ info.type) * 16777619u;
    }
    close(fd);
    return h;
}

static void update_cut_marks(struct desktop *d) {
    for (int i = 0; i < d->icon_count; i++) {
        char path[FILE_PATH_MAX];
        bool cut;
        path_join(path, sizeof(path), DESKTOP_DIR, d->icons[i].info.name);
        d->icons[i].cut = clipboard_files_has(path, &cut) && cut;
    }
}

/* Read the folder again, keeping the selection by name (or selecting `select_name`). */
static void reload(struct desktop *d, const char *select_name) {
    static char keep[MAX_ICONS][OS_NAME_MAX];
    int kept = 0;
    if (select_name) {
        strlcpy(keep[kept++], select_name, sizeof(keep[0]));
    } else {
        for (int i = 0; i < d->icon_count; i++) {
            if (d->icons[i].selected) {
                strlcpy(keep[kept++], d->icons[i].info.name, sizeof(keep[0]));
            }
        }
    }
    damage(d, icons_bounds(d));

    d->icon_count = 0;
    int fd = open(DESKTOP_DIR, O_RDONLY);
    struct os_file_info info;
    for (uint64_t i = 0; fd >= 0 && d->icon_count < MAX_ICONS && readdir(fd, i, &info) == 0; i++) {
        struct desk_icon *ic = &d->icons[d->icon_count++];
        memset(ic, 0, sizeof(*ic));
        ic->info = info;
        strlcpy(ic->label, info.name, sizeof(ic->label));
        ic->image = d->file_icons[file_icon_for(&info)];
        if (info.type == OS_FILE && strcmp(path_extension(info.name), "desktop") == 0) {
            /* An app launcher looks like its app. */
            char path[FILE_PATH_MAX], name[OS_NAME_MAX], icon[FILE_PATH_MAX];
            path_join(path, sizeof(path), DESKTOP_DIR, info.name);
            if (app_file_read(path, name, sizeof(name), icon, sizeof(icon), NULL, 0)) {
                if (name[0]) {
                    strlcpy(ic->label, name, sizeof(ic->label));
                }
                struct gfx_surface *img = icon[0] ? app_image(d, icon) : NULL;
                if (img) {
                    ic->image = img;
                }
            }
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    qsort(d->icons, (size_t)d->icon_count, sizeof(d->icons[0]), compare);
    for (int i = 0; i < d->icon_count; i++) {
        for (int k = 0; k < kept; k++) {
            if (strcmp(d->icons[i].info.name, keep[k]) == 0) {
                d->icons[i].selected = true;
                d->icon_focus = i;
            }
        }
    }
    layout(d);
    update_cut_marks(d);
    d->icons_signature = folder_signature();
    d->icon_hover = -1;
    damage(d, icons_bounds(d));
}

void icons_reload(struct desktop *d) {
    reload(d, NULL);
}

void icons_init(struct desktop *d) {
    load_settings(d);
    d->icon_hover = d->icon_focus = d->icon_click_index = -1;
    d->drag.target = -1;
    load_images(d);
    reload(d, NULL);
}

void icons_tick(struct desktop *d) {
    uint64_t now = uptime_ms();
    if (now - d->icons_checked_ms < 1000 || d->drag.active) {
        return;
    }
    d->icons_checked_ms = now;
    if (folder_signature() != d->icons_signature) {
        reload(d, NULL);
    }
    /* Cut marks change when something is cut or pasted elsewhere. */
    bool before[MAX_ICONS];
    for (int i = 0; i < d->icon_count; i++) {
        before[i] = d->icons[i].cut;
    }
    update_cut_marks(d);
    for (int i = 0; i < d->icon_count; i++) {
        if (before[i] != d->icons[i].cut) {
            damage(d, d->icons[i].rect);
        }
    }
}

static void set_icon_size(struct desktop *d, int size) {
    if (size == d->icon_size) {
        return;
    }
    damage(d, icons_bounds(d));
    d->icon_size = size;
    load_images(d);
    reload(d, NULL);
}

static void set_auto_arrange(struct desktop *d, bool on) {
    damage(d, icons_bounds(d));
    d->auto_arrange = on;
    layout(d);
    damage(d, icons_bounds(d));
}

void icons_set_size(struct desktop *d, int size) {
    set_icon_size(d, size);
    save_settings(d);
}

void icons_set_auto_arrange(struct desktop *d, bool on) {
    if (on != d->auto_arrange) {
        set_auto_arrange(d, on);
        save_settings(d);
    }
}

/* --- drawing ------------------------------------------------------------------------------------ */

/* White text with a soft dark edge, readable on any wallpaper. */
static void shadowed_text(struct desktop *d, float size, float x, float y, const char *text) {
    ttf_draw_text(&d->screen, d->regular, size, x + 1, y + 1, GFX_RGBA(0, 0, 0, 160), text);
    ttf_draw_text(&d->screen, d->regular, size, x, y, GFX_RGB(255, 255, 255), text);
}

/* The label in up to two lines, split at a space if possible, cut short with "..." after that. */
static void draw_label(struct desktop *d, const struct desk_icon *ic, float top) {
    float size = sizes[d->icon_size].font, max = ic->rect.w - 8;
    char first[OS_NAME_MAX], rest[OS_NAME_MAX] = "";
    strlcpy(first, ic->label, sizeof(first));
    if (ttf_text_width(d->regular, size, first) > max) {
        int len = (int)strlen(first), fit = len;
        char tmp[OS_NAME_MAX];
        while (fit > 1) {
            memcpy(tmp, first, (size_t)fit);
            tmp[fit] = '\0';
            if (ttf_text_width(d->regular, size, tmp) <= max) {
                break;
            }
            fit--;
        }
        int cut = fit;
        for (int i = fit; i > 0; i--) {
            if (first[i] == ' ') {
                cut = i;
                break;
            }
        }
        strlcpy(rest, first + cut + (first[cut] == ' '), sizeof(rest));
        first[cut] = '\0';
    }
    const char *lines[2] = { first, rest };
    for (int l = 0; l < 2 && lines[l][0]; l++) {
        char base[OS_NAME_MAX], text[OS_NAME_MAX + 4];
        strlcpy(base, lines[l], sizeof(base));
        strlcpy(text, base, sizeof(text));
        while (strlen(base) > 1 && ttf_text_width(d->regular, size, text) > max) {
            base[strlen(base) - 1] = '\0';
            snprintf(text, sizeof(text), "%s...", base);
        }
        float w = ttf_text_width(d->regular, size, text);
        shadowed_text(d, size, ic->rect.x + (ic->rect.w - w) / 2, top + size + l * (size + 4), text);
    }
}

static struct gfx_rect marquee_rect(struct desktop *d) {
    int x0 = d->icon_press_x, y0 = d->icon_press_y, x1 = d->mouse_x, y1 = d->mouse_y;
    return gfx_rect(x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, (x0 > x1 ? x0 - x1 : x1 - x0) + 1,
                    (y0 > y1 ? y0 - y1 : y1 - y0) + 1);
}

void icons_compose(struct desktop *d, struct gfx_rect area) {
    if (!d->regular) {
        return;
    }
    int icon = sizes[d->icon_size].icon;
    for (int i = 0; i < d->icon_count; i++) {
        const struct desk_icon *ic = &d->icons[i];
        if (gfx_rect_empty(gfx_rect_intersect(ic->rect, area))) {
            continue;
        }
        struct gfx_rect box = gfx_rect_inset(ic->rect, 3);
        bool target = d->drag.active && d->drag.target == i;
        if (ic->selected || target) {
            gfx_fill_rounded_rect(&d->screen, box, 8, GFX_RGBA(255, 255, 255, target ? 95 : 70));
            gfx_stroke_rounded_rect(&d->screen, box, 8, 1, GFX_RGBA(255, 255, 255, 120));
        } else if (i == d->icon_hover && !d->drag.active) {
            gfx_fill_rounded_rect(&d->screen, box, 8, GFX_RGBA(255, 255, 255, 36));
        }
        if (ic->image) {
            gfx_blit(&d->screen, ic->rect.x + (ic->rect.w - icon) / 2, ic->rect.y + 8, ic->image, NULL);
            if (ic->cut) {
                /* Cut: shown faded until pasted. */
                gfx_fill_rounded_rect(&d->screen, gfx_rect(ic->rect.x + (ic->rect.w - icon) / 2, ic->rect.y + 8,
                                                           icon, icon), 6, GFX_RGBA(255, 255, 255, 110));
            }
        }
        draw_label(d, ic, ic->rect.y + 8 + icon + 4);
    }
    if (d->icon_marquee) {
        struct gfx_rect m = marquee_rect(d);
        gfx_fill_rect(&d->screen, m, GFX_RGBA(0x2F, 0x6F, 0xEB, 50));
        gfx_fill_rect(&d->screen, gfx_rect(m.x, m.y, m.w, 1), ACCENT);
        gfx_fill_rect(&d->screen, gfx_rect(m.x, m.y + m.h - 1, m.w, 1), ACCENT);
        gfx_fill_rect(&d->screen, gfx_rect(m.x, m.y, 1, m.h), ACCENT);
        gfx_fill_rect(&d->screen, gfx_rect(m.x + m.w - 1, m.y, 1, m.h), ACCENT);
    }
}

/* --- selecting ---------------------------------------------------------------------------------- */

/* The topmost icon under a point (later ones are drawn on top). */
int icon_at(struct desktop *d, int x, int y);
int icon_at(struct desktop *d, int x, int y) {
    for (int i = d->icon_count - 1; i >= 0; i--) {
        if (gfx_rect_contains(gfx_rect_inset(d->icons[i].rect, 3), x, y)) {
            return i;
        }
    }
    return -1;
}

static int selected_count(struct desktop *d) {
    int n = 0;
    for (int i = 0; i < d->icon_count; i++) {
        n += d->icons[i].selected;
    }
    return n;
}

static void set_selected(struct desktop *d, int i, bool on) {
    if (d->icons[i].selected != on) {
        d->icons[i].selected = on;
        damage(d, d->icons[i].rect);
    }
}

static void select_only(struct desktop *d, int which) {
    for (int i = 0; i < d->icon_count; i++) {
        set_selected(d, i, i == which);
    }
    if (which >= 0) {
        d->icon_focus = which;
    }
}

static void icon_path(int i, struct desktop *d, char *out, size_t size) {
    path_join(out, size, DESKTOP_DIR, d->icons[i].info.name);
}

static void open_icon(struct desktop *d, int i) {
    char path[FILE_PATH_MAX];
    icon_path(i, d, path, sizeof(path));
    long pid = open_file(path);
    if (pid < 0) {
        char message[160];
        if (pid == -ENOEXEC) {
            snprintf(message, sizeof(message), "No app can open \"%s\" yet.", d->icons[i].label);
        } else {
            snprintf(message, sizeof(message), "%s: %s", d->icons[i].label, strerror((int)pid));
        }
        desk_dialog_open(d, DESK_DIALOG_ERROR, "Can't open", message, NULL, NULL);
    }
}

/* --- moving icons on the grid (called by drag.c) ---------------------------------------------------- */

/*
 * The selected icons were dropped with the grabbed one's cell under (x, y):
 * all of them move by the same number of cells. A cell that's taken by an
 * icon that isn't moving sends that one to the nearest free cell.
 */
void icons_move_selected(struct desktop *d, int grabbed, int x, int y);
void icons_move_selected(struct desktop *d, int grabbed, int x, int y) {
    if (d->auto_arrange || grabbed < 0) {
        damage(d, icons_bounds(d));   /* they go back where they were */
        return;
    }
    int cols, rows, col, row;
    grid_size(d, &cols, &rows);
    cell_at(d, x, y, &col, &row);
    int dc = col - d->icons[grabbed].col, dr = row - d->icons[grabbed].row;
    bool *taken = calloc((size_t)(cols * rows), sizeof(bool));
    if (!taken) {
        return;
    }
    damage(d, icons_bounds(d));
    for (int i = 0; i < d->icon_count; i++) {
        struct desk_icon *ic = &d->icons[i];
        if (!ic->selected && ic->col < cols && ic->row < rows) {
            taken[ic->col * rows + ic->row] = true;
        }
    }
    for (int i = 0; i < d->icon_count; i++) {
        struct desk_icon *ic = &d->icons[i];
        if (!ic->selected) {
            continue;
        }
        int want_c = ic->col + dc, want_r = ic->row + dr;
        want_c = want_c < 0 ? 0 : want_c >= cols ? cols - 1 : want_c;
        want_r = want_r < 0 ? 0 : want_r >= rows ? rows - 1 : want_r;
        /* The nearest free cell to where it should go. */
        int best = -1, best_dist = 1 << 30;
        for (int k = 0; k < cols * rows; k++) {
            if (taken[k]) {
                continue;
            }
            int c = k / rows, r = k % rows;
            int dist = (c - want_c) * (c - want_c) + (r - want_r) * (r - want_r);
            if (dist < best_dist) {
                best_dist = dist;
                best = k;
            }
        }
        if (best < 0) {
            continue;   /* nowhere free: it stays */
        }
        taken[best] = true;
        ic->col = best / rows;
        ic->row = best % rows;
        ic->rect = cell_rect(d, ic->col, ic->row);
        set_place(ic->info.name, ic->col, ic->row);
    }
    free(taken);
    save_settings(d);
    damage(d, icons_bounds(d));
}

/* New icons dropped or created at a screen point: remember that cell (or the nearest free one) for them. */
void icons_place_at(struct desktop *d, char (*names)[OS_NAME_MAX], int count, int x, int y);
void icons_place_at(struct desktop *d, char (*names)[OS_NAME_MAX], int count, int x, int y) {
    if (d->auto_arrange) {
        return;
    }
    int cols, rows, col, row;
    grid_size(d, &cols, &rows);
    cell_at(d, x, y, &col, &row);
    bool *taken = calloc((size_t)(cols * rows), sizeof(bool));
    if (!taken) {
        return;
    }
    for (int i = 0; i < d->icon_count; i++) {
        if (d->icons[i].col < cols && d->icons[i].row < rows) {
            taken[d->icons[i].col * rows + d->icons[i].row] = true;
        }
    }
    for (int n = 0; n < count; n++) {
        int best = -1, best_dist = 1 << 30;
        for (int k = 0; k < cols * rows; k++) {
            int c = k / rows, r = k % rows;
            int dist = (c - col) * (c - col) + (r - row) * (r - row);
            if (!taken[k] && dist < best_dist) {
                best_dist = dist;
                best = k;
            }
        }
        if (best < 0) {
            break;   /* full: layout puts it on the last cell */
        }
        taken[best] = true;
        set_place(names[n], best / rows, best % rows);
    }
    free(taken);
}

/* --- actions ---------------------------------------------------------------------------------------- */

static void cut_or_copy(struct desktop *d, bool cut) {
    static char paths[CLIPBOARD_MAX][FILE_PATH_MAX];
    const char *list[CLIPBOARD_MAX];
    int n = 0;
    for (int i = 0; i < d->icon_count && n < CLIPBOARD_MAX; i++) {
        if (d->icons[i].selected) {
            icon_path(i, d, paths[n], FILE_PATH_MAX);
            list[n] = paths[n];
            n++;
        }
    }
    if (n) {
        clipboard_files_set(list, n, cut);
        d->icons_checked_ms = 0;   /* show the cut marks now */
        icons_tick(d);
    }
}

static void paste(struct desktop *d) {
    char last[OS_NAME_MAX] = "", failed[OS_NAME_MAX] = "";
    int n = clipboard_files_paste(DESKTOP_DIR, last, sizeof(last), failed, sizeof(failed));
    for (int i = 0; i < d->icon_count; i++) {
        set_selected(d, i, false);
    }
    reload(d, last[0] ? last : NULL);
    if (n < 0) {
        char message[160];
        snprintf(message, sizeof(message), "%s: %s",
                 n == -EINVAL ? "A folder can't go inside itself" : failed, strerror(n));
        desk_dialog_open(d, DESK_DIALOG_ERROR, "Can't paste", message, NULL, NULL);
    }
}

/* Make a new folder or empty file where the menu was opened, select it, and offer to name it. */
static void create(struct desktop *d, bool folder) {
    char path[FILE_PATH_MAX];
    free_name(DESKTOP_DIR, folder ? "New folder" : "New file.txt", false, path, sizeof(path));
    int err = folder ? mkdir(path) : open(path, O_WRONLY | O_CREAT);
    if (!folder && err >= 0) {
        close(err);
        err = 0;
    }
    if (err < 0) {
        char message[160];
        snprintf(message, sizeof(message), "%s: %s", path_base(path), strerror(err));
        desk_dialog_open(d, DESK_DIALOG_ERROR, "Can't create it", message, NULL, NULL);
        return;
    }
    char names[1][OS_NAME_MAX];
    strlcpy(names[0], path_base(path), OS_NAME_MAX);
    icons_place_at(d, names, 1, menu_x, menu_y);
    for (int i = 0; i < d->icon_count; i++) {
        set_selected(d, i, false);
    }
    reload(d, path_base(path));
    desk_dialog_open(d, DESK_DIALOG_RENAME, folder ? "Name the new folder" : "Name the new file", NULL,
                     path_base(path), path);
}

static void rename_selected(struct desktop *d) {
    int one = -1;
    for (int i = 0; i < d->icon_count; i++) {
        if (d->icons[i].selected) {
            if (one >= 0) {
                return;   /* only one at a time */
            }
            one = i;
        }
    }
    if (one < 0) {
        return;
    }
    char path[FILE_PATH_MAX];
    icon_path(one, d, path, sizeof(path));
    desk_dialog_open(d, DESK_DIALOG_RENAME, "Rename", NULL, d->icons[one].info.name, path);
}

static void delete_selected(struct desktop *d) {
    int n = selected_count(d);
    if (!n) {
        return;
    }
    char message[160];
    if (n == 1) {
        for (int i = 0; i < d->icon_count; i++) {
            if (d->icons[i].selected) {
                snprintf(message, sizeof(message), "\"%s\"%s will be deleted.", d->icons[i].info.name,
                         d->icons[i].info.type == OS_DIRECTORY ? " and everything in it" : "");
            }
        }
    } else {
        snprintf(message, sizeof(message), "%d items (and everything in the folders) will be deleted.", n);
    }
    desk_dialog_open(d, DESK_DIALOG_DELETE, n == 1 ? "Delete?" : "Delete items?", message, NULL, NULL);
}

/* Called by the delete dialog once confirmed. */
void icons_delete_confirmed(struct desktop *d);
void icons_delete_confirmed(struct desktop *d) {
    for (int i = 0; i < d->icon_count; i++) {
        if (!d->icons[i].selected) {
            continue;
        }
        char path[FILE_PATH_MAX];
        icon_path(i, d, path, sizeof(path));
        int err = delete_tree(path);
        if (err < 0) {
            char message[160];
            snprintf(message, sizeof(message), "%s: %s", d->icons[i].info.name, strerror(err));
            desk_dialog_open(d, DESK_DIALOG_ERROR, "Can't delete", message, NULL, NULL);
            break;
        }
    }
    for (int i = 0; i < d->icon_count; i++) {
        set_selected(d, i, false);
    }
    reload(d, NULL);
}

void icons_menu_chosen(struct desktop *d, int id) {
    int one = -1;
    for (int i = 0; i < d->icon_count; i++) {
        if (d->icons[i].selected) {
            one = one == -1 ? i : -2;
        }
    }
    switch (id) {
    case DM_NEW_FOLDER: create(d, true); break;
    case DM_NEW_FILE:   create(d, false); break;
    case DM_PASTE:      paste(d); break;
    case DM_SMALL:      set_icon_size(d, 0); break;
    case DM_MEDIUM:     set_icon_size(d, 1); break;
    case DM_LARGE:      set_icon_size(d, 2); break;
    case DM_AUTO:       set_auto_arrange(d, !d->auto_arrange); break;
    case DM_REFRESH:    reload(d, NULL); break;
    case IM_OPEN:
        for (int i = 0; i < d->icon_count; i++) {
            if (d->icons[i].selected) {
                open_icon(d, i);
            }
        }
        break;
    case IM_OPEN_EDITOR:
        if (one >= 0) {
            char path[FILE_PATH_MAX];
            icon_path(one, d, path, sizeof(path));
            open_in_text_editor(path);
        }
        break;
    case IM_CUT:        cut_or_copy(d, true); break;
    case IM_COPY:       cut_or_copy(d, false); break;
    case IM_RENAME:     rename_selected(d); break;
    case IM_DELETE:     delete_selected(d); break;
    }
}

static bool can_paste(void) {
    static char paths[1][FILE_PATH_MAX];
    bool cut;
    return clipboard_files_get(paths, 1, &cut) > 0;
}

static void show_menu(struct desktop *d, int i) {
    if (i >= 0) {
        if (!d->icons[i].selected) {
            select_only(d, i);
        }
        int n = selected_count(d);
        bool folder = n == 1 && d->icons[i].info.type == OS_DIRECTORY;
        const struct gui_menu_item items[] = {
            { "Open", IM_OPEN, false, false },
            { "Open with Text Editor", IM_OPEN_EDITOR, n != 1 || folder, false },
            { NULL, 0, false, false },
            { "Cut", IM_CUT, false, false },
            { "Copy", IM_COPY, false, false },
            { NULL, 0, false, false },
            { "Rename", IM_RENAME, n != 1, false },
            { "Delete", IM_DELETE, false, false },
        };
        menu_show(d, d->mouse_x, d->mouse_y, items, 8, MENU_ICON);
        return;
    }
    /* On the wallpaper: Copy and Cut act on whatever is selected. */
    menu_x = d->mouse_x;
    menu_y = d->mouse_y;
    bool none = selected_count(d) == 0;
    const struct gui_menu_item items[] = {
        { "New folder", DM_NEW_FOLDER, false, false },
        { "New empty file", DM_NEW_FILE, false, false },
        { NULL, 0, false, false },
        { "Copy", IM_COPY, none, false },
        { "Cut", IM_CUT, none, false },
        { "Paste", DM_PASTE, !can_paste(), false },
        { NULL, 0, false, false },
        { "Small icons", DM_SMALL, false, d->icon_size == 0 },
        { "Medium icons", DM_MEDIUM, false, d->icon_size == 1 },
        { "Large icons", DM_LARGE, false, d->icon_size == 2 },
        { "Auto arrange icons", DM_AUTO, false, d->auto_arrange },
        { NULL, 0, false, false },
        { "Refresh", DM_REFRESH, false, false },
    };
    menu_show(d, d->mouse_x, d->mouse_y, items, 13, MENU_DESKTOP);
}

/* --- mouse and keys ------------------------------------------------------------------------------- */

bool icons_mouse_down(struct desktop *d, uint32_t button, uint32_t modifiers) {
    int i = icon_at(d, d->mouse_x, d->mouse_y);
    if (button == OS_MOUSE_RIGHT) {
        show_menu(d, i);
        return true;
    }
    if (button != OS_MOUSE_LEFT) {
        return false;
    }
    bool ctrl = modifiers & OS_MOD_CTRL, shift = modifiers & OS_MOD_SHIFT;
    uint64_t now = uptime_ms();
    if (i >= 0 && !ctrl && !shift && i == d->icon_click_index && now - d->icon_click_ms < (uint64_t)d->cfg.double_click) {
        d->icon_click_ms = 0;
        select_only(d, i);
        open_icon(d, i);
        return true;
    }
    d->icon_click_ms = now;
    d->icon_click_index = i;
    if (shift && i >= 0) {
        int from = d->icon_focus >= 0 ? d->icon_focus : i;
        int a = from < i ? from : i, b = from < i ? i : from;
        for (int k = 0; k < d->icon_count; k++) {
            set_selected(d, k, (ctrl && d->icons[k].selected) || (k >= a && k <= b));
        }
    } else if (ctrl && i >= 0) {
        set_selected(d, i, !d->icons[i].selected);
    } else if (!ctrl && !(i >= 0 && d->icons[i].selected)) {
        select_only(d, i);   /* (Ctrl on empty space keeps the selection) */
    }
    d->icon_focus = i >= 0 ? i : d->icon_focus;
    d->icon_pressing = true;
    d->icon_marquee = false;
    d->icon_press_x = d->mouse_x;
    d->icon_press_y = d->mouse_y;
    for (int k = 0; k < d->icon_count; k++) {
        d->icon_marquee_base[k] = ctrl && d->icons[k].selected;
    }
    return true;
}

/* Start dragging the selected icons (pressed on icon `grabbed`). In drag.c. */
void drag_from_icons(struct desktop *d, int grabbed, int press_x, int press_y);

void icons_mouse_move(struct desktop *d) {
    if (!d->icon_pressing) {
        int hover = icon_at(d, d->mouse_x, d->mouse_y);
        if (hover != d->icon_hover) {
            if (d->icon_hover >= 0 && d->icon_hover < d->icon_count) {
                damage(d, d->icons[d->icon_hover].rect);
            }
            if (hover >= 0) {
                damage(d, d->icons[hover].rect);
            }
            d->icon_hover = hover;
        }
        return;
    }
    int dx = d->mouse_x - d->icon_press_x, dy = d->mouse_y - d->icon_press_y;
    if (!d->icon_marquee && dx * dx + dy * dy < DRAG_START * DRAG_START) {
        return;
    }
    /* Pressed on a selected icon: drag the icons. On the wallpaper: a selection rectangle. */
    int pressed = d->icon_marquee ? -1 : icon_at(d, d->icon_press_x, d->icon_press_y);
    if (pressed >= 0 && d->icons[pressed].selected) {
        d->icon_pressing = false;
        drag_from_icons(d, pressed, d->icon_press_x, d->icon_press_y);
        return;
    }
    struct gfx_rect before = d->icon_marquee ? marquee_rect(d) : gfx_rect(0, 0, 0, 0);
    d->icon_marquee = true;
    struct gfx_rect m = marquee_rect(d);
    damage(d, gfx_rect_empty(before) ? m : gfx_rect_union(before, m));
    for (int i = 0; i < d->icon_count; i++) {
        struct gfx_rect body = gfx_rect_inset(d->icons[i].rect, 8);
        bool hit = !gfx_rect_empty(gfx_rect_intersect(body, m));
        set_selected(d, i, d->icon_marquee_base[i] || hit);
    }
}

void icons_mouse_up(struct desktop *d, uint32_t button) {
    if (!d->icon_pressing || button != OS_MOUSE_LEFT) {
        return;
    }
    if (d->icon_marquee) {
        damage(d, marquee_rect(d));
    } else {
        /* A plain click on one icon of a multiple selection: now just that one. */
        int i = icon_at(d, d->mouse_x, d->mouse_y);
        if (i >= 0 && selected_count(d) > 1 && i == d->icon_click_index) {
            select_only(d, i);
        }
    }
    d->icon_pressing = false;
    d->icon_marquee = false;
}

#define KEY_A  0x1E
#define KEY_X  0x2D
#define KEY_C  0x2E
#define KEY_V  0x2F
#define KEY_F2 0x3C

void icons_key(struct desktop *d, const struct os_input_event *in) {
    bool ctrl = in->modifiers & OS_MOD_CTRL;
    if (ctrl) {
        switch (in->key) {
        case KEY_A:
            for (int i = 0; i < d->icon_count; i++) {
                set_selected(d, i, true);
            }
            return;
        case KEY_C: cut_or_copy(d, false); return;
        case KEY_X: cut_or_copy(d, true); return;
        case KEY_V: paste(d); return;
        }
    }
    switch (in->key) {
    case OS_KEY_DELETE: delete_selected(d); return;
    case KEY_F2:        rename_selected(d); return;
    case OS_KEY_ESCAPE: select_only(d, -1); return;
    case OS_KEY_ENTER:
        for (int i = 0; i < d->icon_count; i++) {
            if (d->icons[i].selected) {
                open_icon(d, i);
            }
        }
        return;
    case OS_KEY_UP:
    case OS_KEY_DOWN:
        if (d->icon_count) {
            int i = d->icon_focus < 0 ? 0 : d->icon_focus + (in->key == OS_KEY_DOWN ? 1 : -1);
            i = i < 0 ? 0 : i >= d->icon_count ? d->icon_count - 1 : i;
            select_only(d, i);
        }
        return;
    }
}
