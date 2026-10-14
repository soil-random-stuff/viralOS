/* Wallpaper (the pictures in /usr/share/dde-res/Backgrounds) and Desktop & Panel (icons, clock, pinned apps). */

#include <config.h>
#include <image.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"
#include <paths.h>

#define BACKGROUNDS  _PATH_DDE_RES "/Backgrounds"
#define APPS_DIR     _PATH_APPLICATIONS
#define DESKTOP_FILE DDE_DESKTOP_LAYOUT

/* --- Wallpaper ------------------------------------------------------------------------------------ */

#define MAX_PICTURES 48
#define THUMB_W      168
#define THUMB_H      105
#define PREVIEW_W    336
#define PREVIEW_H    210

struct picture {
    char path[OS_PATH_MAX];
    char name[64];
    struct gfx_surface *thumb;
    bool tried;                  /* decoded (or failed to) */
};

static struct picture pictures[MAX_PICTURES];
static int picture_count;
static char current_wallpaper[OS_PATH_MAX];
static struct gfx_surface *preview;
static char preview_of[OS_PATH_MAX];

/* Scaled to fill w x h, the overflow cropped evenly (as the desktop shows it). */
static struct gfx_surface *cover(const struct gfx_surface *img, int w, int h) {
    struct gfx_surface *s = gfx_surface_create(w, h);
    if (s) {
        float sx = (float)w / img->width, sy = (float)h / img->height, scale = sx > sy ? sx : sy;
        int cw = (int)(w / scale + 0.5f), ch = (int)(h / scale + 0.5f);
        cw = cw > img->width ? img->width : cw;
        ch = ch > img->height ? img->height : ch;
        struct gfx_rect from = gfx_rect((img->width - cw) / 2, (img->height - ch) / 2, cw, ch);
        gfx_draw_scaled(s, gfx_rect(0, 0, w, h), img, &from);
    }
    return s;
}

/* Make the corners transparent (anti-aliased), so the picture sits on the card with round corners. */
static void round_corners(struct gfx_surface *s, float radius) {
    if (!s) {
        return;
    }
    int r = (int)radius + 1;
    for (int y = 0; y < s->height; y++) {
        uint32_t *row = gfx_row(s, y);
        for (int x = 0; x < s->width; x++) {
            int dx = x < r ? r - x : x >= s->width - r ? x - (s->width - r - 1) : 0;
            int dy = y < r ? r - y : y >= s->height - r ? y - (s->height - r - 1) : 0;
            if (!dx || !dy) {
                continue;
            }
            /* Distance from the corner circle's centre, against the radius (1 pixel of soft edge). */
            float fx = dx - 0.5f, fy = dy - 0.5f, d2 = fx * fx + fy * fy, inner = radius - 0.5f;
            if (d2 <= (inner - 0.5f) * (inner - 0.5f)) {
                continue;
            }
            float d = (float)sqrt(d2);
            float cover = inner + 0.5f - d;
            cover = cover < 0 ? 0 : cover > 1 ? 1 : cover;
            row[x] = gfx_scale(row[x], (uint32_t)(cover * 255));
        }
    }
}

/* "seanight.png" -> "Seanight" */
static void pretty_name(const char *file, char *out, size_t size) {
    strlcpy(out, file, size);
    char *dot = strrchr(out, '.');
    if (dot) {
        *dot = '\0';
    }
    if (out[0] >= 'a' && out[0] <= 'z') {
        out[0] = (char)(out[0] - 'a' + 'A');
    }
}

void page_wallpaper_show(void) {
    if (!config_get(DDE_CONFIG, "Wallpaper", current_wallpaper, sizeof(current_wallpaper))) {
        strlcpy(current_wallpaper, BACKGROUNDS "/default.png", sizeof(current_wallpaper));
    }
    if (picture_count == 0) {
        int fd = open(BACKGROUNDS, O_RDONLY);
        struct os_file_info info;
        for (uint64_t i = 0; fd >= 0 && readdir(fd, i, &info) == 0 && picture_count < MAX_PICTURES; i++) {
            if (info.type != OS_FILE || !image_supported_name(info.name)) {
                continue;
            }
            struct picture *p = &pictures[picture_count];
            snprintf(p->path, sizeof(p->path), BACKGROUNDS "/%s", info.name);
            pretty_name(info.name, p->name, sizeof(p->name));
            p->thumb = NULL;
            p->tried = false;
            picture_count++;
        }
        if (fd >= 0) {
            close(fd);
        }
    }
    if (strcmp(preview_of, current_wallpaper) != 0) {
        gfx_surface_destroy(preview);
        preview = NULL;
        preview_of[0] = '\0';   /* (made by page_wallpaper_work) */
    }
}

