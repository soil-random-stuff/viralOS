/* Disks: the window. A list of disks on the left; on the right the selected disk, a bar with its
   volumes, buttons for what can be done with the selected one, and its details. */

#include "disks.h"

#include <fileops.h>
#include <gfx.h>
#include <gui.h>
#include <image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ttf.h>
#include <unistd.h>
#include <paths.h>

#define WIN_W        940
#define WIN_H        640
#define SIDEBAR_W    250
#define ROW_H        58
#define PAD          28
#define BAR_H        96
#define TOOL_H       36
#define INFO_ROW     25

#define WHITE        GFX_RGB(255, 255, 255)
#define INK          GFX_RGB(0x1C, 0x1C, 0x22)
#define INK_SOFT     GFX_RGB(0x78, 0x78, 0x82)
#define LINE         GFX_RGB(0xE2, 0xE3, 0xE8)
#define FIELD        GFX_RGB(0xF4, 0xF5, 0xF8)
#define SIDEBAR_BG   GFX_RGB(0xF8, 0xF9, 0xFB)
#define ACCENT       GFX_RGB(0x2F, 0x6F, 0xEB)
#define ACCENT_DARK  GFX_RGB(0x20, 0x55, 0xC0)
#define DANGER       GFX_RGB(0xE0, 0x3E, 0x3E)
#define SELECTED     GFX_RGB(0xE3, 0xEC, 0xFD)
#define FREE_FILL    GFX_RGB(0xEE, 0xEF, 0xF2)

enum { BTN_NONE = -1, BTN_MENU, BTN_MOUNT, BTN_GEAR, BTN_ADD, BTN_DELETE, BTN_COUNT };

enum {
    MENU_FORMAT_VOL = 1, MENU_EDIT_PART, MENU_EDIT_FS, MENU_MOUNT_OPTS, MENU_RESIZE, MENU_CHECK, MENU_REPAIR,
    MENU_FORMAT_DISK = 20, MENU_EJECT, MENU_REFRESH,
};

enum dlg_kind {
    DLG_NONE, DLG_FORMAT_VOL, DLG_FORMAT_DISK, DLG_CREATE, DLG_EDIT_PART, DLG_LABEL, DLG_MOUNT_OPTS, DLG_RESIZE,
    DLG_BUSY,
};

enum field_type { F_TEXT, F_CHOICE, F_CHECK, F_SIZE };

struct field {
    enum field_type type;
    char label[64];
    char text[OS_PATH_MAX];      /* F_SIZE: the number, in `unit`s ("" = all of max_bytes) */
    int cursor;
    const char *choices[12];
    int nchoices;
    int selected;
    bool checked;
    bool digits;                 /* a number */
    int unit;                    /* F_SIZE: units[] */
    uint64_t max_bytes;          /* F_SIZE: the slider's right end */
};

/* F_SIZE's units, chosen from a menu (ids MENU_UNIT + index). */
static const struct {
    const char *name;
    uint64_t bytes;
} units[] = {
    { "B", 1 }, { "KB", 1000 }, { "KiB", 1024 }, { "MB", 1000000 }, { "MiB", 1ull << 20 },
    { "GB", 1000000000 }, { "GiB", 1ull << 30 },
};
#define UNIT_COUNT ((int)(sizeof(units) / sizeof(units[0])))
#define MENU_UNIT  200

struct dialog {
    enum dlg_kind kind;
    char title[96];
    char message[1200];
    struct field fields[4];
    int nfields;
    int focus;                   /* the text field typing goes to */
    const char *ok;              /* the main button (NULL: only "Close") */
    bool danger;
    int hover, pressed;          /* buttons: 0 = main, 1 = cancel */
    int sliding;                 /* the F_SIZE field whose slider is being dragged, or -1 */
    bool was_checked;            /* DLG_MOUNT_OPTS: writing was allowed before */
    int choice_map[12];          /* DLG_EDIT_PART: which part_types[] each choice is */
};

static struct {
    struct gui *g;
    struct gui_window *w;
    struct ttf_font *regular, *bold;
    struct gfx_surface *drive_icons[2];   /* [0] internal, [1] external (USB) */
    int disk, vol;
    int hover_disk, hover_vol, hover_btn, pressed_btn;
    struct dialog dlg;
    char keep_dev[16];           /* re-select this volume after a refresh */
    uint64_t last_refresh;
} ui;

static struct gfx_surface *surface(void) {
    return gui_window_surface(ui.w);
}

static struct disk *cur_disk(void) {
    return ui.disk >= 0 && ui.disk < disk_count ? &disks[ui.disk] : NULL;
}

static struct volume *cur_vol(void) {
    struct disk *d = cur_disk();
    return d && ui.vol >= 0 && ui.vol < d->nvols ? &d->vols[ui.vol] : NULL;
}

static void group_digits(uint64_t v, char *out, size_t size) {
    char raw[32];
    snprintf(raw, sizeof(raw), "%lu", (unsigned long)v);
    size_t len = strlen(raw), o = 0;
    for (size_t i = 0; i < len && o + 2 < size; i++) {
        if (i && (len - i) % 3 == 0) {
            out[o++] = ',';
        }
        out[o++] = raw[i];
    }
    out[o] = '\0';
}

/* The volumes shown in the bar (an extended partition hides behind its logical ones). */
static int visible(const struct disk *d, int *out) {
    int n = 0;
    for (int i = 0; i < d->nvols; i++) {
        if (!d->vols[i].extended) {
            out[n++] = i;
        }
    }
    return n;
}

/* --- refreshing -------------------------------------------------------------------------------------- */

static void refresh(void) {
    char keep_disk[16] = "";
    struct disk *d = cur_disk();
    struct volume *v = cur_vol();
    if (d) {
        strcpy(keep_disk, d->name);
    }
    char keep_vol[16] = "";
    uint64_t keep_start = 0;
    if (ui.keep_dev[0]) {
        strcpy(keep_vol, ui.keep_dev);
        ui.keep_dev[0] = '\0';
    } else if (v) {
        strcpy(keep_vol, v->dev);
        keep_start = v->start;
    }
    model_refresh();
    ui.disk = disk_count ? 0 : -1;
    for (int i = 0; i < disk_count; i++) {
        if (strcmp(disks[i].name, keep_disk) == 0 || (keep_vol[0] && strncmp(keep_vol, disks[i].name, strlen(disks[i].name)) == 0 && keep_disk[0] == '\0')) {
            ui.disk = i;
        }
    }
    ui.vol = -1;
    if ((d = cur_disk())) {
        int vis[MAX_VOLUMES], n = visible(d, vis);
        for (int k = 0; k < n; k++) {
            struct volume *x = &d->vols[vis[k]];
            if ((keep_vol[0] && strcmp(x->dev, keep_vol) == 0) || (!keep_vol[0] && x->start == keep_start)) {
                ui.vol = vis[k];
            }
        }
        if (ui.vol < 0 && n) {
            ui.vol = vis[0];
        }
    }
    ui.last_refresh = uptime_ms();
}

/* --- geometry ------------------------------------------------------------------------------------------- */

static int right_x(void) {
    return SIDEBAR_W + PAD;
}

static int right_w(void) {
    return surface()->width - SIDEBAR_W - 2 * PAD;
}

static int info_rows(const struct disk *d) {
    return d->serial[0] ? 4 : 3;
}

static struct gfx_rect bar_rect(void) {
    struct disk *d = cur_disk();
    int y = 96 + (d ? info_rows(d) : 3) * INFO_ROW + 44;
    return gfx_rect(right_x(), y, right_w(), BAR_H);
}

static struct gfx_rect segment_rect(int k, int n) {
    struct disk *d = cur_disk();
    struct gfx_rect bar = bar_rect();
    int vis[MAX_VOLUMES];
    visible(d, vis);
    /* Proportional, but at least 80 px each. */
    int min = 80, total_w = bar.w;
    int extra = total_w - min * n;
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) {
        sum += d->vols[vis[i]].sectors;
    }
    int x = bar.x;
    for (int i = 0; i <= k; i++) {
        int w = extra > 0 && sum ? min + (int)((uint64_t)extra * d->vols[vis[i]].sectors / sum) : total_w / n;
        if (i == n - 1) {
            w = bar.x + bar.w - x;
        }
        if (i == k) {
            return gfx_rect(x, bar.y, w, bar.h);
        }
        x += w;
    }
    return gfx_rect(0, 0, 0, 0);
}

static struct gfx_rect button_rect(int b) {
    struct gfx_rect bar = bar_rect();
    int y = bar.y + bar.h + 6;
    switch (b) {
    case BTN_MENU:   return gfx_rect(surface()->width - PAD - 36, 24, 36, 34);
    case BTN_MOUNT:  return gfx_rect(bar.x, y, 40, 32);
    case BTN_GEAR:   return gfx_rect(bar.x + 46, y, 40, 32);
    case BTN_DELETE: return gfx_rect(bar.x + bar.w - 40, y, 40, 32);
    case BTN_ADD:    return gfx_rect(bar.x + bar.w - 86, y, 40, 32);
    }
    return gfx_rect(0, 0, 0, 0);
}

static bool button_shown(int b) {
    struct disk *d = cur_disk();
    struct volume *v = cur_vol();
    if (!d) {
        return false;
    }
    switch (b) {
    case BTN_MENU:   return true;
    case BTN_MOUNT:  return v && v->kind != VOL_FREE && v->fs[0] && strcmp(v->fs, "swap") != 0;
    case BTN_GEAR:   return v && v->kind != VOL_FREE;
    case BTN_ADD:    return v && v->kind == VOL_FREE;
    case BTN_DELETE: return v && v->kind == VOL_PARTITION;
    }
    return false;
}

/* --- drawing ----------------------------------------------------------------------------------------------- */

static void text(float x, float y, bool bold, float size, gfx_color c, const char *s) {
    ttf_draw_text(surface(), bold ? ui.bold : ui.regular, size, x, y, c, s);
}

static float text_w(bool bold, float size, const char *s) {
    return ttf_text_width(bold ? ui.bold : ui.regular, size, s);
}

