/*
 * Dialogs: cards drawn over the editor that take all input until closed.
 *
 *   Open / Save as   a folder browser with a file name box
 *   Go to line       a number box
 *   Save changes?    before throwing away unsaved work
 *   Replace file?    before Save as overwrites something
 *   messages         errors
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "editor.h"

#define ROW_H        28
#define DOUBLE_CLICK 450
#define MAX_ENTRIES  512

static struct dialog *dlg = &ed.dialog;

/* --- the folder list ---------------------------------------------------------------------- */

static int lower(int c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int compare(const void *a, const void *b) {
    const struct dialog_entry *x = a, *y = b;
    if (x->folder != y->folder) {
        return x->folder ? -1 : 1;
    }
    const char *p = x->name, *q = y->name;
    while (*p && lower(*p) == lower(*q)) {
        p++;
        q++;
    }
    return lower(*p) - lower(*q);
}

static void join(char *out, size_t size, const char *dir, const char *name) {
    snprintf(out, size, strcmp(dir, "/") == 0 ? "%s%s" : "%s/%s", dir, name);
}

static void load_dir(const char *dir) {
    int fd = open(dir, O_RDONLY);
    if (fd < 0) {
        return;   /* stay where we are */
    }
    if (dir != dlg->dir) {
        strlcpy(dlg->dir, dir, sizeof(dlg->dir));
    }
    if (!dlg->entries) {
        dlg->entries = malloc(sizeof(*dlg->entries) * MAX_ENTRIES);
    }
    dlg->count = 0;
    struct os_file_info info;
    for (uint64_t i = 0; dlg->entries && dlg->count < MAX_ENTRIES && readdir(fd, i, &info) == 0; i++) {
        strlcpy(dlg->entries[dlg->count].name, info.name, sizeof(dlg->entries[0].name));
        dlg->entries[dlg->count].folder = info.type == OS_DIRECTORY;
        dlg->count++;
    }
    close(fd);
    if (dlg->entries) {
        qsort(dlg->entries, (size_t)dlg->count, sizeof(dlg->entries[0]), compare);
    }
    dlg->selected = -1;
    dlg->scroll = 0;
}

static void go_up(void) {
    if (strcmp(dlg->dir, "/") == 0) {
        return;
    }
    char parent[PATH_MAX];
    strlcpy(parent, dlg->dir, sizeof(parent));
    char *slash = strrchr(parent, '/');
    if (slash == parent) {
        parent[1] = '\0';
    } else if (slash) {
        *slash = '\0';
    }
    load_dir(parent);
}

/* --- opening and closing --------------------------------------------------------------------- */

static void reset(enum dialog_kind kind, const char *title) {
    struct dialog_entry *entries = dlg->entries;   /* kept between dialogs */
    memset(dlg, 0, sizeof(*dlg));
    dlg->entries = entries;
    dlg->kind = kind;
    strlcpy(dlg->title, title, sizeof(dlg->title));
    dlg->hover_button = dlg->pressed_button = -1;
    dlg->selected = dlg->last_click_index = -1;
}

void dialog_close(void) {
    dlg->kind = DIALOG_NONE;
}

void dialog_open_file(enum dialog_kind kind) {
    reset(kind, kind == DIALOG_OPEN ? "Open" : "Save as");
    /* Start where the current file is, or in the home folder. */
    char dir[PATH_MAX];
    strlcpy(dir, home_dir(), sizeof(dir));
    if (ed.path[0]) {
        strlcpy(dir, ed.path, sizeof(dir));
        char *slash = strrchr(dir, '/');
        if (slash == dir) {
            dir[1] = '\0';
        } else if (slash) {
            *slash = '\0';
        }
    }
    load_dir(dir);
    if (!dlg->count && strcmp(dlg->dir, dir) != 0) {
        load_dir("/");
    }
    if (kind == DIALOG_SAVE_AS) {
        const char *name = strrchr(ed.path, '/');
        field_set(&dlg->field, name ? name + 1 : "Untitled.txt");
    }
}

void dialog_goto(void) {
    reset(DIALOG_GOTO, "Go to line");
    char text[32];
    snprintf(text, sizeof(text), "%d", ed.cursor.line + 1);
    field_set(&dlg->field, text);
    snprintf(dlg->message, sizeof(dlg->message), "Line number (1 - %d)", ed.count);
}

void dialog_save_changes(enum pending then) {
    reset(DIALOG_SAVE_CHANGES, "Save changes?");
    const char *name = strrchr(ed.path, '/');
    snprintf(dlg->message, sizeof(dlg->message), "Do you want to save the changes to %s?",
             name ? name + 1 : "Untitled");
    ed.pending = then;
}

void dialog_message(const char *title, const char *message) {
    reset(DIALOG_MESSAGE, title);
    strlcpy(dlg->message, message, sizeof(dlg->message));
}

/* --- layout --------------------------------------------------------------------------------------- */

static int buttons(const char **labels) {
    switch (dlg->kind) {
    case DIALOG_OPEN:         labels[0] = "Open"; labels[1] = "Cancel"; return 2;
    case DIALOG_SAVE_AS:      labels[0] = "Save"; labels[1] = "Cancel"; return 2;
    case DIALOG_GOTO:         labels[0] = "Go"; labels[1] = "Cancel"; return 2;
    case DIALOG_REPLACE_FILE: labels[0] = "Replace"; labels[1] = "Cancel"; return 2;
    case DIALOG_SAVE_CHANGES:
        labels[0] = "Save";
        labels[1] = "Don't save";
        labels[2] = "Cancel";
        return 3;
    default:                  labels[0] = "OK"; return 1;
    }
}

static bool is_file_dialog(void) {
    return dlg->kind == DIALOG_OPEN || dlg->kind == DIALOG_SAVE_AS;
}

static struct gfx_rect card(void) {
    struct gfx_surface *s = gui_window_surface(ed.w);
    int w, h;
    switch (dlg->kind) {
    case DIALOG_OPEN:
    case DIALOG_SAVE_AS: w = 560; h = 470; break;
    case DIALOG_GOTO:    w = 340; h = 180; break;
    default:             w = 440; h = 150; break;
    }
    w = w > s->width - 20 ? s->width - 20 : w;
    h = h > s->height - 20 ? s->height - 20 : h;
    return gfx_rect((s->width - w) / 2, (s->height - h) / 2, w, h);
}

static struct gfx_rect button_rect(int i) {
    struct gfx_rect r = card();
    int bw = 96, bh = 32;
    return gfx_rect(r.x + r.w - 20 - bw - i * (bw + 10), r.y + r.h - 20 - bh, bw, bh);
}

static struct gfx_rect up_rect(void) {
    struct gfx_rect r = card();
    return gfx_rect(r.x + 20, r.y + 52, 32, 32);
}

static struct gfx_rect list_rect(void) {
    struct gfx_rect r = card();
    return gfx_rect(r.x + 20, r.y + 96, r.w - 40, r.h - 96 - 118);
}

static struct gfx_rect field_rect(void) {
    struct gfx_rect r = card();
    if (is_file_dialog()) {
        return gfx_rect(r.x + 100, r.y + r.h - 108, r.w - 120, 34);
    }
    return gfx_rect(r.x + 22, r.y + 74, r.w - 44, 34);
}

/* --- drawing --------------------------------------------------------------------------------------- */

static void draw_folder_icon(struct gfx_surface *s, int x, int y, bool folder) {
    if (folder) {
        gfx_fill_rounded_rect(s, gfx_rect(x, y + 1, 8, 4), 1.5f, GFX_RGB(0x4E, 0x9B, 0xF5));
        gfx_fill_rounded_rect(s, gfx_rect(x, y + 3, 16, 11), 2, GFX_RGB(0x6C, 0xB0, 0xFA));
    } else {
        gfx_fill_rect(s, gfx_rect(x + 2, y, 12, 15), GFX_RGB(0xE8, 0xE9, 0xEE));
        gfx_fill_rect(s, gfx_rect(x + 4, y + 4, 8, 1), INK_FAINT);
        gfx_fill_rect(s, gfx_rect(x + 4, y + 7, 8, 1), INK_FAINT);
        gfx_fill_rect(s, gfx_rect(x + 4, y + 10, 6, 1), INK_FAINT);
    }
}

static void draw_files(struct gfx_surface *s) {
    /* Up button and the folder's path. */
    struct gfx_rect up = up_rect();
    gfx_fill_rounded_rect(s, up, 8, dlg->hover_button == 10 ? GFX_RGB(0xE6, 0xE7, 0xEC) : FIELD);
    float cx = up.x + 16, cy = up.y + 16;
    gfx_color ink = strcmp(dlg->dir, "/") ? INK : INK_FAINT;
    gfx_draw_line(s, cx, cy - 6, cx, cy + 6, 1.8f, ink);
    gfx_draw_line(s, cx, cy - 6, cx - 5, cy - 1, 1.8f, ink);
    gfx_draw_line(s, cx, cy - 6, cx + 5, cy - 1, 1.8f, ink);
    struct gfx_rect r = card();
    struct gfx_rect path = gfx_rect(up.x + 42, up.y, r.x + r.w - 20 - up.x - 42, 32);
    gfx_fill_rounded_rect(s, path, 8, FIELD);
    gfx_set_clip(s, gfx_rect_inset(path, 4));
    ttf_draw_text(s, ed.ui, UI_SIZE, path.x + 10, path.y + 21, INK, dlg->dir);
    gfx_reset_clip(s);

    /* The list. */
    struct gfx_rect list = list_rect();
    gfx_stroke_rounded_rect(s, list, 8, 1, LINE_COLOR);
    struct gfx_rect inner = gfx_rect_inset(list, 3);
    gfx_set_clip(s, inner);
    if (!dlg->count) {
        ttf_draw_text(s, ed.ui, UI_SIZE, inner.x + 12, inner.y + 24, INK_SOFT, "This folder is empty");
    }
    for (int i = 0; i < dlg->count; i++) {
        int y = inner.y + i * ROW_H - dlg->scroll;
        if (y + ROW_H < inner.y || y > inner.y + inner.h) {
            continue;
        }
        if (i == dlg->selected) {
            gfx_fill_rounded_rect(s, gfx_rect(inner.x + 2, y + 1, inner.w - 4, ROW_H - 2), 6, SELECTION);
        }
        draw_folder_icon(s, inner.x + 10, y + 7, dlg->entries[i].folder);
        ttf_draw_text(s, ed.ui, UI_SIZE, inner.x + 36, y + 19, INK, dlg->entries[i].name);
    }
    gfx_reset_clip(s);

    struct gfx_rect field = field_rect();
    ttf_draw_text(s, ed.ui, UI_SIZE, r.x + 20, field.y + 22, INK_SOFT, "File name");
    field_draw(s, &dlg->field, field, true, dlg->kind == DIALOG_OPEN ? "Choose a file" : NULL);
}

void dialog_draw(struct gfx_surface *s) {
    gfx_fill_rect(s, gfx_rect(0, 0, s->width, s->height), GFX_RGBA(0, 0, 0, 70));
    struct gfx_rect r = card();
    gfx_fill_rounded_rect(s, gfx_rect(r.x - 1, r.y, r.w + 2, r.h + 3), 15, GFX_RGBA(0, 0, 0, 40));
    gfx_fill_rounded_rect(s, r, 14, WHITE);
    ttf_draw_text(s, ed.ui_bold, 16, r.x + 22, r.y + 36, INK, dlg->title);

    if (is_file_dialog()) {
        draw_files(s);
    } else if (dlg->kind == DIALOG_GOTO) {
        ttf_draw_text(s, ed.ui, UI_SIZE, r.x + 22, r.y + 62, INK_SOFT, dlg->message);
        field_draw(s, &dlg->field, field_rect(), true, NULL);
    } else {
        gfx_set_clip(s, gfx_rect_inset(r, 12));
        ttf_draw_text(s, ed.ui, UI_SIZE, r.x + 22, r.y + 66, INK_SOFT, dlg->message);
        gfx_reset_clip(s);
    }

    const char *labels[3];
    int n = buttons(labels);
    for (int i = 0; i < n; i++) {
        struct gfx_rect b = button_rect(i);
        bool main = i == 0;
        bool danger = main && dlg->kind == DIALOG_REPLACE_FILE;
        gfx_color fill = main ? (danger ? DANGER : ACCENT) : FIELD;
        if (dlg->pressed_button == i) {
            fill = main ? GFX_RGB(0x20, 0x55, 0xC0) : GFX_RGB(0xE2, 0xE3, 0xE8);
        } else if (dlg->hover_button == i) {
            fill = main ? (danger ? GFX_RGB(0xEE, 0x50, 0x50) : GFX_RGB(0x45, 0x80, 0xF5))
                        : GFX_RGB(0xEB, 0xEC, 0xF0);
        }
        gfx_fill_rounded_rect(s, b, 8, fill);
        float w = ttf_text_width(ed.ui, UI_SIZE, labels[i]);
        ttf_draw_text(s, ed.ui, UI_SIZE, b.x + (b.w - w) / 2, b.y + 21, main ? WHITE : INK, labels[i]);
    }
}

/* --- acting ------------------------------------------------------------------------------------------ */

/* The path the file dialog's name box points at. */
static void chosen_path(char *out, size_t size) {
    const char *name = dlg->field.text;
    if (!name[0] && dlg->selected >= 0) {
        name = dlg->entries[dlg->selected].name;
    }
    if (name[0] == '/') {
        strlcpy(out, name, size);
    } else if (name[0]) {
        join(out, size, dlg->dir, name);
    } else {
        out[0] = '\0';
    }
}

static void accept_file(void) {
    char path[PATH_MAX];
    chosen_path(path, sizeof(path));
    if (!path[0]) {
        return;
    }
    struct os_file_info info;
    bool exists = stat(path, &info) == 0;
    if (exists && info.type == OS_DIRECTORY) {
        load_dir(path);   /* a folder: go into it */
        field_set(&dlg->field, dlg->kind == DIALOG_SAVE_AS ? "Untitled.txt" : "");
        return;
    }
    if (dlg->kind == DIALOG_OPEN) {
        dialog_close();
        editor_open_path(path);
        return;
    }
    if (exists && strcmp(path, ed.path) != 0) {
        enum pending keep = ed.pending;
        char name[OS_NAME_MAX];
        const char *slash = strrchr(path, '/');
        strlcpy(name, slash ? slash + 1 : path, sizeof(name));
        reset(DIALOG_REPLACE_FILE, "Replace file?");
        snprintf(dlg->message, sizeof(dlg->message), "%s already exists. Do you want to replace it?", name);
        strlcpy(dlg->target, path, sizeof(dlg->target));
        ed.pending = keep;
        return;
    }
    dialog_close();
    if (editor_save_to(path)) {
        editor_after_pending();
    }
}

/* The main button (or Enter). */
static void accept(void) {
    switch (dlg->kind) {
    case DIALOG_OPEN:
    case DIALOG_SAVE_AS:
        accept_file();
        break;
    case DIALOG_GOTO: {
        int line = atoi(dlg->field.text);
        dialog_close();
        editor_goto_line(line);
        break;
    }
    case DIALOG_REPLACE_FILE: {
        char path[PATH_MAX];
        strlcpy(path, dlg->target, sizeof(path));
        dialog_close();
        if (editor_save_to(path)) {
            editor_after_pending();
        }
        break;
    }
    case DIALOG_SAVE_CHANGES:
        dialog_close();
        if (!ed.path[0]) {
            dialog_open_file(DIALOG_SAVE_AS);   /* the pending action waits for that */
        } else if (editor_save_to(ed.path)) {
            editor_after_pending();
        }
        break;
    default:
        dialog_close();
        break;
    }
}

static void press(int i) {
    if (i == 0) {
        accept();
    } else if (i == 1 && dlg->kind == DIALOG_SAVE_CHANGES) {
        dialog_close();
        editor_after_pending();   /* don't save: go ahead anyway */
    } else {
        dialog_close();
        ed.pending = PENDING_NONE;   /* cancel */
    }
}

static int button_at(int x, int y) {
    const char *labels[3];
    int n = buttons(labels);
    for (int i = 0; i < n; i++) {
        if (gfx_rect_contains(button_rect(i), x, y)) {
            return i;
        }
    }
    if (is_file_dialog() && gfx_rect_contains(up_rect(), x, y)) {
        return 10;
    }
    return -1;
}

static int entry_at(int x, int y) {
    struct gfx_rect inner = gfx_rect_inset(list_rect(), 3);
    if (!gfx_rect_contains(inner, x, y)) {
        return -1;
    }
    int i = (y - inner.y + dlg->scroll) / ROW_H;
    return i < dlg->count ? i : -1;
}

static void reveal(int i) {
    struct gfx_rect inner = gfx_rect_inset(list_rect(), 3);
    if (i * ROW_H < dlg->scroll) {
        dlg->scroll = i * ROW_H;
    } else if ((i + 1) * ROW_H > dlg->scroll + inner.h) {
        dlg->scroll = (i + 1) * ROW_H - inner.h;
    }
}

static void select_entry(int i) {
    dlg->selected = i;
    if (i >= 0) {
        reveal(i);
        if (!dlg->entries[i].folder) {
            field_set(&dlg->field, dlg->entries[i].name);
        }
    }
}

void dialog_event(const struct gui_event *ev) {
    switch (ev->type) {
    case GUI_EVENT_KEY:
        if (ev->key == OS_KEY_ESCAPE) {
            press(-1);
        } else if (ev->key == OS_KEY_ENTER) {
            if (is_file_dialog() && !dlg->field.text[0] && dlg->selected >= 0 &&
                dlg->entries[dlg->selected].folder) {
                char path[PATH_MAX];
                join(path, sizeof(path), dlg->dir, dlg->entries[dlg->selected].name);
                load_dir(path);
            } else {
                accept();
            }
        } else if (is_file_dialog() && (ev->key == OS_KEY_UP || ev->key == OS_KEY_DOWN)) {
            int i = dlg->selected + (ev->key == OS_KEY_DOWN ? 1 : -1);
            if (i >= 0 && i < dlg->count) {
                select_entry(i);
            }
        } else if (is_file_dialog() && ev->key == OS_KEY_BACKSPACE && !dlg->field.text[0]) {
            go_up();
        } else if (dlg->kind == DIALOG_GOTO || is_file_dialog()) {
            field_key(&dlg->field, ev);
        }
        break;
    case GUI_EVENT_MOUSE_MOVE:
        dlg->hover_button = button_at(ev->x, ev->y);
        break;
    case GUI_EVENT_MOUSE_DOWN: {
        if (ev->button != OS_MOUSE_LEFT) {
            break;
        }
        dlg->pressed_button = button_at(ev->x, ev->y);
        if (dlg->pressed_button >= 0) {
            break;
        }
        if ((is_file_dialog() || dlg->kind == DIALOG_GOTO) && gfx_rect_contains(field_rect(), ev->x, ev->y)) {
            field_click(&dlg->field, field_rect(), ev->x, ev->modifiers & OS_MOD_SHIFT);
            break;
        }
        int i = is_file_dialog() ? entry_at(ev->x, ev->y) : -1;
        if (i < 0) {
            break;
        }
        uint64_t now = uptime_ms();
        bool twice = i == dlg->last_click_index && now - dlg->last_click_ms < DOUBLE_CLICK;
        dlg->last_click_index = i;
        dlg->last_click_ms = twice ? 0 : now;
        select_entry(i);
        if (twice) {
            if (dlg->entries[i].folder) {
                char path[PATH_MAX];
                join(path, sizeof(path), dlg->dir, dlg->entries[i].name);
                load_dir(path);
            } else {
                accept();
            }
        }
        break;
    }
    case GUI_EVENT_MOUSE_UP: {
        int pressed = dlg->pressed_button;
        dlg->pressed_button = -1;
        if (pressed >= 0 && pressed == button_at(ev->x, ev->y)) {
            if (pressed == 10) {
                go_up();
            } else {
                press(pressed);
            }
        }
        break;
    }
    case GUI_EVENT_SCROLL:
        if (is_file_dialog()) {
            int max = dlg->count * ROW_H - (list_rect().h - 6);
            dlg->scroll += ev->wheel * ROW_H * 2;
            dlg->scroll = dlg->scroll > max ? max : dlg->scroll;
            dlg->scroll = dlg->scroll < 0 ? 0 : dlg->scroll;
        }
        break;
    default:
        break;
    }
}