/* Decoding big pictures takes a while: one at a time between events, so the page shows at once and
   fills in. The current wallpaper first (it's the preview too). */
bool page_wallpaper_work(void) {
    if (!preview_of[0]) {
        struct gfx_surface *img = image_load(current_wallpaper, NULL);
        strlcpy(preview_of, current_wallpaper, sizeof(preview_of));
        if (img) {
            preview = cover(img, PREVIEW_W, PREVIEW_H);
            round_corners(preview, 10);
            for (int i = 0; i < picture_count; i++) {
                if (!pictures[i].tried && strcmp(pictures[i].path, current_wallpaper) == 0) {
                    pictures[i].thumb = cover(img, THUMB_W, THUMB_H);
                    round_corners(pictures[i].thumb, 8);
                    pictures[i].tried = true;
                }
            }
            gfx_surface_destroy(img);
        }
        return true;
    }
    for (int i = 0; i < picture_count; i++) {
        struct picture *p = &pictures[i];
        if (!p->tried) {
            struct gfx_surface *img = image_load(p->path, NULL);
            p->tried = true;
            if (img) {
                p->thumb = cover(img, THUMB_W, THUMB_H);
                round_corners(p->thumb, 8);
                gfx_surface_destroy(img);
            }
            return true;
        }
    }
    return false;
}

static void framed(struct gfx_surface *img, struct gfx_rect r, bool selected) {
    if (ui.measuring) {
        return;
    }
    if (selected) {
        gfx_fill_rounded_rect(ui.s, gfx_rect_inset(r, -5), 13, ACCENT);
        gfx_fill_rounded_rect(ui.s, gfx_rect_inset(r, -2), 10, CARD);
    }
    if (img) {
        /* Rounded corners: draw it, then cut the corners with the card's colour. */
        gfx_blit(ui.s, r.x, r.y, img, NULL);
    } else {
        gfx_fill_rounded_rect(ui.s, r, 8, GFX_RGB(0xD5, 0xD5, 0xDB));
    }
}

void page_wallpaper(void) {
    ui_card_begin();
    struct gfx_rect r = ui_row(PREVIEW_H + 70);
    struct gfx_rect pr = gfx_rect(r.x + (r.w - PREVIEW_W) / 2, r.y + 20, PREVIEW_W, PREVIEW_H);
    framed(preview, pr, false);
    const char *slash = strrchr(current_wallpaper, '/');
    char name[64];
    pretty_name(slash ? slash + 1 : current_wallpaper, name, sizeof(name));
    float tw = ui_text_width(FONT_BODY, true, name);
    ui_text(FONT_BODY, true, r.x + (r.w - tw) / 2, pr.y + PREVIEW_H + 30, INK, name);
    ui_card_end();

    ui_heading("Pictures");
    ui_card_begin();
    int gap = 18, inner = ui.width - 32;
    int cols = (inner + gap) / (THUMB_W + gap);
    cols = cols < 1 ? 1 : cols;
    int rows = (picture_count + cols - 1) / cols;
    struct gfx_rect grid = ui_row(rows * (THUMB_H + 40) + 16);
    int x0 = grid.x + 16 + (inner - (cols * THUMB_W + (cols - 1) * gap)) / 2;
    for (int i = 0; i < picture_count; i++) {
        struct picture *p = &pictures[i];
        struct gfx_rect t = gfx_rect(x0 + (i % cols) * (THUMB_W + gap), grid.y + 16 + (i / cols) * (THUMB_H + 40),
                                     THUMB_W, THUMB_H);
        bool selected = strcmp(p->path, current_wallpaper) == 0;
        if (ui_clicked(gfx_rect(t.x, t.y, t.w, t.h + 26), p)) {
            config_set(DDE_CONFIG, "Wallpaper", p->path);
            gui_settings_changed(ui.g, -1, -1);
            page_wallpaper_show();
            ui.dirty = true;
        }
        framed(p->thumb, t, selected);
        float nw = ui_text_width(FONT_SMALL, false, p->name);
        ui_text(FONT_SMALL, false, t.x + (t.w - nw) / 2, t.y + t.h + 20, selected ? ACCENT : INK, p->name);
    }
    ui_card_end();
    ui_note("Add your own: put PNG, JPEG or WebP pictures in /usr/share/dde-res/Backgrounds.");
}

/* --- Desktop & Panel ------------------------------------------------------------------------------ */

#define MAX_APPS 32

struct app_entry {
    char launcher[OS_PATH_MAX];
    char id[64];                 /* the launcher's name without ".desktop" (what Pinned= lists) */
    char name[48];
    bool pinned;
    struct gfx_surface *icon;
};