/* Draw text wrapped at `width` (and at newlines); returns the height used. */
static int wrapped(int x, int y, int width, float size, gfx_color c, const char *s) {
    char line[256];
    int lines = 0;
    const char *p = s;
    while (*p) {
        size_t len = 0, fit = 0;
        while (p[len] && p[len] != '\n' && len < sizeof(line) - 1) {
            line[len] = p[len];
            line[len + 1] = '\0';
            if (text_w(false, size, line) > (float)width && fit) {
                break;
            }
            if (p[len] == ' ') {
                fit = len;
            }
            len++;
        }
        if (p[len] && p[len] != '\n' && fit) {
            len = fit;
        }
        memcpy(line, p, len);
        line[len] = '\0';
        text((float)x, (float)(y + lines * (size + 7)), false, size, c, line);
        lines++;
        p += len;
        while (*p == ' ') {
            p++;
        }
        if (*p == '\n') {
            p++;
        }
    }
    return (int)((float)lines * (size + 7));
}

static void load_drive_icons(void) {
    for (int i = 0; i < 2; i++) {
        struct gfx_surface *img = image_load_png(file_icon_path(i ? FILE_ICON_EXTERNAL_DRIVE : FILE_ICON_INTERNAL_DRIVE),
                                                 NULL);
        if (img) {
            ui.drive_icons[i] = gfx_surface_scaled(img, 36, 36);
            gfx_surface_destroy(img);
        }
    }
}

static void draw_sidebar(void) {
    struct gfx_surface *s = surface();
    gfx_fill_rect(s, gfx_rect(0, 0, SIDEBAR_W, s->height), SIDEBAR_BG);
    gfx_fill_rect(s, gfx_rect(SIDEBAR_W - 1, 0, 1, s->height), LINE);
    text(20, 34, true, 15, INK, "Disks");
    for (int i = 0; i < disk_count; i++) {
        struct gfx_rect r = gfx_rect(8, 54 + i * ROW_H, SIDEBAR_W - 16, ROW_H - 4);
        if (i == ui.disk) {
            gfx_fill_rounded_rect(s, r, 10, SELECTED);
        } else if (i == ui.hover_disk) {
            gfx_fill_rounded_rect(s, r, 10, GFX_RGBA(0, 0, 0, 12));
        }
        struct gfx_surface *icon = ui.drive_icons[(disks[i].flags & OS_BLOCK_EXTERNAL) != 0];
        if (icon) {
            gfx_blit(s, r.x + 8, r.y + (r.h - 36) / 2, icon, NULL);
        }
        char title[64];
        disk_title(&disks[i], title, sizeof(title));
        gfx_set_clip(s, gfx_rect(r.x + 52, r.y, r.w - 58, r.h));
        text((float)r.x + 52, (float)r.y + 23, true, 13, INK, title);
        text((float)r.x + 52, (float)r.y + 42, false, 12, INK_SOFT, disks[i].model);
        gfx_reset_clip(s);
    }
    if (!disk_count) {
        wrapped(20, 80, SIDEBAR_W - 40, 12, INK_SOFT, "No disks. Plug in a USB drive.");
    }
}

static void info_row(int y, const char *label, const char *value) {
    int x = right_x();
    float lw = text_w(false, 13, label);
    text((float)x + 128 - lw, (float)y, false, 13, INK_SOFT, label);
    struct gfx_surface *s = surface();
    gfx_set_clip(s, gfx_rect(x + 140, y - 16, right_w() - 140, 24));
    text((float)x + 140, (float)y, false, 13, INK, value);
    gfx_reset_clip(s);
}

static void draw_button(int b, const char *glyph) {
    if (!button_shown(b)) {
        return;
    }
    struct gfx_surface *s = surface();
    struct gfx_rect r = button_rect(b);
    bool danger = b == BTN_DELETE;
    gfx_color fill = danger ? DANGER : FIELD;
    if (ui.pressed_btn == b) {
        fill = danger ? GFX_RGB(0xC0, 0x30, 0x30) : GFX_RGB(0xE2, 0xE3, 0xE8);
    } else if (ui.hover_btn == b) {
        fill = danger ? GFX_RGB(0xEE, 0x50, 0x50) : GFX_RGB(0xEB, 0xEC, 0xF0);
    }
    gfx_fill_rounded_rect(s, r, 8, fill);
    gfx_color ink = danger ? WHITE : INK;
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
    if (strcmp(glyph, "play") == 0) {
        float tri[6] = { cx - 4, cy - 6, cx - 4, cy + 6, cx + 6, cy };
        gfx_fill_polygon(s, tri, 3, ink);
    } else if (strcmp(glyph, "stop") == 0) {
        gfx_fill_rect(s, gfx_rect((int)cx - 5, (int)cy - 5, 10, 10), ink);
    } else if (strcmp(glyph, "gear") == 0) {
        gfx_fill_circle(s, cx, cy, 7, ink);
        for (int k = 0; k < 8; k++) {
            static const float dx[8] = { 0, 0.71f, 1, 0.71f, 0, -0.71f, -1, -0.71f };
            static const float dy[8] = { -1, -0.71f, 0, 0.71f, 1, 0.71f, 0, -0.71f };
            gfx_fill_circle(s, cx + dx[k] * 8, cy + dy[k] * 8, 2.2f, ink);
        }
        gfx_fill_circle(s, cx, cy, 3, fill);
    } else if (strcmp(glyph, "plus") == 0) {
        gfx_fill_rect(s, gfx_rect((int)cx - 6, (int)cy - 1, 12, 2), ink);
        gfx_fill_rect(s, gfx_rect((int)cx - 1, (int)cy - 6, 2, 12), ink);
    } else if (strcmp(glyph, "minus") == 0) {
        gfx_fill_rect(s, gfx_rect((int)cx - 6, (int)cy - 1, 12, 2), ink);
    } else if (strcmp(glyph, "dots") == 0) {
        for (int k = -1; k <= 1; k++) {
            gfx_fill_circle(s, cx, cy + k * 6.0f, 2, ink);
        }
    }
}

static void draw_volume_bar(struct disk *d) {
    struct gfx_surface *s = surface();
    struct gfx_rect bar = bar_rect();
    int vis[MAX_VOLUMES], n = visible(d, vis);
    gfx_fill_rounded_rect(s, gfx_rect(bar.x - 1, bar.y - 1, bar.w + 2, bar.h + 2), 9, LINE);
    gfx_fill_rounded_rect(s, bar, 8, WHITE);
    for (int k = 0; k < n; k++) {
        struct volume *v = &d->vols[vis[k]];
        struct gfx_rect r = segment_rect(k, n);
        bool sel = vis[k] == ui.vol;
        gfx_color fill = sel ? ACCENT : v->kind == VOL_FREE ? FREE_FILL : vis[k] == ui.hover_vol ? GFX_RGB(0xF2, 0xF5, 0xFC) : WHITE;
        struct gfx_rect in = gfx_rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
        gfx_fill_rounded_rect(s, in, 7, fill);
        if (k > 0) {
            gfx_fill_rect(s, gfx_rect(r.x, r.y + 6, 1, r.h - 12), LINE);
        }
        gfx_color ink = sel ? WHITE : INK, soft = sel ? GFX_RGBA(255, 255, 255, 220) : INK_SOFT;
        char l1[OS_NAME_MAX], l2[64], l3[64], size[32];
        human_size(vol_bytes(d, v), size, sizeof(size));
        if (v->kind == VOL_FREE) {
            snprintf(l1, sizeof(l1), "Free Space");
            snprintf(l2, sizeof(l2), "%s", size);
            l3[0] = '\0';
        } else {
            snprintf(l1, sizeof(l1), "%s", v->label[0] ? v->label : v->part_name[0] ? v->part_name : "");
            if (v->kind == VOL_PARTITION) {
                snprintf(l2, sizeof(l2), "Partition %d", v->number);
            } else {
                snprintf(l2, sizeof(l2), "Whole Disk");
            }
            snprintf(l3, sizeof(l3), "%s %s", size, fs_short(v));
        }
        gfx_set_clip(s, gfx_rect(in.x + 4, in.y, in.w - 8, in.h));
        const char *lines[3] = { l1, l2, l3 };
        int count = 0, ys[3];
        for (int i = 0; i < 3; i++) {
            if (lines[i][0]) {
                count++;
            }
        }
        int y = in.y + in.h / 2 - count * 9 + 12;
        for (int i = 0, j = 0; i < 3; i++) {
            if (!lines[i][0]) {
                continue;
            }
            ys[j] = y + j * 18;
            bool bold = i == 0 && v->kind != VOL_FREE;
            float w = text_w(bold, 12, lines[i]);
            float tx = in.x + (in.w - w) / 2;
            if (tx < in.x + 6) {
                tx = (float)in.x + 6;
            }
            text(tx, (float)ys[j], bold, 12, i == 0 ? ink : soft, lines[i]);
            j++;
        }
        /* A mounted volume gets a small dot. */
        if (v->mounted_at[0]) {
            gfx_fill_circle(s, (float)(in.x + in.w - 12), (float)(in.y + in.h - 12), 4, sel ? WHITE : ACCENT);
        }
        gfx_reset_clip(s);
    }
    draw_button(BTN_MOUNT, cur_vol() && cur_vol()->mounted_at[0] ? "stop" : "play");
    draw_button(BTN_GEAR, "gear");
    draw_button(BTN_ADD, "plus");
    draw_button(BTN_DELETE, "minus");
}

static void draw_details(struct disk *d, struct volume *v) {
    struct gfx_rect bar = bar_rect();
    int y = bar.y + bar.h + 6 + 32 + 34;
    char a[160], b[64], c[64];
    uint64_t bytes = vol_bytes(d, v);
    human_size(bytes, b, sizeof(b));
    if (v->mounted_at[0] && v->fs_size) {
        char f[32];
        human_size(v->fs_free, f, sizeof(f));
        unsigned pct10 = (unsigned)(v->fs_size ? (v->fs_size - v->fs_free) * 1000 / v->fs_size : 0);
        snprintf(a, sizeof(a), "%s \xE2\x80\x94 %s free (%u.%u%% full)", b, f, pct10 / 10, pct10 % 10);
    } else {
        group_digits(bytes, c, sizeof(c));
        snprintf(a, sizeof(a), "%s (%s bytes)", b, c);
    }
    info_row(y, "Size", a);
    y += INFO_ROW;
    if (v->kind == VOL_FREE) {
        info_row(y, "Contents", "Unallocated Space");
        return;
    }
    if (v->fs[0]) {
        snprintf(a, sizeof(a), "%s \xE2\x80\x94 %s%s%s", v->fs_desc, v->mounted_at[0] ? "Mounted at " : "Not Mounted",
                 v->mounted_at, !v->mounted_at[0] ? "" : v->mounted_ro ? " (read-only)" : " (read-write)");
    } else {
        snprintf(a, sizeof(a), "%s", v->extended ? "Extended Partition" : "Unknown (no filesystem this can read)");
    }
    info_row(y, "Contents", a);
    y += INFO_ROW;
    snprintf(a, sizeof(a), _PATH_DEV "/%s", v->dev);
    info_row(y, "Device", a);
    y += INFO_ROW;
    if (v->uuid[0]) {
        info_row(y, "UUID", v->uuid);
        y += INFO_ROW;
    }
    if (v->kind == VOL_PARTITION) {
        info_row(y, "Partition Type", part_type_name(d, v));
        y += INFO_ROW;
        if (v->part_name[0]) {
            info_row(y, "Partition Name", v->part_name);
        }
    }
}

static void draw_dialog(void);

static void draw(void) {
    struct gfx_surface *s = surface();
    gfx_fill_rect(s, gfx_rect(0, 0, s->width, s->height), WHITE);
    draw_sidebar();
    struct disk *d = cur_disk();
    if (d) {
        int x = right_x();
        char title[64], a[160], b[64];
        disk_title(d, title, sizeof(title));
        text((float)x, 44, true, 18, INK, title);
        snprintf(a, sizeof(a), _PATH_DEV "/%s", d->name);
        text((float)x, 66, false, 12, INK_SOFT, a);
        draw_button(BTN_MENU, "dots");
        int y = 106;
        info_row(y, "Model", d->model);
        y += INFO_ROW;
        if (d->serial[0]) {
            info_row(y, "Serial Number", d->serial);
            y += INFO_ROW;
        }
        human_size(disk_bytes(d), b, sizeof(b));
        char digits[40];
        group_digits(disk_bytes(d), digits, sizeof(digits));
        snprintf(a, sizeof(a), "%s (%s bytes)%s", b, digits, d->flags & OS_BLOCK_READ_ONLY ? ", read-only" : "");
        info_row(y, "Size", a);
        y += INFO_ROW;
        info_row(y, "Partitioning", strcmp(d->table, "gpt") == 0 ? "GUID Partition Table"
                                    : strcmp(d->table, "mbr") == 0 ? "Master Boot Record" : "None");
        text((float)x, (float)bar_rect().y - 14, true, 14, INK, "Volumes");
        draw_volume_bar(d);
        struct volume *v = cur_vol();
        if (v) {
            draw_details(d, v);
        }
    } else {
        text((float)right_x(), 60, false, 14, INK_SOFT, "No disk selected");
    }
    if (ui.dlg.kind != DLG_NONE) {
        draw_dialog();
    }
    gui_window_invalidate(ui.w, NULL);
}

/* --- dialogs -------------------------------------------------------------------------------------------- */

static struct field *add_field(enum field_type type, const char *label) {
    struct dialog *g = &ui.dlg;
    struct field *f = &g->fields[g->nfields++];
    memset(f, 0, sizeof(*f));
    f->type = type;
    snprintf(f->label, sizeof(f->label), "%s", label);
    return f;
}

static void dialog_open(enum dlg_kind kind, const char *title, const char *ok, bool danger) {
    struct dialog *g = &ui.dlg;
    memset(g, 0, sizeof(*g));
    g->kind = kind;
    snprintf(g->title, sizeof(g->title), "%s", title);
    g->ok = ok;
    g->danger = danger;
    g->hover = g->pressed = -1;
    g->focus = -1;
    g->sliding = -1;
}

/* --- F_SIZE: a number and a unit, or empty for all of max_bytes ---------------------------------------- */

/* The size typed (bytes); false if the box is empty (= everything) or holds no number. */
static bool size_value(const struct field *f, uint64_t *bytes) {
    uint64_t whole = 0, frac = 0, scale = 1;
    bool any = false, dot = false;
    for (const char *c = f->text; *c; c++) {
        if (*c == '.' && !dot) {
            dot = true;
        } else if (*c >= '0' && *c <= '9') {
            any = true;
            if (!dot) {
                whole = whole * 10 + (uint64_t)(*c - '0');
            } else if (scale < 1000000) {
                frac = frac * 10 + (uint64_t)(*c - '0');
                scale *= 10;
            }
        }
    }
    if (!any) {
        return false;
    }
    uint64_t u = units[f->unit].bytes;
    *bytes = whole * u + frac * u / scale;
    return true;
}

/* What the slider shows: the size typed, or everything when the box is empty. */
static uint64_t size_shown(const struct field *f) {
    uint64_t b;
    if (!size_value(f, &b) || b > f->max_bytes) {
        return f->max_bytes;
    }
    return b;
}

/* Write `bytes` into the box in the field's unit: up to two decimals, trailing zeros dropped. */
static void size_set(struct field *f, uint64_t bytes) {
    uint64_t u = units[f->unit].bytes;
    uint64_t hundredths = bytes / u * 100 + bytes % u * 100 / u;
    uint64_t whole = hundredths / 100, rest = hundredths % 100;
    if (rest == 0) {
        snprintf(f->text, sizeof(f->text), "%lu", (unsigned long)whole);
    } else if (rest % 10 == 0) {
        snprintf(f->text, sizeof(f->text), "%lu.%lu", (unsigned long)whole, (unsigned long)(rest / 10));
    } else {
        snprintf(f->text, sizeof(f->text), "%lu.%02lu", (unsigned long)whole, (unsigned long)rest);
    }
    f->cursor = (int)strlen(f->text);
}

static struct field *add_size_field(const char *label, uint64_t max_bytes) {
    struct field *f = add_field(F_SIZE, label);
    f->max_bytes = max_bytes;
    f->unit = max_bytes >= 10ull * 1000000000 ? 5 : 3;   /* GB for big spaces, else MB */
    return f;
}

static void draw(void);

/* A message in a dialog window of its own; returns when it's been read. */
static void message(enum gui_dialog_icon icon, const char *title, const char *body) {
    draw();   /* what it's about, up to date behind it */
    gui_message(ui.g, ui.w, icon, title, body);
}

static int field_height(const struct field *f) {
    switch (f->type) {
    case F_TEXT:   return 66;
    case F_CHOICE: return 26 + 28 * f->nchoices + 6;
    case F_CHECK:  return 34;
    case F_SIZE:   return 104;
    }
    return 0;
}

static int message_height(void) {
    /* (measured by drawing it off-screen would be exact; lines * line height is close enough) */
    struct dialog *g = &ui.dlg;
    if (!g->message[0]) {
        return 0;
    }
    int lines = 1, width = 0;
    for (const char *c = g->message; *c; c++) {
        width++;
        if (*c == '\n' || width > 58) {
            lines++;
            width = 0;
        }
    }
    return lines * 20 + 10;
}

static struct gfx_rect dialog_rect(void) {
    struct dialog *g = &ui.dlg;
    int h = 70 + message_height() + 60;
    for (int i = 0; i < g->nfields; i++) {
        h += field_height(&g->fields[i]);
    }
    if (g->kind == DLG_BUSY) {
        h = 150;
    }
    int w = 480;
    struct gfx_surface *s = surface();
    return gfx_rect((s->width - w) / 2, (s->height - h) / 2, w, h);
}

static struct gfx_rect dialog_button(int i) {
    struct gfx_rect r = dialog_rect();
    int bw = 96, bh = 34;
    return gfx_rect(r.x + r.w - 22 - bw - i * (bw + 10), r.y + r.h - 20 - bh, bw, bh);
}

static int dialog_buttons(void) {
    if (ui.dlg.kind == DLG_BUSY) {
        return 0;
    }
    return ui.dlg.ok ? 2 : 1;
}

static const char *dialog_button_label(int i) {
    if (ui.dlg.ok) {
        return i == 0 ? ui.dlg.ok : "Cancel";
    }
    return "Close";
}

/* Where field i starts (y), and the rects inside it. */
static int field_y(int i) {
    struct dialog *g = &ui.dlg;
    struct gfx_rect r = dialog_rect();
    int y = r.y + 64 + message_height();
    for (int k = 0; k < i; k++) {
        y += field_height(&g->fields[k]);
    }
    return y;
}

static struct gfx_rect text_box(int i) {
    struct gfx_rect r = dialog_rect();
    return gfx_rect(r.x + 24, field_y(i) + 22, r.w - 48, 36);
}

static struct gfx_rect choice_rect(int i, int c) {
    struct gfx_rect r = dialog_rect();
    return gfx_rect(r.x + 24, field_y(i) + 24 + c * 28, r.w - 48, 26);
}

/* F_SIZE: the number's box, the unit button beside it, and the slider under them. */
#define UNIT_W 92

static struct gfx_rect size_box(int i) {
    struct gfx_rect r = dialog_rect();
    return gfx_rect(r.x + 24, field_y(i) + 22, r.w - 48 - UNIT_W - 8, 36);
}

static struct gfx_rect unit_rect(int i) {
    struct gfx_rect b = size_box(i);
    return gfx_rect(b.x + b.w + 8, b.y, UNIT_W, b.h);
}

static struct gfx_rect slider_rect(int i) {   /* the track; the knob goes 9 px past it on each side */
    struct gfx_rect r = dialog_rect();
    return gfx_rect(r.x + 24 + 9, field_y(i) + 22 + 36 + 22, r.w - 48 - 18, 4);
}

/* Where a click or drag at x puts the slider: everything at the far right, else whole MiBs. */
static void slider_to(struct field *f, struct gfx_rect track, int x) {
    if (x >= track.x + track.w) {
        f->text[0] = '\0';
        f->cursor = 0;
        return;
    }
    uint64_t pos = x <= track.x ? 0 : (uint64_t)(x - track.x);
    uint64_t bytes = f->max_bytes / (uint64_t)track.w * pos + f->max_bytes % (uint64_t)track.w * pos / (uint64_t)track.w;
    bytes = bytes / MIB * MIB;
    size_set(f, bytes < MIB ? MIB : bytes);
}