static struct app_entry apps[MAX_APPS];
static int app_count;
static int icon_size;
static bool auto_arrange, clock24;
static const char *const sizes[] = { "Small", "Medium", "Large" };

static void unquote(char *v) {
    size_t n = strlen(v);
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
        memmove(v, v + 1, n - 2);
        v[n - 2] = '\0';
    }
}

void page_desktop_show(void) {
    icon_size = (int)config_get_int(DESKTOP_FILE, "IconSize", 1);
    auto_arrange = config_get_int(DESKTOP_FILE, "AutoArrange", 0) != 0;
    clock24 = config_get_int(DDE_CONFIG, "Clock24", 1) != 0;
    for (int i = 0; i < app_count; i++) {
        gfx_surface_destroy(apps[i].icon);
    }
    app_count = 0;
    char pinned[1024] = "";
    config_get(DDE_CONFIG, "Pinned", pinned, sizeof(pinned));
    int fd = open(APPS_DIR, O_RDONLY);
    struct os_file_info info;
    const size_t ext = strlen(".desktop");
    for (uint64_t i = 0; fd >= 0 && readdir(fd, i, &info) == 0 && app_count < MAX_APPS; i++) {
        size_t n = strlen(info.name);
        if (info.type != OS_FILE || n <= ext || strcmp(info.name + n - ext, ".desktop") != 0) {
            continue;
        }
        struct app_entry *a = &apps[app_count];
        snprintf(a->launcher, sizeof(a->launcher), APPS_DIR "/%s", info.name);
        if (!config_get(a->launcher, "Name", a->name, sizeof(a->name))) {
            continue;
        }
        unquote(a->name);
        strlcpy(a->id, info.name, n - ext + 1 < sizeof(a->id) ? n - ext + 1 : sizeof(a->id));
        a->pinned = config_list_has(pinned, a->id);
        char value[160];
        a->icon = NULL;
        if (config_get(a->launcher, "Icon", value, sizeof(value))) {
            unquote(value);
            struct gfx_surface *img = image_load(value, NULL);
            if (img) {
                a->icon = gfx_surface_scaled(img, 28, 28);
                gfx_surface_destroy(img);
            }
        }
        app_count++;
    }
    if (fd >= 0) {
        close(fd);
    }
    /* By name. */
    for (int i = 1; i < app_count; i++) {
        for (int j = i; j > 0 && strcmp(apps[j - 1].name, apps[j].name) > 0; j--) {
            struct app_entry t = apps[j];
            apps[j] = apps[j - 1];
            apps[j - 1] = t;
        }
    }
}

/* Pinned= in the user's DDE settings: the pinned apps' launcher names. */
static void save_pinned(void) {
    char list[1024] = "";
    for (int i = 0; i < app_count; i++) {
        if (apps[i].pinned) {
            if (list[0]) {
                strlcat(list, ",", sizeof(list));
            }
            strlcat(list, apps[i].id, sizeof(list));
        }
    }
    config_set(DDE_CONFIG, "Pinned", list);
}

void page_desktop(void) {
    ui_heading("Desktop");
    ui_card_begin();
    if (ui_segmented_row("Icon size", sizes, 3, &icon_size)) {
        gui_settings_changed(ui.g, icon_size, -1);
    }
    if (ui_toggle_row("Arrange icons automatically", "Keep them in a grid, sorted by name", &auto_arrange)) {
        gui_settings_changed(ui.g, -1, auto_arrange);
    }
    ui_card_end();

    ui_heading("Panel");
    ui_card_begin();
    if (ui_toggle_row("24-hour clock", NULL, &clock24)) {
        config_set_int(DDE_CONFIG, "Clock24", clock24);
        gui_settings_changed(ui.g, -1, -1);
    }
    ui_card_end();

    ui_heading("Pinned apps");
    ui_card_begin();
    for (int i = 0; i < app_count; i++) {
        struct app_entry *a = &apps[i];
        struct gfx_rect r = ui_row(48);
        if (a->icon && !ui.measuring) {
            gfx_blit(ui.s, r.x + 16, r.y + 10, a->icon, NULL);
        }
        ui_text_clipped(FONT_BODY, false, r.x + 56, r.y + 29, r.w - 140, INK, a->name);
        if (ui_toggle(gfx_rect(r.x + r.w - 56, r.y + 12, 40, 24), &a->pinned)) {
            save_pinned();
            gui_settings_changed(ui.g, -1, -1);
        }
    }
    ui_card_end();
    ui_note("Pinned apps always have a place on the panel, running or not.");
}