static void draw_size_field(struct field *f, int i, bool focused) {
    struct gfx_surface *s = surface();
    struct gfx_rect box = size_box(i);
    gfx_fill_rounded_rect(s, box, 8, FIELD);
    if (focused) {
        gfx_stroke_rounded_rect(s, box, 8, 1.5f, ACCENT);
    }
    gfx_set_clip(s, gfx_rect_inset(box, 4));
    if (f->text[0]) {
        text((float)box.x + 12, (float)box.y + 23, false, 13, INK, f->text);
    } else {
        char all[48], hint[96];
        human_size(f->max_bytes, all, sizeof(all));
        snprintf(hint, sizeof(hint), "All available space (%s)", all);
        text((float)box.x + 12, (float)box.y + 23, false, 13, INK_SOFT, hint);
    }
    if (focused) {
        char before[OS_PATH_MAX];
        memcpy(before, f->text, (size_t)f->cursor);
        before[f->cursor] = '\0';
        float cx = box.x + 12 + text_w(false, 13, before);
        gfx_fill_rect(s, gfx_rect((int)cx, box.y + 9, 1, 18), INK);
    }
    gfx_reset_clip(s);

    /* The unit: a button that opens a menu of them. */
    struct gfx_rect u = unit_rect(i);
    gfx_fill_rounded_rect(s, u, 8, FIELD);
    text((float)u.x + 14, (float)u.y + 23, false, 13, INK, units[f->unit].name);
    float ax = u.x + u.w - 20.0f, ay = u.y + 16.0f;
    gfx_draw_line(s, ax, ay, ax + 4, ay + 4, 1.6f, INK_SOFT);
    gfx_draw_line(s, ax + 4, ay + 4, ax + 8, ay, 1.6f, INK_SOFT);

    /* The slider: filled up to the size shown. */
    struct gfx_rect t = slider_rect(i);
    uint64_t shown = size_shown(f);
    int fill = f->max_bytes ? (int)(shown * (uint64_t)t.w / f->max_bytes) : 0;
    gfx_fill_rounded_rect(s, t, 2, LINE);
    gfx_fill_rounded_rect(s, gfx_rect(t.x, t.y, fill, t.h), 2, ACCENT);
    float kx = (float)(t.x + fill), ky = t.y + t.h / 2.0f;
    gfx_fill_circle(s, kx, ky + 1, 10, GFX_RGBA(0, 0, 0, 30));
    gfx_fill_circle(s, kx, ky, 9, WHITE);
    gfx_fill_circle(s, kx, ky, 4, ACCENT);
}

static void draw_dialog(void) {
    struct dialog *g = &ui.dlg;
    struct gfx_surface *s = surface();
    gfx_fill_rect(s, gfx_rect(0, 0, s->width, s->height), GFX_RGBA(0, 0, 0, 70));
    struct gfx_rect r = dialog_rect();
    gfx_fill_rounded_rect(s, gfx_rect(r.x - 1, r.y, r.w + 2, r.h + 3), 15, GFX_RGBA(0, 0, 0, 40));
    gfx_fill_rounded_rect(s, r, 14, WHITE);
    text((float)r.x + 24, (float)r.y + 38, true, 16, INK, g->title);
    if (g->kind == DLG_BUSY) {
        wrapped(r.x + 24, r.y + 74, r.w - 48, 13, INK_SOFT, job_status());
        /* A moving bar: still working. */
        struct gfx_rect track = gfx_rect(r.x + 24, r.y + 104, r.w - 48, 6);
        gfx_fill_rounded_rect(s, track, 3, FIELD);
        int pos = (int)(uptime_ms() / 4 % (uint64_t)(track.w + 80)) - 80;
        int x0 = track.x + (pos < 0 ? 0 : pos), x1 = track.x + pos + 80;
        if (x1 > track.x + track.w) {
            x1 = track.x + track.w;
        }
        if (x1 > x0) {
            gfx_fill_rounded_rect(s, gfx_rect(x0, track.y, x1 - x0, track.h), 3, ACCENT);
        }
        return;
    }
    if (g->message[0]) {
        gfx_set_clip(s, gfx_rect_inset(r, 10));
        wrapped(r.x + 24, r.y + 70, r.w - 48, 13, INK_SOFT, g->message);
        gfx_reset_clip(s);
    }
    for (int i = 0; i < g->nfields; i++) {
        struct field *f = &g->fields[i];
        int y = field_y(i);
        if (f->type != F_CHECK) {
            text((float)r.x + 24, (float)y + 14, true, 12, INK, f->label);
        }
        if (f->type == F_SIZE) {
            draw_size_field(f, i, g->focus == i);
        } else if (f->type == F_TEXT) {
            struct gfx_rect box = text_box(i);
            gfx_fill_rounded_rect(s, box, 8, FIELD);
            if (g->focus == i) {
                gfx_stroke_rounded_rect(s, box, 8, 1.5f, ACCENT);
            }
            gfx_set_clip(s, gfx_rect_inset(box, 4));
            text((float)box.x + 12, (float)box.y + 23, false, 13, INK, f->text);
            if (g->focus == i) {
                char before[OS_PATH_MAX];
                memcpy(before, f->text, (size_t)f->cursor);
                before[f->cursor] = '\0';
                float cx = box.x + 12 + text_w(false, 13, before);
                gfx_fill_rect(s, gfx_rect((int)cx, box.y + 9, 1, 18), INK);
            }
            gfx_reset_clip(s);
        } else if (f->type == F_CHOICE) {
            for (int c = 0; c < f->nchoices; c++) {
                struct gfx_rect cr = choice_rect(i, c);
                float cx = cr.x + 9.0f, cy = cr.y + cr.h / 2.0f;
                gfx_fill_circle(s, cx, cy, 8, f->selected == c ? ACCENT : LINE);
                gfx_fill_circle(s, cx, cy, f->selected == c ? 3.5f : 6.5f, WHITE);
                text((float)cr.x + 26, (float)cr.y + 18, false, 13, INK, f->choices[c]);
            }
        } else {
            struct gfx_rect cr = gfx_rect(r.x + 24, y + 6, r.w - 48, 24);
            gfx_fill_rounded_rect(s, gfx_rect(cr.x, cr.y + 3, 18, 18), 5, f->checked ? ACCENT : LINE);
            if (f->checked) {
                gfx_draw_line(s, cr.x + 4.0f, cr.y + 12.0f, cr.x + 8.0f, cr.y + 16.0f, 2, WHITE);
                gfx_draw_line(s, cr.x + 8.0f, cr.y + 16.0f, cr.x + 14.0f, cr.y + 7.0f, 2, WHITE);
            } else {
                gfx_fill_rounded_rect(s, gfx_rect(cr.x + 2, cr.y + 5, 14, 14), 4, WHITE);
            }
            text((float)cr.x + 28, (float)cr.y + 17, false, 13, INK, f->label);
        }
    }
    for (int i = 0; i < dialog_buttons(); i++) {
        struct gfx_rect b = dialog_button(i);
        bool main = i == 0 && g->ok;
        gfx_color fill = main ? (g->danger ? DANGER : ACCENT) : FIELD;
        if (g->pressed == i) {
            fill = main ? (g->danger ? GFX_RGB(0xC0, 0x30, 0x30) : ACCENT_DARK) : GFX_RGB(0xE2, 0xE3, 0xE8);
        } else if (g->hover == i) {
            fill = main ? (g->danger ? GFX_RGB(0xEE, 0x50, 0x50) : GFX_RGB(0x45, 0x80, 0xF5)) : GFX_RGB(0xEB, 0xEC, 0xF0);
        }
        gfx_fill_rounded_rect(s, b, 8, fill);
        const char *label = dialog_button_label(i);
        float w = text_w(false, 13, label);
        text(b.x + (b.w - w) / 2, (float)b.y + 22, false, 13, main ? WHITE : INK, label);
    }
}

/* --- actions --------------------------------------------------------------------------------------------- */

static const char *const fs_choices[] = { "FAT \xE2\x80\x94 works with every system and device",
                                          "exFAT \xE2\x80\x94 for big files; Windows, macOS, cameras",
                                          "NTFS \xE2\x80\x94 Windows",
                                          "Ext4 \xE2\x80\x94 Linux",
                                          "No filesystem" };

static void dev_path(const char *dev, char *out, size_t size) {
    snprintf(out, size, _PATH_DEV "/%s", dev);
}

/* Add the mkfs step for choice `type` (0 FAT, 1 exFAT, 2 NTFS, 3 Ext4) on device `dev`. */
static void add_mkfs(int type, const char *dev, const char *label, uint64_t bytes) {
    char path[64];
    dev_path(dev, path, sizeof(path));
    char up[16];
    switch (type) {
    case 0:
        snprintf(up, sizeof(up), "%.11s", label);
        for (char *c = up; *c; c++) {
            if (*c >= 'a' && *c <= 'z') {
                *c = (char)(*c - 32);   /* FAT labels are upper case */
            }
        }
        if (bytes >= 512 * MIB) {
            if (up[0]) {
                job_run("mkfs.fat", "-I", "-F", "32", "-n", up, path, NULL);
            } else {
                job_run("mkfs.fat", "-I", "-F", "32", path, NULL);
            }
        } else if (up[0]) {
            job_run("mkfs.fat", "-I", "-n", up, path, NULL);
        } else {
            job_run("mkfs.fat", "-I", path, NULL);
        }
        break;
    case 1:
        if (label[0]) {
            job_run("mkfs.exfat", "-L", label, path, NULL);
        } else {
            job_run("mkfs.exfat", path, NULL);
        }
        break;
    case 2:
        if (label[0]) {
            job_run("mkntfs", "-f", "-F", "-L", label, path, NULL);
        } else {
            job_run("mkntfs", "-f", "-F", path, NULL);
        }
        break;
    case 3:
        if (label[0]) {
            job_run("mke2fs", "-F", "-t", "ext4", "-L", label, path, NULL);
        } else {
            job_run("mke2fs", "-F", "-t", "ext4", path, NULL);
        }
        break;
    }
}

/* The partition type that goes with filesystem choice `type`. */
static void type_for_fs(int type, uint64_t bytes, const char **guid, uint8_t *mbr) {
    switch (type) {
    case 0:  *guid = part_types[0].guid; *mbr = bytes >= 512 * MIB ? 0x0C : 0x0E; break;
    case 1:
    case 2:  *guid = part_types[0].guid; *mbr = 0x07; break;
    default: *guid = part_types[1].guid; *mbr = 0x83; break;
    }
}

static void unmount_all(struct disk *d) {
    for (int i = 0; i < d->nvols; i++) {
        if (d->vols[i].mounted_at[0]) {
            job_step(STEP_UNMOUNT, d->vols[i].mounted_at, NULL);
        }
    }
}

static void run_job(void) {
    job_start();
    dialog_open(DLG_BUSY, job_current()->title, NULL, false);
}

static void do_format_volume(struct disk *d, struct volume *v, int type, const char *label) {
    char title[96];
    snprintf(title, sizeof(title), "Formatting %s", v->dev);
    job_begin(title);
    if (v->mounted_at[0]) {
        job_step(STEP_UNMOUNT, v->mounted_at, NULL);
    }
    add_mkfs(type, v->dev, label, vol_bytes(d, v));
    if (v->kind == VOL_PARTITION && !v->logical) {
        /* The partition type goes with the filesystem (Windows hides FAT in a "Linux" partition). */
        struct job *j = job_current();
        j->disk = ui.disk;
        if (pt_read(d, &j->table) == 0) {
            struct pt_entry *e = pt_find(&j->table, v->number);
            const char *guid;
            uint8_t mbr;
            type_for_fs(type, vol_bytes(d, v), &guid, &mbr);
            if (e) {
                guid_parse(guid, e->type_guid);
                e->mbr_type = mbr;
                job_step(STEP_WRITE_TABLE, NULL, NULL);
                job_step(STEP_RESCAN, d->name, NULL);
            }
        }
        job_step(STEP_MOUNT, v->dev, NULL);
        job_optional();
    } else if (v->kind == VOL_WHOLE) {
        job_step(STEP_RESCAN, d->name, NULL);   /* mounts what's now on it */
    }
    snprintf(ui.keep_dev, sizeof(ui.keep_dev), "%s", v->dev);
    run_job();
}

static void do_format_disk(struct disk *d, int scheme) {
    char title[96];
    snprintf(title, sizeof(title), "Formatting %s", d->name);
    job_begin(title);
    unmount_all(d);
    struct job *j = job_current();
    j->disk = ui.disk;
    job_step(STEP_WIPE, NULL, NULL);
    if (scheme < 2) {
        pt_new(d, &j->table, scheme == 0 ? "gpt" : "mbr");
        job_step(STEP_WRITE_TABLE, NULL, NULL);
    }
    job_step(STEP_RESCAN, d->name, NULL);
    run_job();
}

/* bytes: 0 for all the free space there. */
static void do_create(struct disk *d, struct volume *v, uint64_t bytes, int type, const char *label) {
    struct job *j;
    job_begin("Creating a partition");
    j = job_current();
    j->disk = ui.disk;
    if (pt_read(d, &j->table) != 0) {
        message(GUI_ICON_ERROR, "Can't create a partition", "The partition table couldn't be read.");
        return;
    }
    if (j->table.has_logical) {
        message(GUI_ICON_ERROR, "Can't create a partition",
                "This disk has logical partitions (an MBR extended partition); this app can't change those.");
        return;
    }
    uint64_t align = pt_align(d);
    uint64_t start = (v->start + align - 1) / align * align;
    uint64_t room = pt_free_after(d, &j->table, start, 0);
    uint64_t sectors = bytes / d->sector_size;
    if (!bytes || sectors > room) {
        sectors = room;
    }
    if (sectors >= align) {
        sectors = sectors / align * align;   /* whole MiBs */
    } else if (room >= align) {
        sectors = align;                     /* at least one */
    }
    const char *guid;
    uint8_t mbr;
    type_for_fs(type < 4 ? type : 3, sectors * d->sector_size, &guid, &mbr);
    int number = pt_add(&j->table, start, sectors, guid, mbr, strcmp(j->table.type, "gpt") == 0 ? label : NULL);
    if (number < 0) {
        message(GUI_ICON_ERROR, "Can't create a partition", number == -ENOSPC ? "The partition table is full (an MBR disk holds four)."
                                                              : "There's no room for it there.");
        return;
    }
    char dev[16];
    snprintf(dev, sizeof(dev), "%sp%d", d->name, number);
    job_step(STEP_WRITE_TABLE, NULL, NULL);
    job_step(STEP_RESCAN, d->name, NULL);
    job_step(STEP_WAIT_DEVICE, dev, NULL);
    if (type < 4) {
        add_mkfs(type, dev, label, sectors * d->sector_size);
        job_step(STEP_MOUNT, dev, NULL);
        job_optional();
    }
    snprintf(ui.keep_dev, sizeof(ui.keep_dev), "%s", dev);
    run_job();
}

static void do_delete(struct disk *d, struct volume *v) {
    job_begin("Deleting a partition");
    struct job *j = job_current();
    j->disk = ui.disk;
    if (pt_read(d, &j->table) != 0 || j->table.has_logical || v->logical) {
        message(GUI_ICON_ERROR, "Can't delete the partition", "This app can't change MBR disks with logical partitions.");
        return;
    }
    if (v->mounted_at[0]) {
        job_step(STEP_UNMOUNT, v->mounted_at, NULL);
    }
    pt_remove(&j->table, v->number);
    job_step(STEP_WRITE_TABLE, NULL, NULL);
    job_step(STEP_RESCAN, d->name, NULL);
    run_job();
}

static void do_label(struct volume *v, const char *label) {
    char path[64];
    dev_path(v->dev, path, sizeof(path));
    job_begin("Changing the label");
    bool was_mounted = v->mounted_at[0] != '\0';
    if (was_mounted) {
        job_step(STEP_UNMOUNT, v->mounted_at, NULL);
    }
    if (strcmp(v->fs, "vfat") == 0) {
        char up[16];
        snprintf(up, sizeof(up), "%.11s", label);
        for (char *c = up; *c; c++) {
            if (*c >= 'a' && *c <= 'z') {
                *c = (char)(*c - 32);
            }
        }
        job_run("fatlabel", path, up[0] ? up : "-r", NULL);
    } else if (strcmp(v->fs, "exfat") == 0) {
        job_run("exfatlabel", path, label, NULL);
    } else if (strncmp(v->fs, "ext", 3) == 0) {
        job_run("tune2fs", "-L", label, path, NULL);
    } else if (strcmp(v->fs, "ntfs") == 0) {
        job_run("ntfslabel", path, label, NULL);
    }
    if (was_mounted) {
        job_step(STEP_MOUNT, v->dev, NULL);
        job_optional();
    }
    snprintf(ui.keep_dev, sizeof(ui.keep_dev), "%s", v->dev);
    run_job();
}

static void do_edit_partition(struct disk *d, struct volume *v, int choice, const char *name) {
    job_begin("Changing the partition");
    struct job *j = job_current();
    j->disk = ui.disk;
    if (pt_read(d, &j->table) != 0 || v->logical) {
        message(GUI_ICON_ERROR, "Can't change the partition", "This app can't change logical partitions on MBR disks.");
        return;
    }
    struct pt_entry *e = pt_find(&j->table, v->number);
    if (!e) {
        message(GUI_ICON_ERROR, "Can't change the partition", "It isn't in the partition table any more.");
        return;
    }
    const struct part_type *t = &part_types[ui.dlg.choice_map[choice]];
    if (strcmp(j->table.type, "gpt") == 0) {
        guid_parse(t->guid, e->type_guid);
        snprintf(e->name, sizeof(e->name), "%s", name);
    } else {
        e->mbr_type = t->mbr;
    }
    job_step(STEP_WRITE_TABLE, NULL, NULL);
    job_step(STEP_RESCAN, d->name, NULL);
    snprintf(ui.keep_dev, sizeof(ui.keep_dev), "%s", v->dev);
    run_job();
}

/* NTFS and ext4 are read-only in the kernel; read-write, a program serves them (ntfsd, ext4d). */
static bool can_be_rw(const struct volume *v) {
    return strcmp(v->fs, "ntfs") == 0 || strncmp(v->fs, "ext", 3) == 0;
}

static void do_mount_options(struct volume *v, bool automatic, const char *path, bool rw, bool rw_before) {
    job_begin("Saving the mount options");
    struct job *j = job_current();
    snprintf(j->mount_uuid, sizeof(j->mount_uuid), "%s", v->uuid);
    snprintf(j->mount_path, sizeof(j->mount_path), "%s", path);
    j->mount_auto = automatic;
    j->mount_rw = rw;
    job_step(STEP_SAVE_MOUNT_OPTIONS, NULL, NULL);
    if (v->mounted_at[0] && ((strcmp(v->mounted_at, path) != 0 && path[0]) || rw != rw_before)) {
        job_step(STEP_UNMOUNT, v->mounted_at, NULL);
        job_step(STEP_MOUNT, v->dev, path);
    }
    snprintf(ui.keep_dev, sizeof(ui.keep_dev), "%s", v->dev);
    run_job();
}

/* bytes: 0 for as big as the free space after it allows. */
static void do_resize(struct disk *d, struct volume *v, uint64_t bytes) {
    job_begin("Resizing");
    struct job *j = job_current();
    j->disk = ui.disk;
    if (pt_read(d, &j->table) != 0 || v->logical || j->table.has_logical) {
        message(GUI_ICON_ERROR, "Can't resize", "This app can't change logical partitions on MBR disks.");
        return;
    }
    struct pt_entry *e = pt_find(&j->table, v->number);
    uint64_t align = pt_align(d);
    uint64_t max = pt_free_after(d, &j->table, v->start, v->number);
    uint64_t sectors = (bytes ? bytes / d->sector_size : max) / align * align;
    if (!e || !sectors || sectors > max) {
        message(GUI_ICON_ERROR, "Can't resize", "That size doesn't fit there.");
        return;
    }
    bool grow = sectors > v->sectors;
    char path[64], size_arg[32];
    dev_path(v->dev, path, sizeof(path));
    bool ext = strncmp(v->fs, "ext", 3) == 0, ntfs = strcmp(v->fs, "ntfs") == 0, fat = strcmp(v->fs, "vfat") == 0;
    if (v->mounted_at[0]) {
        job_step(STEP_UNMOUNT, v->mounted_at, NULL);
    }
    if (fat) {
        /* fatresize (GNU parted's libparted) changes the filesystem and the partition table together,
           growing or shrinking (and between FAT16 and FAT32 if the size needs it). The partition ends
           at start + size / sector size, inclusive: one sector less than the size we want. */
        char disk_path[64], num[12];
        dev_path(d->name, disk_path, sizeof(disk_path));
        snprintf(num, sizeof(num), "%d", v->number);
        snprintf(size_arg, sizeof(size_arg), "%lu", (unsigned long)((sectors - 1) * d->sector_size));
        job_run("fsck.fat", "-a", path, NULL);
        job_optional();
        job_run("fatresize", "-f", "-n", num, "-s", size_arg, disk_path, NULL);
        job_step(STEP_RESCAN, d->name, NULL);
        job_step(STEP_WAIT_DEVICE, v->dev, NULL);
        job_step(STEP_MOUNT, v->dev, NULL);
        job_optional();
        snprintf(ui.keep_dev, sizeof(ui.keep_dev), "%s", v->dev);
        run_job();
        return;
    }
    e->sectors = sectors;
    if (!grow) {
        /* Shrink the filesystem first, then the partition. */
        if (ext) {
            snprintf(size_arg, sizeof(size_arg), "%luK", (unsigned long)(sectors * d->sector_size / 1024));
            job_run("e2fsck", "-f", "-y", path, NULL);
            job_optional();
            job_run("resize2fs", path, size_arg, NULL);
        } else if (ntfs) {
            snprintf(size_arg, sizeof(size_arg), "%lu", (unsigned long)(sectors * d->sector_size));
            job_run("ntfsresize", "-f", "-f", "-s", size_arg, path, NULL);
        }
    }
    job_step(STEP_WRITE_TABLE, NULL, NULL);
    job_step(STEP_RESCAN, d->name, NULL);
    job_step(STEP_WAIT_DEVICE, v->dev, NULL);
    if (grow) {
        /* The partition first, then the filesystem fills it. */
        if (ext) {
            job_run("e2fsck", "-f", "-y", path, NULL);
            job_optional();
            job_run("resize2fs", path, NULL);
        } else if (ntfs) {
            job_run("ntfsresize", "-f", "-f", path, NULL);
        }
    }
    if (v->fs[0]) {
        job_step(STEP_MOUNT, v->dev, NULL);
        job_optional();
    }
    snprintf(ui.keep_dev, sizeof(ui.keep_dev), "%s", v->dev);
    run_job();
}

static bool show_output_after;   /* check/repair: show what the tool said, even when it worked */

static void do_check(struct volume *v, bool repair) {
    char path[64];
    dev_path(v->dev, path, sizeof(path));
    job_begin(repair ? "Repairing the filesystem" : "Checking the filesystem");
    bool was_mounted = v->mounted_at[0] != '\0';
    if (was_mounted) {
        job_step(STEP_UNMOUNT, v->mounted_at, NULL);
    }
    if (strcmp(v->fs, "vfat") == 0) {
        job_run("fsck.fat", repair ? "-a" : "-n", path, NULL);
    } else if (strcmp(v->fs, "exfat") == 0) {
        job_run("fsck.exfat", repair ? "-y" : "-n", path, NULL);
    } else if (strncmp(v->fs, "ext", 3) == 0) {
        job_run("e2fsck", "-f", repair ? "-y" : "-n", path, NULL);
    } else if (strcmp(v->fs, "ntfs") == 0) {
        job_run("ntfsck", repair ? "-a" : "-n", path, NULL);
    }
    if (was_mounted) {
        job_step(STEP_MOUNT, v->dev, NULL);
        job_optional();
    }
    show_output_after = true;
    snprintf(ui.keep_dev, sizeof(ui.keep_dev), "%s", v->dev);
    run_job();
}

/* --- opening dialogs ---------------------------------------------------------------------------------------- */

static void open_format_volume(void) {
    struct volume *v = cur_vol();
    dialog_open(DLG_FORMAT_VOL, "Format Volume", "Format", true);
    char what[64];
    human_size(vol_bytes(cur_disk(), v), what, sizeof(what));
    snprintf(ui.dlg.message, sizeof(ui.dlg.message), "Everything on this %s volume (/dev/%s) will be erased.",
             what, v->dev);
    struct field *name = add_field(F_TEXT, "Volume Name");
    snprintf(name->text, sizeof(name->text), "%s", v->label);
    name->cursor = (int)strlen(name->text);
    ui.dlg.focus = 0;
    struct field *type = add_field(F_CHOICE, "Type");
    for (int i = 0; i < 4; i++) {
        type->choices[type->nchoices++] = fs_choices[i];
    }
    type->selected = strcmp(v->fs, "exfat") == 0 ? 1 : strcmp(v->fs, "ntfs") == 0 ? 2 : strncmp(v->fs, "ext", 3) == 0 ? 3 : 0;
}

static void open_format_disk(void) {
    struct disk *d = cur_disk();
    char title[64];
    disk_title(d, title, sizeof(title));
    dialog_open(DLG_FORMAT_DISK, "Format Disk", "Format", true);
    snprintf(ui.dlg.message, sizeof(ui.dlg.message),
             "Everything on %s (%s) will be erased: all partitions and their files.", title, d->model);
    struct field *f = add_field(F_CHOICE, "Partitioning");
    f->choices[f->nchoices++] = "GPT \xE2\x80\x94 modern; disks over 2 TB, UEFI computers";
    f->choices[f->nchoices++] = "MBR / DOS \xE2\x80\x94 older computers and devices";
    f->choices[f->nchoices++] = "None \xE2\x80\x94 empty, no partition table";
}

static void open_create(void) {
    struct disk *d = cur_disk();
    struct volume *v = cur_vol();
    dialog_open(DLG_CREATE, "Create Partition", "Create", false);
    char room[32];
    human_size(vol_bytes(d, v), room, sizeof(room));
    snprintf(ui.dlg.message, sizeof(ui.dlg.message), "Free space here: %s. Leave the size empty to use all of it.",
             room);
    add_size_field("Size", vol_bytes(d, v));
    ui.dlg.focus = 0;
    add_field(F_TEXT, "Volume Name");
    struct field *type = add_field(F_CHOICE, "Type");
    for (int i = 0; i < 5; i++) {
        type->choices[type->nchoices++] = fs_choices[i];
    }
}

static void open_edit_partition(void) {
    struct disk *d = cur_disk();
    struct volume *v = cur_vol();
    dialog_open(DLG_EDIT_PART, "Edit Partition", "Change", false);
    struct field *type = add_field(F_CHOICE, "Type");
    bool gpt = strcmp(d->table, "gpt") == 0;
    for (int i = 0; i < part_type_count && type->nchoices < 12; i++) {
        if (gpt ? part_types[i].guid != NULL : part_types[i].mbr != 0) {
            ui.dlg.choice_map[type->nchoices] = i;
            uint8_t g[16];
            if (gpt && guid_parse(part_types[i].guid, g) && memcmp(g, v->type_guid, 16) == 0) {
                type->selected = type->nchoices;
            }
            if (!gpt && part_types[i].mbr == v->mbr_type) {
                type->selected = type->nchoices;
            }
            type->choices[type->nchoices++] = part_types[i].name;
        }
    }
    if (gpt) {
        struct field *name = add_field(F_TEXT, "Name");
        snprintf(name->text, sizeof(name->text), "%s", v->part_name);
        name->cursor = (int)strlen(name->text);
        ui.dlg.focus = 1;
    }
}

static void open_label(void) {
    struct volume *v = cur_vol();
    if (!v->fs[0] || strcmp(v->fs, "swap") == 0) {
        message(GUI_ICON_WARNING, "Edit Filesystem", "There's no filesystem here to name.");
        return;
    }
    dialog_open(DLG_LABEL, "Edit Filesystem", "Change", false);
    snprintf(ui.dlg.message, sizeof(ui.dlg.message), "%s",
             strcmp(v->fs, "vfat") == 0 ? "FAT labels are up to 11 characters, in capitals."
             : strncmp(v->fs, "ext", 3) == 0 ? "Up to 16 characters." : "Up to 11 characters (exFAT) or 32 (NTFS).");
    struct field *f = add_field(F_TEXT, "Label");
    snprintf(f->text, sizeof(f->text), "%s", v->label);
    f->cursor = (int)strlen(f->text);
    ui.dlg.focus = 0;
}

static void open_mount_options(void) {
    struct volume *v = cur_vol();
    if (!v->uuid[0]) {
        message(GUI_ICON_WARNING, "Edit Mount Options", "This volume has no filesystem with a UUID to remember it by.");
        return;
    }
    dialog_open(DLG_MOUNT_OPTS, "Edit Mount Options", "Save", false);
    char path[OS_PATH_MAX] = "";
    bool automatic = true, rw = false;
    if (!mount_options_get(v->uuid, path, sizeof(path), &automatic, &rw)) {
        if (v->mounted_at[0]) {
            snprintf(path, sizeof(path), "%s", v->mounted_at);
        } else {
            snprintf(path, sizeof(path), _PATH_MEDIA "/%s", v->label[0] ? v->label : v->dev);
        }
    }
    snprintf(ui.dlg.message, sizeof(ui.dlg.message), "For the filesystem with UUID %s, wherever it's plugged in.", v->uuid);
    struct field *check = add_field(F_CHECK, "Mount automatically when the drive is plugged in");
    check->checked = automatic;
    struct field *where = add_field(F_TEXT, "Mount Point");
    snprintf(where->text, sizeof(where->text), "%s", path);
    where->cursor = (int)strlen(where->text);
    ui.dlg.focus = 1;
    if (can_be_rw(v)) {
        struct field *write = add_field(F_CHECK, "Allow writing (otherwise it's read-only)");
        write->checked = rw;
        ui.dlg.was_checked = rw;
    }
}

/* Before turning writing on: say what can go wrong, and have it confirmed. */
static bool confirm_rw(const struct volume *v) {
    bool ntfs = strcmp(v->fs, "ntfs") == 0;
    struct gui_dialog dlg = {
        .icon = GUI_ICON_WARNING,
        .title = ntfs ? "Allow writing to this NTFS volume?" : "Allow writing to this Ext4 volume?",
        .message = ntfs
            ? "Writing is done by ntfs-3g's library, run as a program. If Windows is hibernated or was shut down "
              "with Fast Startup, writing here can destroy files: shut Windows down fully first (Shift + Shut "
              "down), or turn Fast Startup off.\n\nAlways eject the drive before unplugging it."
            : "Writing is done by e2fsprogs' library (as fuse2fs does), run as a program. It doesn't use the "
              "journal: if the computer crashes or the drive is pulled out while it's mounted, files can be "
              "damaged. The volume is marked to be checked after an unclean stop.\n\nAlways eject the drive "
              "before unplugging it.",
        .buttons = { "Allow Writing", "Cancel" },
        .danger = true,
        .check = "I understand the risk",
        .require_check = true,
    };
    draw();
    return gui_dialog_run(ui.g, ui.w, &dlg) == 0;
}

static void open_resize(void) {
    struct disk *d = cur_disk();
    struct volume *v = cur_vol();
    if (v->kind != VOL_PARTITION) {
        message(GUI_ICON_WARNING, "Resize", "Only partitions can be resized.");
        return;
    }
    if (v->fs[0] && strncmp(v->fs, "ext", 3) != 0 && strcmp(v->fs, "ntfs") != 0 && strcmp(v->fs, "vfat") != 0) {
        message(GUI_ICON_WARNING, "Resize", "Only FAT, Ext2/3/4 and NTFS filesystems can be resized (there's no resizing "
                "tool for exFAT).");
        return;
    }
    struct ptable t;
    if (pt_read(d, &t) != 0) {
        message(GUI_ICON_WARNING, "Resize", "The partition table couldn't be read.");
        return;
    }
    uint64_t max = pt_free_after(d, &t, v->start, v->number) * d->sector_size;
    dialog_open(DLG_RESIZE, "Resize Volume", "Resize", false);
    char now[32], most[32];
    human_size(vol_bytes(d, v), now, sizeof(now));
    human_size(max, most, sizeof(most));
    snprintf(ui.dlg.message, sizeof(ui.dlg.message),
             "Now %s; it can grow to %s (leave the size empty for that). Shrinking can't go below what the "
             "files need.", now, most);
    struct field *f = add_size_field("New Size", max);
    size_set(f, vol_bytes(d, v));
    ui.dlg.focus = 0;
}

static void open_delete(void) {
    struct volume *v = cur_vol();
    char text[200];
    snprintf(text, sizeof(text), "Partition %d%s%s%s and everything on it will be deleted. This can't be undone.",
             v->number, v->label[0] ? " (" : "", v->label, v->label[0] ? ")" : "");
    draw();
    if (gui_confirm(ui.g, ui.w, GUI_ICON_WARNING, "Delete the partition?", text, "Delete", true)) {
        do_delete(cur_disk(), v);
    }
}

static void dialog_accept(void) {
    struct dialog *g = &ui.dlg;
    struct disk *d = cur_disk();
    struct volume *v = cur_vol();
    enum dlg_kind kind = g->kind;
    g->kind = DLG_NONE;
    if (!d || (!v && kind != DLG_FORMAT_DISK)) {
        return;
    }
    switch (kind) {
    case DLG_FORMAT_VOL: do_format_volume(d, v, g->fields[1].selected, g->fields[0].text); break;
    case DLG_FORMAT_DISK: do_format_disk(d, g->fields[0].selected); break;
    case DLG_CREATE: {
        uint64_t bytes = 0;   /* 0: all of it */
        if (g->fields[0].text[0] && !size_value(&g->fields[0], &bytes)) {
            message(GUI_ICON_WARNING, "Create Partition", "The size isn't a number.");
            return;
        }
        do_create(d, v, bytes, g->fields[2].selected, g->fields[1].text);
        break;
    }
    case DLG_EDIT_PART: do_edit_partition(d, v, g->fields[0].selected, g->nfields > 1 ? g->fields[1].text : ""); break;
    case DLG_LABEL: do_label(v, g->fields[0].text); break;
    case DLG_MOUNT_OPTS: {
        if (g->fields[1].text[0] != '/') {
            message(GUI_ICON_WARNING, "Edit Mount Options", "The mount point must be a full path, like /media/Photos.");
            return;
        }
        bool rw = g->nfields > 2 && g->fields[2].checked;
        if (rw && !g->was_checked && !confirm_rw(v)) {
            return;
        }
        do_mount_options(v, g->fields[0].checked, g->fields[1].text, rw, g->was_checked);
        break;
    }
    case DLG_RESIZE: {
        uint64_t bytes = 0;   /* 0: as big as it can get */
        if (g->fields[0].text[0] && !size_value(&g->fields[0], &bytes)) {
            message(GUI_ICON_WARNING, "Resize", "The size isn't a number.");
            return;
        }
        do_resize(d, v, bytes);
        break;
    }
    default: break;
    }
}

/* --- input ------------------------------------------------------------------------------------------------- */

static void dialog_key(const struct gui_event *ev) {
    struct dialog *g = &ui.dlg;
    if (g->kind == DLG_BUSY) {
        return;
    }
    if (ev->key == OS_KEY_ESCAPE) {
        g->kind = DLG_NONE;
        return;
    }
    if (ev->key == OS_KEY_ENTER) {
        if (g->ok) {
            dialog_accept();
        } else {
            g->kind = DLG_NONE;
        }
        return;
    }
    if (ev->key == OS_KEY_TAB) {
        for (int k = 1; k <= g->nfields; k++) {
            int i = (g->focus + k) % g->nfields;
            if (g->fields[i].type == F_TEXT || g->fields[i].type == F_SIZE) {
                g->focus = i;
                break;
            }
        }
        return;
    }
    if (g->focus < 0) {
        return;
    }
    struct field *f = &g->fields[g->focus];
    int len = (int)strlen(f->text);
    switch (ev->key) {
    case OS_KEY_LEFT:  f->cursor -= f->cursor > 0; return;
    case OS_KEY_RIGHT: f->cursor += f->cursor < len; return;
    case OS_KEY_HOME:  f->cursor = 0; return;
    case OS_KEY_END:   f->cursor = len; return;
    case OS_KEY_BACKSPACE:
        if (f->cursor > 0) {
            memmove(f->text + f->cursor - 1, f->text + f->cursor, (size_t)(len - f->cursor + 1));
            f->cursor--;
        }
        return;
    case OS_KEY_DELETE:
        if (f->cursor < len) {
            memmove(f->text + f->cursor, f->text + f->cursor + 1, (size_t)(len - f->cursor));
        }
        return;
    }
    bool ok = ev->ch >= ' ' && ev->ch < 0x7F && !(ev->modifiers & OS_MOD_CTRL);
    if (f->digits && !(ev->ch >= '0' && ev->ch <= '9')) {
        ok = false;
    }
    if (f->type == F_SIZE && !((ev->ch >= '0' && ev->ch <= '9') || (ev->ch == '.' && !strchr(f->text, '.')))) {
        ok = false;   /* a number, maybe with a decimal point */
    }
    if (ok && len < (int)sizeof(f->text) - 1 && len < 200) {
        memmove(f->text + f->cursor + 1, f->text + f->cursor, (size_t)(len - f->cursor + 1));
        f->text[f->cursor++] = (char)ev->ch;
    }
}

static int dialog_button_at(int x, int y) {
    for (int i = 0; i < dialog_buttons(); i++) {
        if (gfx_rect_contains(dialog_button(i), x, y)) {
            return i;
        }
    }
    return -1;
}

static void dialog_mouse(const struct gui_event *ev) {
    struct dialog *g = &ui.dlg;
    if (g->kind == DLG_BUSY) {
        return;
    }
    int b = dialog_button_at(ev->x, ev->y);
    if (ev->type == GUI_EVENT_MOUSE_MOVE) {
        g->hover = b;
        if (g->sliding >= 0 && (ev->buttons & OS_MOUSE_LEFT)) {
            slider_to(&g->fields[g->sliding], slider_rect(g->sliding), ev->x);
        }
        return;
    }
    if (ev->type == GUI_EVENT_MOUSE_DOWN && ev->button == OS_MOUSE_LEFT) {
        g->pressed = b;
        for (int i = 0; i < g->nfields; i++) {
            struct field *f = &g->fields[i];
            if (f->type == F_SIZE) {
                struct gfx_rect knob_zone = gfx_rect_inset(slider_rect(i), -12);
                if (gfx_rect_contains(size_box(i), ev->x, ev->y)) {
                    g->focus = i;
                    f->cursor = (int)strlen(f->text);
                } else if (gfx_rect_contains(knob_zone, ev->x, ev->y)) {
                    g->sliding = i;
                    g->focus = i;
                    slider_to(f, slider_rect(i), ev->x);
                } else if (gfx_rect_contains(unit_rect(i), ev->x, ev->y)) {
                    /* The units, in a menu just under the button. */
                    struct gui_menu_item items[UNIT_COUNT];
                    for (int u = 0; u < UNIT_COUNT; u++) {
                        items[u] = (struct gui_menu_item){ units[u].name, MENU_UNIT + i * 16 + u, false, u == f->unit };
                    }
                    struct gfx_rect ur = unit_rect(i);
                    gui_show_menu(ui.g, ev->screen_x - ev->x + ur.x, ev->screen_y - ev->y + ur.y + ur.h + 4, items,
                                  UNIT_COUNT, ui.w);
                }
                continue;
            }
            if (f->type == F_TEXT && gfx_rect_contains(text_box(i), ev->x, ev->y)) {
                g->focus = i;
                f->cursor = (int)strlen(f->text);
            } else if (f->type == F_CHOICE) {
                for (int c = 0; c < f->nchoices; c++) {
                    if (gfx_rect_contains(choice_rect(i, c), ev->x, ev->y)) {
                        f->selected = c;
                    }
                }
            } else if (f->type == F_CHECK) {
                struct gfx_rect r = dialog_rect();
                if (gfx_rect_contains(gfx_rect(r.x + 24, field_y(i) + 6, r.w - 48, 24), ev->x, ev->y)) {
                    f->checked = !f->checked;
                }
            }
        }
        return;
    }
    if (ev->type == GUI_EVENT_MOUSE_UP && ev->button == OS_MOUSE_LEFT) {
        int pressed = g->pressed;
        g->pressed = -1;
        g->sliding = -1;
        if (pressed >= 0 && pressed == b) {
            if (b == 0 && g->ok) {
                dialog_accept();
            } else {
                g->kind = DLG_NONE;
            }
        }
    }
}

/* A unit chosen from an F_SIZE field's menu: the size stays the same, written in the new unit. */
static void dialog_menu(int id) {
    struct dialog *g = &ui.dlg;
    int i = (id - MENU_UNIT) / 16, u = (id - MENU_UNIT) % 16;
    if (id < MENU_UNIT || i >= g->nfields || g->fields[i].type != F_SIZE || u >= UNIT_COUNT) {
        return;
    }
    struct field *f = &g->fields[i];
    uint64_t bytes;
    bool typed = size_value(f, &bytes);
    f->unit = u;
    if (typed) {
        size_set(f, bytes);
    }
}

static int button_at(int x, int y) {
    for (int b = 0; b < BTN_COUNT; b++) {
        if (button_shown(b) && gfx_rect_contains(button_rect(b), x, y)) {
            return b;
        }
    }
    return BTN_NONE;
}

static int disk_at(int x, int y) {
    for (int i = 0; i < disk_count; i++) {
        if (gfx_rect_contains(gfx_rect(8, 54 + i * ROW_H, SIDEBAR_W - 16, ROW_H - 4), x, y)) {
            return i;
        }
    }
    return -1;
}

static int volume_at(int x, int y) {
    struct disk *d = cur_disk();
    if (!d) {
        return -1;
    }
    int vis[MAX_VOLUMES], n = visible(d, vis);
    for (int k = 0; k < n; k++) {
        if (gfx_rect_contains(segment_rect(k, n), x, y)) {
            return vis[k];
        }
    }
    return -1;
}

static void show_gear_menu(const struct gui_event *ev) {
    struct volume *v = cur_vol();
    bool part = v->kind == VOL_PARTITION, fs = v->fs[0] && strcmp(v->fs, "swap") != 0;
    bool resizable = part && (!v->fs[0] || strncmp(v->fs, "ext", 3) == 0 || strcmp(v->fs, "ntfs") == 0 ||
                                strcmp(v->fs, "vfat") == 0);
    struct gui_menu_item items[] = {
        { "Format Volume...", MENU_FORMAT_VOL, v->extended, false },
        { "Edit Partition...", MENU_EDIT_PART, !part || v->logical, false },
        { "Edit Filesystem...", MENU_EDIT_FS, !fs, false },
        { "Edit Mount Options...", MENU_MOUNT_OPTS, !fs, false },
        { NULL, 0, false, false },
        { "Resize...", MENU_RESIZE, !resizable, false },
        { "Check Filesystem", MENU_CHECK, !fs, false },
        { "Repair Filesystem", MENU_REPAIR, !fs, false },
    };
    struct gfx_rect r = button_rect(BTN_GEAR);
    gui_show_menu(ui.g, ev->screen_x - (ev->x - r.x), ev->screen_y - (ev->y - r.y) + r.h + 4, items,
                  (int)(sizeof(items) / sizeof(items[0])), ui.w);
}

static void show_disk_menu(const struct gui_event *ev) {
    struct disk *d = cur_disk();
    bool mounted = false;
    for (int i = 0; i < d->nvols; i++) {
        mounted |= d->vols[i].mounted_at[0] != '\0';
    }
    struct gui_menu_item items[] = {
        { "Format Disk...", MENU_FORMAT_DISK, false, false },
        { "Eject (Unmount Everything)", MENU_EJECT, !mounted, false },
        { NULL, 0, false, false },
        { "Refresh", MENU_REFRESH, false, false },
    };
    struct gfx_rect r = button_rect(BTN_MENU);
    gui_show_menu(ui.g, ev->screen_x - (ev->x - r.x) - 200 + r.w, ev->screen_y - (ev->y - r.y) + r.h + 4, items,
                  (int)(sizeof(items) / sizeof(items[0])), ui.w);
}

static void toggle_mount(void) {
    struct volume *v = cur_vol();
    int err = v->mounted_at[0] ? unmount(v->mounted_at) : mount_device(v->dev, NULL);
    if (err < 0) {
        message(GUI_ICON_ERROR, v->mounted_at[0] ? "Can't unmount" : "Can't mount",
                err == -EBUSY ? "Something is using it." : strerror(err));
    }
    snprintf(ui.keep_dev, sizeof(ui.keep_dev), "%s", v->dev);
    model_forget();
    refresh();
}

static void on_click(const struct gui_event *ev) {
    int b = button_at(ev->x, ev->y);
    if (b == BTN_MOUNT) {
        toggle_mount();
    } else if (b == BTN_GEAR) {
        show_gear_menu(ev);
    } else if (b == BTN_MENU) {
        show_disk_menu(ev);
    } else if (b == BTN_ADD) {
        open_create();
    } else if (b == BTN_DELETE) {
        open_delete();
    }
}

static void on_menu(int id) {
    struct disk *d = cur_disk();
    struct volume *v = cur_vol();
    if (!d) {
        return;
    }
    switch (id) {
    case MENU_FORMAT_VOL:  open_format_volume(); break;
    case MENU_EDIT_PART:   open_edit_partition(); break;
    case MENU_EDIT_FS:     open_label(); break;
    case MENU_MOUNT_OPTS:  open_mount_options(); break;
    case MENU_RESIZE:      open_resize(); break;
    case MENU_CHECK:       do_check(v, false); break;
    case MENU_REPAIR:      do_check(v, true); break;
    case MENU_FORMAT_DISK: open_format_disk(); break;
    case MENU_REFRESH:     model_forget(); refresh(); break;
    case MENU_EJECT:
        for (int i = 0; i < d->nvols; i++) {
            if (d->vols[i].mounted_at[0]) {
                int err = eject(d->vols[i].mounted_at);
                if (err < 0) {
                    message(GUI_ICON_ERROR, "Can't eject", strerror(err));
                }
                break;
            }
        }
        refresh();
        break;
    }
}

int main(void) {
    ui.g = gui_open();
    if (!ui.g) {
        return 1;
    }
    ui.regular = gui_font(ui.g, GUI_FONT_REGULAR);
    ui.bold = gui_font(ui.g, GUI_FONT_BOLD);
    ui.w = gui_window_create(ui.g, "Disks", -1, -1, WIN_W, WIN_H);
    if (!ui.w || !ui.regular || !ui.bold) {
        return 1;
    }
    ui.hover_disk = ui.hover_vol = ui.hover_btn = ui.pressed_btn = -1;
    ui.disk = ui.vol = -1;
    load_drive_icons();
    refresh();
    draw();

    struct gui_event ev;
    for (;;) {
        int timeout = job_busy() ? 100 : 2000;
        if (!gui_wait_event(ui.g, &ev, timeout)) {
            if (job_busy()) {
                bool ok;
                char msg[1200];
                if (job_poll(&ok, msg, sizeof(msg))) {
                    ui.dlg.kind = DLG_NONE;
                    model_forget();
                    refresh();
                    if (!ok) {
                        if (strstr(msg, "fatresize") && strstr(msg, "cannot resize this partition to this size")) {
                            /* libparted keeps a FAT's cluster size: FAT16 can only grow so far. */
                            size_t n = strlen(msg);
                            snprintf(msg + n, sizeof(msg) - n, "\n\nA FAT16 volume can't grow past the size its "
                                     "cluster size allows. To make it bigger, copy the files off, format it as FAT "
                                     "at the new size (or exFAT), and copy them back.");
                        }
                        message(GUI_ICON_ERROR, "That didn't work", msg);
                    } else if (show_output_after) {
                        message(GUI_ICON_INFO, "Done", "The filesystem looks fine.");
                    }
                    show_output_after = false;
                }
            } else if (ui.dlg.kind == DLG_NONE && uptime_ms() - ui.last_refresh >= 3000) {
                refresh();   /* drives come and go */
            }
            draw();
            continue;
        }
        if (ev.type == GUI_EVENT_CLOSE || ev.type == GUI_EVENT_QUIT) {
            if (job_busy()) {
                continue;   /* don't leave a disk half-formatted */
            }
            break;
        }
        if (ev.type == GUI_EVENT_RESIZE) {
            draw();
            continue;
        }
        if (ui.dlg.kind != DLG_NONE) {
            if (ev.type == GUI_EVENT_KEY) {
                dialog_key(&ev);
            } else if (ev.type == GUI_EVENT_MOUSE_DOWN || ev.type == GUI_EVENT_MOUSE_UP || ev.type == GUI_EVENT_MOUSE_MOVE) {
                dialog_mouse(&ev);
            } else if (ev.type == GUI_EVENT_MENU) {
                dialog_menu(ev.menu_id);
            }
            draw();
            continue;
        }
        switch (ev.type) {
        case GUI_EVENT_MOUSE_MOVE: {
            int hd = disk_at(ev.x, ev.y), hv = volume_at(ev.x, ev.y), hb = button_at(ev.x, ev.y);
            gui_window_set_cursor(ui.w, hd >= 0 || hv >= 0 || hb >= 0 ? GUI_CURSOR_HAND : GUI_CURSOR_ARROW);
            if (hd == ui.hover_disk && hv == ui.hover_vol && hb == ui.hover_btn) {
                continue;
            }
            ui.hover_disk = hd, ui.hover_vol = hv, ui.hover_btn = hb;
            break;
        }
        case GUI_EVENT_MOUSE_DOWN: {
            if (ev.button != OS_MOUSE_LEFT) {
                break;
            }
            int i = disk_at(ev.x, ev.y);
            if (i >= 0 && i != ui.disk) {
                ui.disk = i;
                ui.vol = -1;
                int vis[MAX_VOLUMES];
                if (visible(&disks[i], vis)) {
                    ui.vol = vis[0];
                }
            }
            int v = volume_at(ev.x, ev.y);
            if (v >= 0) {
                ui.vol = v;
            }
            ui.pressed_btn = button_at(ev.x, ev.y);
            break;
        }
        case GUI_EVENT_MOUSE_UP:
            if (ev.button == OS_MOUSE_LEFT && ui.pressed_btn >= 0 && ui.pressed_btn == button_at(ev.x, ev.y)) {
                ui.pressed_btn = -1;
                on_click(&ev);
            }
            ui.pressed_btn = -1;
            break;
        case GUI_EVENT_MENU:
            on_menu(ev.menu_id);
            break;
        default:
            break;
        }
        draw();
    }
    return 0;
}
