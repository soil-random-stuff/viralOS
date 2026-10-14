/*
 * Files: the file manager. Opens in the home folder (or the folder given
 * as an argument) and lists what's in it, folders first.
 *
 *   sidebar                        Home, Desktop, Documents, Downloads, Music, Pictures, Root, then
 *                                  the drives in /media (each with an eject button)
 *   click, Ctrl+click, Shift+click select one, add/remove one, select a range
 *   drag across the list           select everything the rectangle touches
 *   drag selected items            onto a folder, another window or the desktop: moved there
 *   double-click, Enter            open (folders here, text files in the Text Editor)
 *   back / forward / up buttons    Alt+Left, Alt+Right, Backspace
 *   right-click                    open, cut, copy, paste, rename, delete, properties...
 *   Ctrl+X / C / V / A, F2, Delete
 *   Ctrl+H                         show or hide hidden files (names starting with a dot)
 *
 * Cutting, copying and pasting share the clipboard with the desktop. The
 * folder is checked once a second, so changes made elsewhere show up.
 */

#include <fileops.h>
#include <gui.h>
#include <image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_ENTRIES   512
#define HISTORY_MAX   32
#define MAX_DRIVES    8
#define PATH_MAX      FILE_PATH_MAX

#define TOOLBAR_H     50
#define SIDEBAR_W     180
#define SIDE_ROW_H    34
#define HEADER_H      30
#define ROW_H         34
#define STATUS_H      28
#define PAD           14
#define ICON          22
#define SIDE_ICON     18
#define BIG_ICON      48
#define TEXT_SIZE     13.0f
#define SMALL_SIZE    12.0f
#define DOUBLE_CLICK  450     /* ms */
#define DRAG_START    4       /* pixels the mouse moves before a press becomes a marquee */

#define WHITE         GFX_RGB(255, 255, 255)
#define INK           GFX_RGB(0x1C, 0x1C, 0x22)
#define INK_SOFT      GFX_RGB(0x78, 0x78, 0x82)
#define INK_FAINT     GFX_RGB(0xC4, 0xC4, 0xCA)
#define LINE          GFX_RGB(0xEC, 0xEC, 0xF0)
#define FIELD         GFX_RGB(0xF4, 0xF5, 0xF8)
#define SIDEBAR_BG    GFX_RGB(0xF8, 0xF9, 0xFB)
#define ACCENT        GFX_RGB(0x2F, 0x6F, 0xEB)
#define DANGER        GFX_RGB(0xE0, 0x3E, 0x3E)
#define SELECTED      GFX_RGB(0xE3, 0xEC, 0xFD)
#define HOVERED       GFX_RGB(0xF4, 0xF5, 0xF8)

enum {
    MENU_OPEN = 1, MENU_OPEN_EDITOR, MENU_CUT, MENU_COPY, MENU_PASTE, MENU_RENAME, MENU_DELETE,
    MENU_NEW_FOLDER, MENU_NEW_FILE, MENU_REFRESH, MENU_SELECT_ALL, MENU_PROPERTIES, MENU_FOLDER_PROPERTIES,
};
enum { BUTTON_BACK, BUTTON_FORWARD, BUTTON_UP, BUTTON_COUNT };

enum dialog_kind { DIALOG_NONE, DIALOG_RENAME, DIALOG_NEW_FOLDER, DIALOG_NEW_FILE, DIALOG_DELETE,
                   DIALOG_PROPERTIES, DIALOG_ERROR };

/* The sidebar's places; "~" is the user's home folder (filled in at start). */
static struct { const char *label; char path[FILE_PATH_MAX]; } places[] = {
    { "Home", "~" },
    { "Desktop", "~/Desktop" },
    { "Documents", "~/Documents" },
    { "Downloads", "~/Downloads" },
    { "Music", "~/Music" },
    { "Pictures", "~/Pictures" },
    { "Root (/)", "/" },
};

static void resolve_places(void) {
    for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
        if (places[i].path[0] == '~') {
            char rest[FILE_PATH_MAX];
            strlcpy(rest, places[i].path + 1, sizeof(rest));
            snprintf(places[i].path, sizeof(places[i].path), "%s%s", home_dir(), rest);
        }
    }
}
#define PLACES ((int)(sizeof(places) / sizeof(places[0])))

struct dialog {
    enum dialog_kind kind;
    char title[96];
    char message[160];
    char field[OS_NAME_MAX];       /* rename / new: the text being edited */
    int cursor;
    char target[PATH_MAX];         /* rename: the file */
    enum file_icon icon;           /* properties */
    char lines[8][2][96];          /* properties: label, value */
    int line_count;
    int hover_button, pressed_button;
};

static struct {
    struct gui *g;
    struct gui_window *w;
    struct ttf_font *regular, *bold;
    struct gfx_surface *icons[FILE_ICON_COUNT], *big_icons[FILE_ICON_COUNT], *side_icon;
    struct gfx_surface *side_drive[2];   /* [0] internal, [1] external */

    char path[PATH_MAX];
    char back[HISTORY_MAX][PATH_MAX], forward[HISTORY_MAX][PATH_MAX];
    int back_len, forward_len;
    struct os_file_info entries[MAX_ENTRIES];
    bool selected[MAX_ENTRIES];
    int count;
    int focus;                     /* the item the keyboard is on (and Shift ranges start from) */
    int hovered, hover_place;
    int scroll;                    /* pixels the list is scrolled down */
    int hover_button, pressed_button;
    uint64_t last_click_ms;
    int last_click_index;

    /* A press in the list that may turn into a marquee (on empty space) or a drag (on an item). */
    bool pressing, marquee;
    int press_item;
    int press_x, press_y;          /* press_y: in list coordinates (scroll included) */
    int mouse_x, mouse_y;
    bool marquee_base[MAX_ENTRIES];   /* kept selected during the marquee (Ctrl) */

    char status[160];
    struct dialog dialog;
    uint32_t signature;            /* of the folder's contents when last read */

    /* The drives in the sidebar (rechecked once a second); hover_place counts them after the places. */
    struct {
        char path[PATH_MAX];
        bool read_only;
        bool external;
    } drives[MAX_DRIVES];
    int drive_count;
    int hover_eject;
} f;

static void dialog_error(const char *title, const char *what, int err);
static void go(const char *path);
static bool on_drive(const char *path, const char *drive);

/* --- small helpers ------------------------------------------------------------------------ */

static struct gfx_surface *surface(void) {
    return gui_window_surface(f.w);
}

static void format_size(uint64_t size, char *out, size_t n) {
    if (size < 1024) {
        snprintf(out, n, "%lu byte%s", (unsigned long)size, size == 1 ? "" : "s");
    } else if (size < 1024 * 1024) {
        snprintf(out, n, "%.1f KB", size / 1024.0);
    } else {
        snprintf(out, n, "%.1f MB", size / (1024.0 * 1024.0));
    }
}

static void format_date(uint64_t t, char *out, size_t n) {
    static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (!t) {
        strlcpy(out, "-", n);
        return;
    }
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    snprintf(out, n, "%d %s %d, %02d:%02d", tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900,
             tm.tm_hour, tm.tm_min);
}

static int selected_count(void) {
    int n = 0;
    for (int i = 0; i < f.count; i++) {
        n += f.selected[i];
    }
    return n;
}

/* The only selected item, or -1 if there are none or several. */
static int single_selected(void) {
    int found = -1;
    for (int i = 0; i < f.count; i++) {
        if (f.selected[i]) {
            if (found >= 0) {
                return -1;
            }
            found = i;
        }
    }
    return found;
}

static void entry_path(int i, char *out, size_t size) {
    path_join(out, size, f.path, f.entries[i].name);
}

static void clear_selection(void) {
    memset(f.selected, 0, sizeof(f.selected));
}

/* The status bar describes the selection. */
static void update_status(void) {
    int n = selected_count(), one = single_selected();
    if (one >= 0) {
        const struct os_file_info *info = &f.entries[one];
        if (info->type == OS_DIRECTORY) {
            snprintf(f.status, sizeof(f.status), "%s  (folder)", info->name);
        } else {
            char size[32];
            format_size(info->size, size, sizeof(size));
            snprintf(f.status, sizeof(f.status), "%s  (%s)", info->name, size);
        }
    } else if (n > 1) {
        snprintf(f.status, sizeof(f.status), "%d items selected", n);
    } else {
        snprintf(f.status, sizeof(f.status), "%d item%s", f.count, f.count == 1 ? "" : "s");
    }
}

/* --- reading folders ------------------------------------------------------------------------------- */

static int compare(const void *a, const void *b) {
    const struct os_file_info *x = a, *y = b;
    if (x->type != y->type) {
        return x->type == OS_DIRECTORY ? -1 : 1;   /* folders first */
    }
    return name_compare(x->name, y->name);
}

/* A cheap fingerprint of a folder: names, sizes and times. */
static uint32_t folder_signature(const char *path) {
    int fd = open(path, O_RDONLY);
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

static bool show_hidden;   /* Ctrl+H: names starting with '.' too */

static bool load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(f.status, sizeof(f.status), "Cannot open %s: %s", path, strerror(fd));
        return false;
    }
    f.count = 0;
    for (uint64_t i = 0; f.count < MAX_ENTRIES && readdir(fd, i, &f.entries[f.count]) == 0; i++) {
        f.count += show_hidden || f.entries[f.count].name[0] != '.';
    }
    close(fd);
    qsort(f.entries, (size_t)f.count, sizeof(f.entries[0]), compare);
    f.signature = folder_signature(path);
    clear_selection();
    f.focus = -1;
    f.hovered = -1;
    f.scroll = 0;
    if (path != f.path) {
        strlcpy(f.path, path, sizeof(f.path));
    }
    update_status();
    gui_window_set_title(f.w, strcmp(path, "/") == 0 ? "/" : path_base(path));
    return true;
}

/* Read the list of drives; true if it changed. */
static bool load_drives(void) {
    static struct os_drive_info info;
    int n = 0;
    bool changed = false;
    while (n < MAX_DRIVES && drives(n, &info) == 0) {
        changed |= n >= f.drive_count || strcmp(f.drives[n].path, info.path) != 0 ||
                   f.drives[n].external != !!(info.flags & OS_DRIVE_EXTERNAL);
        f.drives[n].external = info.flags & OS_DRIVE_EXTERNAL;
        strlcpy(f.drives[n].path, info.path, sizeof(f.drives[n].path));
        f.drives[n].read_only = info.flags & OS_DRIVE_READ_ONLY;
        n++;
    }
    changed |= n != f.drive_count;
    f.drive_count = n;
    if (f.hover_eject >= n) {
        f.hover_eject = -1;
    }
    return changed;
}

static void eject_drive(int d) {
    char path[PATH_MAX];
    strlcpy(path, f.drives[d].path, sizeof(path));
    if (on_drive(f.path, path)) {
        go(HOME_DIR);   /* step off it first */
    }
    int err = eject(path);
    load_drives();
    if (err < 0) {
        dialog_error("Can't eject the drive", path_base(path), err);
        return;
    }
    snprintf(f.status, sizeof(f.status), "%s can be unplugged now.", path_base(path));
}

/* Reload, keeping the selection (by name), or selecting just `select_name`. */
static void refresh(const char *select_name) {
    static char keep[MAX_ENTRIES][OS_NAME_MAX];
    int kept = 0;
    if (select_name) {
        strlcpy(keep[kept++], select_name, sizeof(keep[0]));
    } else {
        for (int i = 0; i < f.count && kept < MAX_ENTRIES; i++) {
            if (f.selected[i]) {
                strlcpy(keep[kept++], f.entries[i].name, sizeof(keep[0]));
            }
        }
    }
    int scroll = f.scroll;
    load(f.path);
    f.scroll = scroll;
    for (int i = 0; i < f.count; i++) {
        for (int k = 0; k < kept; k++) {
            if (strcmp(f.entries[i].name, keep[k]) == 0) {
                f.selected[i] = true;
                f.focus = i;
            }
        }
    }
    update_status();
}

static void push(char stack[][PATH_MAX], int *len, const char *path) {
    if (*len == HISTORY_MAX) {
        memmove(stack[0], stack[1], sizeof(stack[0]) * (HISTORY_MAX - 1));
        (*len)--;
    }
    strlcpy(stack[(*len)++], path, PATH_MAX);
}

/* Go to a folder as a new step: what was "forward" is forgotten. */
static void go(const char *path) {
    char from[PATH_MAX];
    strlcpy(from, f.path, sizeof(from));
    if (strcmp(path, from) == 0 || !load(path)) {
        return;
    }
    push(f.back, &f.back_len, from);
    f.forward_len = 0;
}

static void go_back(void) {
    if (f.back_len > 0) {
        char from[PATH_MAX];
        strlcpy(from, f.path, sizeof(from));
        if (load(f.back[f.back_len - 1])) {
            f.back_len--;
            push(f.forward, &f.forward_len, from);
        }
    }
}

static void go_forward(void) {
    if (f.forward_len > 0) {
        char from[PATH_MAX];
        strlcpy(from, f.path, sizeof(from));
        if (load(f.forward[f.forward_len - 1])) {
            f.forward_len--;
            push(f.back, &f.back_len, from);
        }
    }
}

static void go_up(void) {
    if (strcmp(f.path, "/") != 0) {
        char path[PATH_MAX];
        path_parent(f.path, path, sizeof(path));
        go(path);
    }
}

static void open_entry(int i) {
    if (i < 0 || i >= f.count) {
        return;
    }
    char path[PATH_MAX];
    entry_path(i, path, sizeof(path));
    if (f.entries[i].type == OS_DIRECTORY) {
        go(path);
        return;
    }
    long pid = open_file(path);
    if (pid == -ENOEXEC) {
        snprintf(f.status, sizeof(f.status), "%s: no app opens this kind of file (try Open with Text Editor)",
                 f.entries[i].name);
    } else if (pid < 0) {
        dialog_error("Can't open", f.entries[i].name, (int)pid);
    }
}

/* --- dialogs ------------------------------------------------------------------------------------------ */

static void dialog_reset(enum dialog_kind kind, const char *title) {
    struct dialog *d = &f.dialog;
    memset(d, 0, sizeof(*d));
    d->kind = kind;
    strlcpy(d->title, title, sizeof(d->title));
    d->hover_button = d->pressed_button = -1;
}

static void draw(void);

/* An error in a dialog window (Error.png); returns once it's been read. */
static void dialog_error(const char *title, const char *what, int err) {
    char message[PATH_MAX + 64];
    snprintf(message, sizeof(message), "%s: %s", what, strerror(err));
    draw();
    gui_message(f.g, f.w, GUI_ICON_ERROR, title, message);
}

static void delete_selected(void) {
    for (int i = 0; i < f.count; i++) {
        if (!f.selected[i]) {
            continue;
        }
        char path[PATH_MAX];
        entry_path(i, path, sizeof(path));
        int err = delete_tree(path);
        if (err < 0) {
            dialog_error("Can't delete", f.entries[i].name, err);
            break;
        }
    }
    clear_selection();
    refresh(NULL);
}

static void dialog_field(const char *text) {
    strlcpy(f.dialog.field, text, sizeof(f.dialog.field));
    f.dialog.cursor = (int)strlen(f.dialog.field);
}

static void dialog_open(enum dialog_kind kind) {
    char path[PATH_MAX];
    int one = single_selected(), n = selected_count();
    switch (kind) {
    case DIALOG_NEW_FOLDER:
        dialog_reset(kind, "New folder");
        free_name(f.path, "New folder", false, path, sizeof(path));
        dialog_field(path_base(path));
        break;
    case DIALOG_NEW_FILE:
        dialog_reset(kind, "New empty file");
        free_name(f.path, "New file.txt", false, path, sizeof(path));
        dialog_field(path_base(path));
        break;
    case DIALOG_RENAME:
        if (one < 0) {
            return;
        }
        dialog_reset(kind, "Rename");
        entry_path(one, f.dialog.target, sizeof(f.dialog.target));
        dialog_field(f.entries[one].name);
        break;
    case DIALOG_DELETE:
        if (!n) {
            return;
        }
        {
            char message[PATH_MAX + 64];
            const char *title;
            if (one >= 0) {
                bool folder = f.entries[one].type == OS_DIRECTORY;
                title = folder ? "Delete folder?" : "Delete file?";
                snprintf(message, sizeof(message),
                         folder ? "\"%s\" and everything in it will be deleted." : "\"%s\" will be deleted.",
                         f.entries[one].name);
            } else {
                title = "Delete items?";
                snprintf(message, sizeof(message), "%d items (and everything in the folders) will be deleted.", n);
            }
            draw();
            if (gui_confirm(f.g, f.w, GUI_ICON_WARNING, title, message, "Delete", true)) {
                delete_selected();
            }
        }
        return;
    default:
        break;
    }
}

static void add_line(const char *label, const char *value) {
    struct dialog *d = &f.dialog;
    if (d->line_count < 8) {
        strlcpy(d->lines[d->line_count][0], label, sizeof(d->lines[0][0]));
        strlcpy(d->lines[d->line_count][1], value, sizeof(d->lines[0][1]));
        d->line_count++;
    }
}

static void properties_of(const char *path) {
    struct os_file_info info;
    if (stat(path, &info) < 0) {
        return;
    }
    dialog_reset(DIALOG_PROPERTIES, strcmp(path, "/") == 0 ? "/" : info.name);
    f.dialog.icon = file_icon_for(&info);
    char value[96], size[32];
    add_line("Type", file_type_name(path, &info));
    path_parent(path, value, sizeof(value));
    add_line("Location", strcmp(path, "/") == 0 ? "-" : value);
    if (info.type == OS_DIRECTORY) {
        uint64_t bytes = 0;
        int files = 0, folders = 0;
        measure_tree(path, &bytes, &files, &folders);
        format_size(bytes, size, sizeof(size));
        add_line("Size", size);
        snprintf(value, sizeof(value), "%d file%s, %d folder%s", files, files == 1 ? "" : "s",
                 folders, folders == 1 ? "" : "s");
        add_line("Contains", value);
    } else {
        format_size(info.size, size, sizeof(size));
        if (info.size >= 1024) {
            snprintf(value, sizeof(value), "%s (%lu bytes)", size, (unsigned long)info.size);
        } else {
            strlcpy(value, size, sizeof(value));
        }
        add_line("Size", value);
    }
    format_date(info.ctime, value, sizeof(value));
    add_line("Created", value);
    format_date(info.mtime, value, sizeof(value));
    add_line("Modified", value);
    char perms[10];
    for (int i = 0; i < 9; i++) {
        perms[i] = info.mode & (0400 >> i) ? "rwx"[i % 3] : '-';
    }
    perms[9] = '\0';
    unsigned mode = info.mode & 0777;
    snprintf(value, sizeof(value), "%s (%u%u%u)", perms, mode >> 6, (mode >> 3) & 7, mode & 7);
    add_line("Permissions", value);
}

/* Several items: what they add up to. */
static void properties_of_selection(void) {
    int one = single_selected();
    if (one >= 0) {
        char path[PATH_MAX];
        entry_path(one, path, sizeof(path));
        properties_of(path);
        return;
    }
    char title[64];
    snprintf(title, sizeof(title), "%d items", selected_count());
    dialog_reset(DIALOG_PROPERTIES, title);
    f.dialog.icon = FILE_ICON_FILE;
    uint64_t bytes = 0;
    int files = 0, folders = 0;
    for (int i = 0; i < f.count; i++) {
        if (!f.selected[i]) {
            continue;
        }
        if (f.entries[i].type == OS_DIRECTORY) {
            char path[PATH_MAX];
            entry_path(i, path, sizeof(path));
            folders++;
            measure_tree(path, &bytes, &files, &folders);
        } else {
            files++;
            bytes += f.entries[i].size;
        }
    }
    char value[96];
    add_line("Location", f.path);
    format_size(bytes, value, sizeof(value));
    add_line("Size", value);
    snprintf(value, sizeof(value), "%d file%s, %d folder%s", files, files == 1 ? "" : "s", folders,
             folders == 1 ? "" : "s");
    add_line("Contains", value);
}

/* The dialog's buttons, right to left: [0] is the main one. */
static int dialog_buttons(const char **labels) {
    switch (f.dialog.kind) {
    case DIALOG_RENAME:     labels[0] = "Rename"; labels[1] = "Cancel"; return 2;
    case DIALOG_NEW_FOLDER:
    case DIALOG_NEW_FILE:   labels[0] = "Create"; labels[1] = "Cancel"; return 2;
    case DIALOG_DELETE:     labels[0] = "Delete"; labels[1] = "Cancel"; return 2;
    default:                labels[0] = "OK"; return 1;
    }
}

static bool has_field(enum dialog_kind kind) {
    return kind == DIALOG_RENAME || kind == DIALOG_NEW_FOLDER || kind == DIALOG_NEW_FILE;
}

static struct gfx_rect dialog_rect(void) {
    struct gfx_surface *s = surface();
    int w = 380, h;
    if (f.dialog.kind == DIALOG_PROPERTIES) {
        h = 120 + f.dialog.line_count * 26 + 56;
    } else {
        h = has_field(f.dialog.kind) ? 160 : 150;
    }
    w = w > s->width - 20 ? s->width - 20 : w;
    return gfx_rect((s->width - w) / 2, (s->height - h) / 2, w, h);
}

static struct gfx_rect dialog_button_rect(int i) {
    struct gfx_rect r = dialog_rect();
    int bw = 88, bh = 32;
    return gfx_rect(r.x + r.w - 18 - bw - i * (bw + 10), r.y + r.h - 18 - bh, bw, bh);
}

static void draw_dialog(void) {
    struct dialog *d = &f.dialog;
    struct gfx_surface *s = surface();
    gfx_fill_rect(s, gfx_rect(0, 0, s->width, s->height), GFX_RGBA(0, 0, 0, 70));
    struct gfx_rect r = dialog_rect();
    gfx_fill_rounded_rect(s, gfx_rect(r.x - 1, r.y, r.w + 2, r.h + 3), 15, GFX_RGBA(0, 0, 0, 40));
    gfx_fill_rounded_rect(s, r, 14, WHITE);

    int x = r.x + 22, y = r.y + 36;
    if (d->kind == DIALOG_PROPERTIES) {
        struct gfx_surface *icon = f.big_icons[d->icon];
        if (icon) {
            gfx_blit(s, x, r.y + 22, icon, NULL);
        }
        gfx_set_clip(s, gfx_rect(x + BIG_ICON + 14, r.y, r.w - BIG_ICON - 58, r.h));
        ttf_draw_text(s, f.bold, 16, x + BIG_ICON + 14, r.y + 52, INK, d->title);
        gfx_reset_clip(s);
        y = r.y + 104;
        gfx_fill_rect(s, gfx_rect(x, y - 18, r.w - 44, 1), LINE);
        gfx_set_clip(s, gfx_rect_inset(r, 10));
        for (int i = 0; i < d->line_count; i++) {
            ttf_draw_text(s, f.regular, TEXT_SIZE, x, y + i * 26, INK_SOFT, d->lines[i][0]);
            ttf_draw_text(s, f.regular, TEXT_SIZE, x + 100, y + i * 26, INK, d->lines[i][1]);
        }
        gfx_reset_clip(s);
    } else {
        ttf_draw_text(s, f.bold, 16, x, y, INK, d->title);
        if (has_field(d->kind)) {
            struct gfx_rect box = gfx_rect(x, y + 16, r.w - 44, 36);
            gfx_fill_rounded_rect(s, box, 8, FIELD);
            gfx_stroke_rounded_rect(s, box, 8, 1.5f, ACCENT);
            gfx_set_clip(s, gfx_rect_inset(box, 4));
            float tx = box.x + 12, ty = box.y + 23;
            ttf_draw_text(s, f.regular, TEXT_SIZE, tx, ty, INK, d->field);
            char before[OS_NAME_MAX];
            strlcpy(before, d->field, sizeof(before));
            before[d->cursor] = '\0';
            float cx = tx + ttf_text_width(f.regular, TEXT_SIZE, before);
            gfx_fill_rect(s, gfx_rect((int)cx, box.y + 9, 1, 18), INK);
            gfx_reset_clip(s);
        } else {
            gfx_set_clip(s, gfx_rect_inset(r, 10));
            ttf_draw_text(s, f.regular, TEXT_SIZE, x, y + 30, INK_SOFT, d->message);
            gfx_reset_clip(s);
        }
    }

    const char *labels[2];
    int n = dialog_buttons(labels);
    for (int i = 0; i < n; i++) {
        struct gfx_rect b = dialog_button_rect(i);
        bool main = i == 0;
        gfx_color fill = main ? (d->kind == DIALOG_DELETE ? DANGER : ACCENT) : FIELD;
        if (d->pressed_button == i) {
            fill = main ? GFX_RGB(0x20, 0x55, 0xC0) : GFX_RGB(0xE2, 0xE3, 0xE8);
        } else if (d->hover_button == i) {
            fill = main ? (d->kind == DIALOG_DELETE ? GFX_RGB(0xEE, 0x50, 0x50) : GFX_RGB(0x45, 0x80, 0xF5))
                        : GFX_RGB(0xEB, 0xEC, 0xF0);
        }
        gfx_fill_rounded_rect(s, b, 8, fill);
        float w = ttf_text_width(f.regular, TEXT_SIZE, labels[i]);
        ttf_draw_text(s, f.regular, TEXT_SIZE, b.x + (b.w - w) / 2, b.y + 21, main ? WHITE : INK, labels[i]);
    }
}

/* The main button: do what the dialog is for. */
static void dialog_accept(void) {
    struct dialog *d = &f.dialog;
    enum dialog_kind kind = d->kind;
    d->kind = DIALOG_NONE;
    if (has_field(kind)) {
        const char *what = kind == DIALOG_RENAME ? "Can't rename" : "Can't create it";
        if (!d->field[0] || strchr(d->field, '/') || strcmp(d->field, ".") == 0 || strcmp(d->field, "..") == 0) {
            dialog_error(what, d->field[0] ? d->field : "(empty)", -EINVAL);
            return;
        }
        char path[PATH_MAX];
        path_join(path, sizeof(path), f.path, d->field);
        int err = 0;
        if (kind == DIALOG_RENAME) {
            err = strcmp(path, d->target) == 0 ? 0 : rename(d->target, path);
        } else if (kind == DIALOG_NEW_FOLDER) {
            err = mkdir(path);
        } else {
            struct os_file_info info;
            err = stat(path, &info) == 0 ? -EEXIST : open(path, O_WRONLY | O_CREAT);
            if (err >= 0) {
                close(err);
                err = 0;
            }
        }
        if (err < 0) {
            dialog_error(what, d->field, err);
            return;
        }
        refresh(d->field);
    }
}

static void dialog_key(const struct gui_event *ev) {
    struct dialog *d = &f.dialog;
    if (ev->key == OS_KEY_ESCAPE) {
        d->kind = DIALOG_NONE;
        return;
    }
    if (ev->key == OS_KEY_ENTER) {
        dialog_accept();
        return;
    }
    if (!has_field(d->kind)) {
        return;
    }
    int len = (int)strlen(d->field);
    switch (ev->key) {
    case OS_KEY_LEFT:  d->cursor -= d->cursor > 0; return;
    case OS_KEY_RIGHT: d->cursor += d->cursor < len; return;
    case OS_KEY_HOME:  d->cursor = 0; return;
    case OS_KEY_END:   d->cursor = len; return;
    case OS_KEY_BACKSPACE:
        if (d->cursor > 0) {
            memmove(d->field + d->cursor - 1, d->field + d->cursor, (size_t)(len - d->cursor + 1));
            d->cursor--;
        }
        return;
    case OS_KEY_DELETE:
        if (d->cursor < len) {
            memmove(d->field + d->cursor, d->field + d->cursor + 1, (size_t)(len - d->cursor));
        }
        return;
    }
    if (ev->ch >= ' ' && ev->ch < 0x7F && ev->ch != '/' && len < (int)sizeof(d->field) - 1 &&
        !(ev->modifiers & OS_MOD_CTRL)) {
        memmove(d->field + d->cursor + 1, d->field + d->cursor, (size_t)(len - d->cursor + 1));
        d->field[d->cursor++] = (char)ev->ch;
    }
}

static int dialog_button_at(int x, int y) {
    const char *labels[2];
    int n = dialog_buttons(labels);
    for (int i = 0; i < n; i++) {
        if (gfx_rect_contains(dialog_button_rect(i), x, y)) {
            return i;
        }
    }
    return -1;
}

static void dialog_mouse(const struct gui_event *ev) {
    struct dialog *d = &f.dialog;
    int b = dialog_button_at(ev->x, ev->y);
    if (ev->type == GUI_EVENT_MOUSE_MOVE) {
        d->hover_button = b;
    } else if (ev->type == GUI_EVENT_MOUSE_DOWN && ev->button == OS_MOUSE_LEFT) {
        d->pressed_button = b;
    } else if (ev->type == GUI_EVENT_MOUSE_UP && ev->button == OS_MOUSE_LEFT) {
        int pressed = d->pressed_button;
        d->pressed_button = -1;
        if (pressed >= 0 && pressed == b) {
            if (b == 0) {
                dialog_accept();
            } else {
                d->kind = DIALOG_NONE;
            }
        }
    }
}

/* --- clipboard actions ------------------------------------------------------------------------- */

static void cut_or_copy(bool cut) {
    static char paths[CLIPBOARD_MAX][PATH_MAX];
    const char *list[CLIPBOARD_MAX];
    int n = 0;
    for (int i = 0; i < f.count && n < CLIPBOARD_MAX; i++) {
        if (f.selected[i]) {
            entry_path(i, paths[n], PATH_MAX);
            list[n] = paths[n];
            n++;
        }
    }
    if (!n) {
        return;
    }
    clipboard_files_set(list, n, cut);
    if (n == 1) {
        snprintf(f.status, sizeof(f.status), cut ? "Cut %s: paste it somewhere to move it" : "Copied %s",
                 path_base(paths[0]));
    } else {
        snprintf(f.status, sizeof(f.status), cut ? "Cut %d items" : "Copied %d items", n);
    }
}

static void paste(void) {
    char last[OS_NAME_MAX] = "", failed[OS_NAME_MAX] = "";
    int n = clipboard_files_paste(f.path, last, sizeof(last), failed, sizeof(failed));
    refresh(last[0] ? last : NULL);
    if (n < 0) {
        dialog_error("Can't paste", n == -EINVAL ? "A folder can't go inside itself" : failed, n);
    } else if (n == 0) {
        strlcpy(f.status, "Nothing to paste", sizeof(f.status));
    }
}

static bool can_paste(void) {
    static char paths[1][PATH_MAX];
    bool cut;
    return clipboard_files_get(paths, 1, &cut) > 0;
}

/* --- layout and drawing -------------------------------------------------------------------------- */

static struct gfx_rect list_area(void) {
    struct gfx_surface *s = surface();
    int top = TOOLBAR_H + HEADER_H;
    return gfx_rect(SIDEBAR_W, top, s->width - SIDEBAR_W, s->height - top - STATUS_H);
}

static struct gfx_rect place_rect(int i) {
    int y = TOOLBAR_H + 10 + i * SIDE_ROW_H + (i == PLACES - 1 ? 12 : 0);   /* Root after a gap */
    if (i >= PLACES) {
        y = TOOLBAR_H + 10 + PLACES * SIDE_ROW_H + 12 + 30 + (i - PLACES) * SIDE_ROW_H;   /* under "Drives" */
    }
    return gfx_rect(8, y, SIDEBAR_W - 16, SIDE_ROW_H - 2);
}

static struct gfx_rect eject_rect(int drive) {
    struct gfx_rect r = place_rect(PLACES + drive);
    return gfx_rect(r.x + r.w - 28, r.y + (r.h - 24) / 2, 24, 24);
}

/* The path a sidebar entry opens: a place or a drive. */
static const char *place_path(int i) {
    return i < PLACES ? places[i].path : f.drives[i - PLACES].path;
}

/* Is `path` the drive's folder or inside it? */
static bool on_drive(const char *path, const char *drive) {
    size_t n = strlen(drive);
    return strncmp(path, drive, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

static struct gfx_rect button_rect(int i) {
    return gfx_rect(PAD - 4 + i * 36, (TOOLBAR_H - 32) / 2, 32, 32);
}

static int max_scroll(void) {
    int h = f.count * ROW_H + 8 - list_area().h;
    return h > 0 ? h : 0;
}

static int name_column(void) {
    return SIDEBAR_W + PAD + ICON + 14;
}

static int size_column(void) {
    return surface()->width - 290;
}

static int date_column(void) {
    return surface()->width - 180;
}

/* Row i on screen (list scrolled). */
static struct gfx_rect row_rect(int i) {
    struct gfx_rect area = list_area();
    return gfx_rect(area.x + PAD - 6, area.y + 4 + i * ROW_H - f.scroll, area.w - 2 * PAD + 12, ROW_H);
}

static void draw_arrow(struct gfx_surface *s, struct gfx_rect r, int which, gfx_color ink) {
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, k = 6;
    if (which == BUTTON_UP) {
        gfx_draw_line(s, cx, cy - k, cx, cy + k, 1.8f, ink);
        gfx_draw_line(s, cx, cy - k, cx - 5, cy - 1, 1.8f, ink);
        gfx_draw_line(s, cx, cy - k, cx + 5, cy - 1, 1.8f, ink);
        return;
    }
    float dir = which == BUTTON_BACK ? -1 : 1;
    gfx_draw_line(s, cx - k, cy, cx + k, cy, 1.8f, ink);
    gfx_draw_line(s, cx + dir * k, cy, cx + dir * 1, cy - 5, 1.8f, ink);
    gfx_draw_line(s, cx + dir * k, cy, cx + dir * 1, cy + 5, 1.8f, ink);
}

static bool button_enabled(int i) {
    switch (i) {
    case BUTTON_BACK:    return f.back_len > 0;
    case BUTTON_FORWARD: return f.forward_len > 0;
    default:             return strcmp(f.path, "/") != 0;
    }
}

static void draw_toolbar(void) {
    struct gfx_surface *s = surface();
    gfx_fill_rect(s, gfx_rect(0, 0, s->width, TOOLBAR_H), WHITE);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        struct gfx_rect r = button_rect(i);
        bool on = button_enabled(i);
        if (on && f.pressed_button == i) {
            gfx_fill_rounded_rect(s, r, 8, GFX_RGBA(0, 0, 0, 28));
        } else if (on && f.hover_button == i) {
            gfx_fill_rounded_rect(s, r, 8, GFX_RGBA(0, 0, 0, 14));
        }
        draw_arrow(s, r, i, on ? INK : INK_FAINT);
    }
    int x = button_rect(BUTTON_COUNT - 1).x + 44;
    struct gfx_rect box = gfx_rect(x, (TOOLBAR_H - 32) / 2, s->width - x - PAD, 32);
    gfx_fill_rounded_rect(s, box, 8, FIELD);
    gfx_set_clip(s, gfx_rect_inset(box, 4));
    ttf_draw_text(s, f.regular, TEXT_SIZE, box.x + 12, box.y + 21, INK, f.path);
    gfx_reset_clip(s);
    gfx_fill_rect(s, gfx_rect(0, TOOLBAR_H - 1, s->width, 1), LINE);
}

static void draw_sidebar(void) {
    struct gfx_surface *s = surface();
    gfx_fill_rect(s, gfx_rect(0, TOOLBAR_H, SIDEBAR_W, s->height - TOOLBAR_H), SIDEBAR_BG);
    gfx_fill_rect(s, gfx_rect(SIDEBAR_W - 1, TOOLBAR_H, 1, s->height - TOOLBAR_H), LINE);
    for (int i = 0; i < PLACES; i++) {
        struct gfx_rect r = place_rect(i);
        bool here = strcmp(places[i].path, f.path) == 0;
        if (here) {
            gfx_fill_rounded_rect(s, r, 8, SELECTED);
        } else if (f.hover_place == i) {
            gfx_fill_rounded_rect(s, r, 8, GFX_RGBA(0, 0, 0, 12));
        }
        if (f.side_icon) {
            gfx_blit(s, r.x + 10, r.y + (r.h - SIDE_ICON) / 2, f.side_icon, NULL);
        }
        ttf_draw_text(s, here ? f.bold : f.regular, TEXT_SIZE, r.x + 38, r.y + r.h / 2.0f + 4.5f, INK,
                      places[i].label);
    }
    struct gfx_rect root = place_rect(PLACES - 1);
    gfx_fill_rect(s, gfx_rect(root.x + 8, root.y - 8, root.w - 16, 1), LINE);

    if (!f.drive_count) {
        return;
    }
    ttf_draw_text(s, f.bold, SMALL_SIZE, root.x + 10, root.y + SIDE_ROW_H + 26, INK_SOFT, "Drives");
    for (int d = 0; d < f.drive_count; d++) {
        struct gfx_rect r = place_rect(PLACES + d);
        bool here = strcmp(f.drives[d].path, f.path) == 0;
        if (here) {
            gfx_fill_rounded_rect(s, r, 8, SELECTED);
        } else if (f.hover_place == PLACES + d) {
            gfx_fill_rounded_rect(s, r, 8, GFX_RGBA(0, 0, 0, 12));
        }
        struct gfx_surface *icon = f.side_drive[f.drives[d].external];
        if (icon) {
            gfx_blit(s, r.x + 10, r.y + (r.h - SIDE_ICON) / 2, icon, NULL);
        }
        /* The name, clipped before the eject button. */
        struct gfx_rect eject = eject_rect(d);
        gfx_set_clip(s, gfx_rect(r.x + 38, r.y, eject.x - r.x - 40, r.h));
        ttf_draw_text(s, here ? f.bold : f.regular, TEXT_SIZE, r.x + 38, r.y + r.h / 2.0f + 4.5f, INK,
                      path_base(f.drives[d].path));
        gfx_reset_clip(s);
        /* Eject: a triangle over a bar. */
        bool hot = f.hover_eject == d;
        if (hot) {
            gfx_fill_circle(s, eject.x + 12.0f, eject.y + 12.0f, 11.0f, GFX_RGBA(0, 0, 0, 20));
        }
        gfx_color ink = hot ? INK : INK_SOFT;
        float cx = eject.x + 12.0f, cy = eject.y + 11.0f;
        float tri[6] = { cx - 5.5f, cy + 1.5f, cx + 5.5f, cy + 1.5f, cx, cy - 5.0f };
        gfx_fill_polygon(s, tri, 3, ink);
        gfx_fill_rect(s, gfx_rect((int)(cx - 5.5f), (int)(cy + 3.5f), 11, 2), ink);
    }
}

static void draw_header(void) {
    struct gfx_surface *s = surface();
    gfx_fill_rect(s, gfx_rect(SIDEBAR_W, TOOLBAR_H, s->width - SIDEBAR_W, HEADER_H), WHITE);
    float baseline = TOOLBAR_H + 19;
    ttf_draw_text(s, f.regular, SMALL_SIZE, name_column(), baseline, INK_SOFT, "Name");
    ttf_draw_text(s, f.regular, SMALL_SIZE, size_column(), baseline, INK_SOFT, "Size");
    ttf_draw_text(s, f.regular, SMALL_SIZE, date_column(), baseline, INK_SOFT, "Modified");
    gfx_fill_rect(s, gfx_rect(SIDEBAR_W + PAD, TOOLBAR_H + HEADER_H - 1, s->width - SIDEBAR_W - 2 * PAD, 1), LINE);
}

/* The marquee on screen, from the press point (which scrolls with the list) to the mouse. */
static struct gfx_rect marquee_rect(void) {
    struct gfx_rect area = list_area();
    int x0 = f.press_x, y0 = area.y + f.press_y - f.scroll;
    int x1 = f.mouse_x, y1 = f.mouse_y;
    int left = x0 < x1 ? x0 : x1, top = y0 < y1 ? y0 : y1;
    return gfx_rect(left, top, (x0 > x1 ? x0 - x1 : x1 - x0) + 1, (y0 > y1 ? y0 - y1 : y1 - y0) + 1);
}

static void draw_marquee(struct gfx_surface *s, struct gfx_rect m) {
    gfx_fill_rect(s, m, GFX_RGBA(0x2F, 0x6F, 0xEB, 40));
    gfx_fill_rect(s, gfx_rect(m.x, m.y, m.w, 1), ACCENT);
    gfx_fill_rect(s, gfx_rect(m.x, m.y + m.h - 1, m.w, 1), ACCENT);
    gfx_fill_rect(s, gfx_rect(m.x, m.y, 1, m.h), ACCENT);
    gfx_fill_rect(s, gfx_rect(m.x + m.w - 1, m.y, 1, m.h), ACCENT);
}

static void draw_list(void) {
    struct gfx_surface *s = surface();
    struct gfx_rect area = list_area();
    gfx_set_clip(s, area);
    gfx_fill_rect(s, area, WHITE);
    if (f.count == 0) {
        const char *empty = "This folder is empty";
        float w = ttf_text_width(f.regular, TEXT_SIZE, empty);
        ttf_draw_text(s, f.regular, TEXT_SIZE, area.x + (area.w - w) / 2, area.y + 60, INK_SOFT, empty);
    }
    /* Cut items show faded until they're pasted. */
    static char cut_paths[CLIPBOARD_MAX][PATH_MAX];
    bool cut = false;
    int cut_count = clipboard_files_get(cut_paths, CLIPBOARD_MAX, &cut);
    if (!cut) {
        cut_count = 0;
    }
    for (int i = 0; i < f.count; i++) {
        struct gfx_rect row = row_rect(i);
        if (row.y + ROW_H < area.y || row.y > area.y + area.h) {
            continue;
        }
        if (f.selected[i]) {
            gfx_fill_rounded_rect(s, row, 7, SELECTED);
        } else if (i == f.hovered && !f.marquee) {
            gfx_fill_rounded_rect(s, row, 7, HOVERED);
        }
        const struct os_file_info *info = &f.entries[i];
        char path[PATH_MAX];
        entry_path(i, path, sizeof(path));
        bool is_cut = false;
        for (int k = 0; k < cut_count && !is_cut; k++) {
            is_cut = strcmp(cut_paths[k], path) == 0;
        }
        struct gfx_surface *icon = f.icons[file_icon_for(info)];
        int iy = row.y + (ROW_H - ICON) / 2;
        if (icon) {
            gfx_blit(s, area.x + PAD + 1, iy, icon, NULL);
            if (is_cut) {
                gfx_fill_rect(s, gfx_rect(area.x + PAD + 1, iy, ICON, ICON), GFX_RGBA(255, 255, 255, 140));
            }
        }
        float baseline = row.y + ROW_H / 2.0f + 5;
        gfx_set_clip(s, gfx_rect_intersect(area, gfx_rect(0, row.y, size_column() - 12, ROW_H)));
        ttf_draw_text(s, f.regular, TEXT_SIZE, name_column(), baseline, is_cut ? INK_SOFT : INK, info->name);
        gfx_set_clip(s, area);
        char text[48];
        if (info->type == OS_DIRECTORY) {
            strlcpy(text, "Folder", sizeof(text));
        } else {
            format_size(info->size, text, sizeof(text));
        }
        ttf_draw_text(s, f.regular, SMALL_SIZE, size_column(), baseline, INK_SOFT, text);
        format_date(info->mtime, text, sizeof(text));
        ttf_draw_text(s, f.regular, SMALL_SIZE, date_column(), baseline, INK_SOFT, text);
    }
    if (f.marquee) {
        draw_marquee(s, gfx_rect_intersect(marquee_rect(), area));
    }
    int total = f.count * ROW_H + 8;
    if (total > area.h) {
        int bar_h = area.h * area.h / total;
        bar_h = bar_h < 24 ? 24 : bar_h;
        int bar_y = area.y + (area.h - bar_h) * f.scroll / max_scroll();
        gfx_fill_rounded_rect(s, gfx_rect(s->width - 8, bar_y + 2, 4, bar_h - 4), 2, GFX_RGBA(0, 0, 0, 70));
    }
    gfx_reset_clip(s);
}

static void draw_status(void) {
    struct gfx_surface *s = surface();
    int y = s->height - STATUS_H;
    gfx_fill_rect(s, gfx_rect(SIDEBAR_W, y, s->width - SIDEBAR_W, STATUS_H), WHITE);
    gfx_fill_rect(s, gfx_rect(SIDEBAR_W, y, s->width - SIDEBAR_W, 1), LINE);
    gfx_set_clip(s, gfx_rect(SIDEBAR_W, y, s->width - SIDEBAR_W - PAD, STATUS_H));
    ttf_draw_text(s, f.regular, SMALL_SIZE, SIDEBAR_W + PAD, y + 18, INK_SOFT, f.status);
    gfx_reset_clip(s);
}

static void draw(void) {
    draw_list();
    draw_header();
    draw_sidebar();
    draw_toolbar();
    draw_status();
    if (f.dialog.kind != DIALOG_NONE) {
        draw_dialog();
    }
    gui_window_invalidate(f.w, NULL);
}

/* --- selecting ------------------------------------------------------------------------------------- */

static int entry_at(int x, int y) {
    struct gfx_rect area = list_area();
    if (!gfx_rect_contains(area, x, y)) {
        return -1;
    }
    int offset = y - area.y - 4 + f.scroll;
    int i = offset / ROW_H;
    return offset >= 0 && i < f.count ? i : -1;
}

static int button_at(int x, int y) {
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (gfx_rect_contains(button_rect(i), x, y)) {
            return i;
        }
    }
    return -1;
}

static int place_at(int x, int y) {
    for (int i = 0; i < PLACES + f.drive_count; i++) {
        if (gfx_rect_contains(place_rect(i), x, y)) {
            return i;
        }
    }
    return -1;
}

static int eject_at(int x, int y) {
    for (int i = 0; i < f.drive_count; i++) {
        if (gfx_rect_contains(eject_rect(i), x, y)) {
            return i;
        }
    }
    return -1;
}

static void scroll_to(int scroll) {
    int max = max_scroll();
    f.scroll = scroll < 0 ? 0 : scroll > max ? max : scroll;
}

static void reveal(int i) {
    struct gfx_rect area = list_area();
    int top = 4 + i * ROW_H;
    if (top < f.scroll) {
        scroll_to(top - 4);
    } else if (top + ROW_H > f.scroll + area.h) {
        scroll_to(top + ROW_H - area.h + 4);
    }
}

static void select_only(int i) {
    clear_selection();
    if (i >= 0 && i < f.count) {
        f.selected[i] = true;
        f.focus = i;
        reveal(i);
    }
    update_status();
}

static void select_range(int from, int to, bool add) {
    if (!add) {
        clear_selection();
    }
    if (from < 0) {
        from = to;
    }
    int a = from < to ? from : to, b = from < to ? to : from;
    for (int i = a; i <= b && i < f.count; i++) {
        if (i >= 0) {
            f.selected[i] = true;
        }
    }
    f.focus = to;
    reveal(to);
    update_status();
}

static void select_all(void) {
    for (int i = 0; i < f.count; i++) {
        f.selected[i] = true;
    }
    update_status();
}

/* While dragging: everything the rectangle touches (plus what was selected, with Ctrl). */
static void update_marquee(void) {
    struct gfx_rect m = marquee_rect();
    for (int i = 0; i < f.count; i++) {
        bool hit = !gfx_rect_empty(gfx_rect_intersect(row_rect(i), m));
        f.selected[i] = f.marquee_base[i] || hit;
    }
    update_status();
}

/* --- input ------------------------------------------------------------------------------------------- */

static void context_menu(const struct gui_event *ev, int i) {
    if (i >= 0) {
        if (!f.selected[i]) {
            select_only(i);   /* right-click outside the selection: just that one */
        }
        int one = single_selected();
        bool folder = one >= 0 && f.entries[one].type == OS_DIRECTORY;
        bool multi = one < 0;
        const struct gui_menu_item items[] = {
            { "Open", MENU_OPEN, multi, false },
            { "Open with Text Editor", MENU_OPEN_EDITOR, multi || folder, false },
            { NULL, 0, false, false },
            { "Cut", MENU_CUT, false, false },
            { "Copy", MENU_COPY, false, false },
            { "Paste", MENU_PASTE, !can_paste(), false },
            { NULL, 0, false, false },
            { "Rename", MENU_RENAME, multi, false },
            { "Delete", MENU_DELETE, false, false },
            { NULL, 0, false, false },
            { "Properties", MENU_PROPERTIES, false, false },
        };
        draw();
        gui_show_menu(f.g, ev->screen_x, ev->screen_y, items, 11, f.w);
    } else {
        select_only(-1);
        const struct gui_menu_item items[] = {
            { "Paste", MENU_PASTE, !can_paste(), false },
            { NULL, 0, false, false },
            { "New folder", MENU_NEW_FOLDER, false, false },
            { "New empty file", MENU_NEW_FILE, false, false },
            { NULL, 0, false, false },
            { "Select all", MENU_SELECT_ALL, !f.count, false },
            { "Refresh", MENU_REFRESH, false, false },
            { "Properties", MENU_FOLDER_PROPERTIES, false, false },
        };
        draw();
        gui_show_menu(f.g, ev->screen_x, ev->screen_y, items, 8, f.w);
    }
}

static void on_menu(int id) {
    int one = single_selected();
    switch (id) {
    case MENU_OPEN:       open_entry(one); break;
    case MENU_OPEN_EDITOR:
        if (one >= 0) {
            char path[PATH_MAX];
            entry_path(one, path, sizeof(path));
            long pid = open_in_text_editor(path);
            if (pid < 0) {
                dialog_error("Can't open the Text Editor", f.entries[one].name, (int)pid);
            }
        }
        break;
    case MENU_CUT:        cut_or_copy(true); break;
    case MENU_COPY:       cut_or_copy(false); break;
    case MENU_PASTE:      paste(); break;
    case MENU_RENAME:     dialog_open(DIALOG_RENAME); break;
    case MENU_DELETE:     dialog_open(DIALOG_DELETE); break;
    case MENU_NEW_FOLDER: dialog_open(DIALOG_NEW_FOLDER); break;
    case MENU_NEW_FILE:   dialog_open(DIALOG_NEW_FILE); break;
    case MENU_SELECT_ALL: select_all(); break;
    case MENU_REFRESH:    refresh(NULL); break;
    case MENU_PROPERTIES: properties_of_selection(); break;
    case MENU_FOLDER_PROPERTIES: properties_of(f.path); break;
    }
}

static void on_mouse_down(const struct gui_event *ev) {
    int b = button_at(ev->x, ev->y);
    if (b >= 0 && ev->button == OS_MOUSE_LEFT) {
        f.pressed_button = b;
        return;
    }
    int ej = eject_at(ev->x, ev->y);
    if (ej >= 0 && ev->button == OS_MOUSE_LEFT) {
        eject_drive(ej);
        return;
    }
    int place = place_at(ev->x, ev->y);
    if (place >= 0 && ev->button == OS_MOUSE_LEFT) {
        go(place_path(place));
        return;
    }
    struct gfx_rect area = list_area();
    if (!gfx_rect_contains(area, ev->x, ev->y)) {
        return;
    }
    int i = entry_at(ev->x, ev->y);
    if (ev->button == OS_MOUSE_RIGHT) {
        context_menu(ev, i);
        return;
    }
    if (ev->button != OS_MOUSE_LEFT) {
        return;
    }
    bool ctrl = ev->modifiers & OS_MOD_CTRL, shift = ev->modifiers & OS_MOD_SHIFT;
    uint64_t now = uptime_ms();
    if (i >= 0 && !ctrl && !shift && i == f.last_click_index && now - f.last_click_ms < DOUBLE_CLICK) {
        f.last_click_ms = 0;
        open_entry(i);
        return;
    }
    f.last_click_ms = now;
    f.last_click_index = i;

    if (shift && i >= 0) {
        select_range(f.focus, i, ctrl);
    } else if (ctrl && i >= 0) {
        f.selected[i] = !f.selected[i];
        f.focus = i;
        update_status();
    } else if (i >= 0 && f.selected[i]) {
        f.focus = i;   /* part of a multiple selection: decided on release */
    } else if (!ctrl) {
        select_only(i);
    }
    /* Maybe the start of a marquee or a drag. */
    f.pressing = true;
    f.press_item = i;
    f.marquee = false;
    f.press_x = ev->x;
    f.press_y = ev->y - area.y + f.scroll;
    f.mouse_x = ev->x;
    f.mouse_y = ev->y;
    if (ctrl) {
        memcpy(f.marquee_base, f.selected, sizeof(f.selected));
    } else {
        memset(f.marquee_base, 0, sizeof(f.marquee_base));
    }
}

/* Files dropped on the window: into the folder they were dropped on, or the one shown. */
static void on_drop(const struct gui_event *ev) {
    static char paths[256][FILE_PATH_MAX];
    int n = gui_drop_paths(paths, 256);
    if (!n) {
        return;
    }
    char target[PATH_MAX];
    strlcpy(target, f.path, sizeof(target));
    int i = entry_at(ev->x, ev->y);
    if (i >= 0 && f.entries[i].type == OS_DIRECTORY) {
        char folder[PATH_MAX];
        entry_path(i, folder, sizeof(folder));
        bool dragged = false;
        for (int k = 0; k < n; k++) {
            dragged |= strcmp(paths[k], folder) == 0;
        }
        if (!dragged) {
            strlcpy(target, folder, sizeof(target));
        }
    }
    char failed[OS_NAME_MAX] = "";
    int moved = move_into(paths, n, target, NULL, failed, sizeof(failed));
    refresh(NULL);
    if (moved < 0) {
        dialog_error("Can't move it", failed, moved);
    } else if (moved > 0) {
        snprintf(f.status, sizeof(f.status), "Moved %d item%s to %s", moved, moved == 1 ? "" : "s",
                 strcmp(target, "/") == 0 ? "/" : path_base(target));
    }
}

static void on_mouse_up(const struct gui_event *ev) {
    int pressed = f.pressed_button;
    f.pressed_button = -1;
    if (pressed >= 0 && button_at(ev->x, ev->y) == pressed && button_enabled(pressed)) {
        if (pressed == BUTTON_BACK) {
            go_back();
        } else if (pressed == BUTTON_FORWARD) {
            go_forward();
        } else {
            go_up();
        }
    }
    if (f.pressing && !f.marquee && ev->button == OS_MOUSE_LEFT) {
        /* A plain click on one item of a multiple selection: now just that item. */
        int i = entry_at(ev->x, ev->y);
        if (i >= 0 && !(ev->modifiers & (OS_MOD_CTRL | OS_MOD_SHIFT)) && selected_count() > 1) {
            select_only(i);
        }
    }
    f.pressing = false;
    f.marquee = false;
}

static void on_mouse_move(const struct gui_event *ev) {
    f.mouse_x = ev->x;
    f.mouse_y = ev->y;
    if (!f.pressing || !(ev->buttons & OS_MOUSE_LEFT)) {
        return;
    }
    struct gfx_rect area = list_area();
    int dx = ev->x - f.press_x, dy = ev->y - (area.y + f.press_y - f.scroll);
    if (!f.marquee && dx * dx + dy * dy >= DRAG_START * DRAG_START) {
        if (f.press_item >= 0 && f.selected[f.press_item]) {
            /* Dragging items: the desktop takes it from here (see on_drop for the other side). */
            static char paths[CLIPBOARD_MAX][PATH_MAX];
            const char *list[CLIPBOARD_MAX];
            int n = 0;
            for (int i = 0; i < f.count && n < CLIPBOARD_MAX; i++) {
                if (f.selected[i]) {
                    entry_path(i, paths[n], PATH_MAX);
                    list[n] = paths[n];
                    n++;
                }
            }
            f.pressing = false;
            gui_start_drag(f.w, list, n);
            return;
        }
        f.marquee = true;
    }
    if (f.marquee) {
        /* Past the top or bottom: scroll along. */
        if (ev->y < area.y) {
            scroll_to(f.scroll - ROW_H / 2);
        } else if (ev->y > area.y + area.h) {
            scroll_to(f.scroll + ROW_H / 2);
        }
        update_marquee();
    }
}

#define KEY_A  0x1E
#define KEY_X  0x2D
#define KEY_C  0x2E
#define KEY_V  0x2F
#define KEY_F2 0x3C
#define KEY_H  0x23

static void on_key(const struct gui_event *ev) {
    bool ctrl = ev->modifiers & OS_MOD_CTRL, alt = ev->modifiers & OS_MOD_ALT;
    bool shift = ev->modifiers & OS_MOD_SHIFT;
    if (ctrl) {
        switch (ev->key) {
        case KEY_X: cut_or_copy(true); return;
        case KEY_C: cut_or_copy(false); return;
        case KEY_V: paste(); return;
        case KEY_A: select_all(); return;
        case KEY_H:
            show_hidden = !show_hidden;
            load(f.path);
            return;
        }
    }
    int next = -2;
    switch (ev->key) {
    case OS_KEY_DOWN:  next = f.focus + 1 < f.count ? f.focus + 1 : f.count - 1; break;
    case OS_KEY_UP:    next = f.focus > 0 ? f.focus - 1 : (f.count ? 0 : -1); break;
    case OS_KEY_HOME:  next = f.count ? 0 : -1; break;
    case OS_KEY_END:   next = f.count - 1; break;
    case OS_KEY_ENTER: open_entry(single_selected() >= 0 ? single_selected() : f.focus); return;
    case OS_KEY_BACKSPACE: go_up(); return;
    case OS_KEY_LEFT:
        if (alt) {
            go_back();
        }
        return;
    case OS_KEY_RIGHT:
        if (alt) {
            go_forward();
        }
        return;
    case OS_KEY_DELETE: dialog_open(DIALOG_DELETE); return;
    case KEY_F2:        dialog_open(DIALOG_RENAME); return;
    }
    if (next >= -1 && next < f.count) {
        if (shift && next >= 0) {
            select_range(f.focus, next, false);
        } else {
            select_only(next);
        }
    }
}

static void load_icons(void) {
    for (int i = 0; i < FILE_ICON_COUNT; i++) {
        struct gfx_surface *img = image_load_png(file_icon_path((enum file_icon)i), NULL);
        if (img) {
            f.icons[i] = gfx_surface_scaled(img, ICON, ICON);
            f.big_icons[i] = gfx_surface_scaled(img, BIG_ICON, BIG_ICON);
            if (i == FILE_ICON_FOLDER) {
                f.side_icon = gfx_surface_scaled(img, SIDE_ICON, SIDE_ICON);
            } else if (i == FILE_ICON_INTERNAL_DRIVE || i == FILE_ICON_EXTERNAL_DRIVE) {
                f.side_drive[i == FILE_ICON_EXTERNAL_DRIVE] = gfx_surface_scaled(img, SIDE_ICON, SIDE_ICON);
            }
            gfx_surface_destroy(img);
        }
    }
}

int main(int argc, char **argv) {
    resolve_places();
    ensure_home_folders();
    f.g = gui_open();
    if (!f.g) {
        return 1;
    }
    f.regular = gui_font(f.g, GUI_FONT_REGULAR);
    f.bold = gui_font(f.g, GUI_FONT_BOLD);
    if (!f.regular || !f.bold) {
        return 1;
    }
    load_icons();
    f.w = gui_window_create(f.g, "Files", -1, -1, 820, 480);
    if (!f.w) {
        return 1;
    }
    f.hover_button = f.pressed_button = -1;
    f.hover_place = -1;
    f.hover_eject = -1;
    f.last_click_index = -1;
    load_drives();
    if (argc < 2 || !load(argv[1])) {
        load(HOME_DIR);
    }
    draw();

    struct gui_event ev;
    for (;;) {
        if (!gui_wait_event(f.g, &ev, 1000)) {
            /* Nothing for a second: see whether the folder changed behind our back, and the drives. */
            bool redraw = load_drives();
            struct os_file_info here;
            if (stat(f.path, &here) < 0) {
                /* Gone (a drive that was pulled out, say): back home. */
                go(HOME_DIR);
                redraw = true;
            } else if (f.dialog.kind == DIALOG_NONE && !f.pressing && folder_signature(f.path) != f.signature) {
                refresh(NULL);
                redraw = true;
            }
            if (redraw) {
                draw();
            }
            continue;
        }
        int status;
        while (wait(-1, &status, WNOHANG) > 0) {
            /* collect apps we opened files in, once they close */
        }
        if (ev.type == GUI_EVENT_CLOSE || ev.type == GUI_EVENT_QUIT) {
            break;
        }
        if (ev.type == GUI_EVENT_RESIZE) {
            scroll_to(f.scroll);
            draw();
            continue;
        }
        if (f.dialog.kind != DIALOG_NONE) {
            /* The dialog has the window's attention until it's closed. */
            if (ev.type == GUI_EVENT_KEY) {
                dialog_key(&ev);
            } else if (ev.type == GUI_EVENT_MOUSE_DOWN || ev.type == GUI_EVENT_MOUSE_UP ||
                       ev.type == GUI_EVENT_MOUSE_MOVE) {
                int hover = f.dialog.hover_button;
                dialog_mouse(&ev);
                if (ev.type == GUI_EVENT_MOUSE_MOVE && hover == f.dialog.hover_button) {
                    continue;
                }
            } else {
                continue;
            }
            draw();
            continue;
        }
        switch (ev.type) {
        case GUI_EVENT_MOUSE_DOWN:
            on_mouse_down(&ev);
            break;
        case GUI_EVENT_MOUSE_UP:
            on_mouse_up(&ev);
            break;
        case GUI_EVENT_MOUSE_MOVE: {
            on_mouse_move(&ev);
            if (f.marquee) {
                break;
            }
            int hovered = entry_at(ev.x, ev.y), button = button_at(ev.x, ev.y), place = place_at(ev.x, ev.y);
            int ej = eject_at(ev.x, ev.y);
            gui_window_set_cursor(f.w, place >= 0 ? GUI_CURSOR_HAND : GUI_CURSOR_ARROW);   /* places are links */
            if (hovered == f.hovered && button == f.hover_button && place == f.hover_place && ej == f.hover_eject) {
                continue;   /* nothing to redraw */
            }
            f.hovered = hovered;
            f.hover_button = button;
            f.hover_place = place;
            f.hover_eject = ej;
            break;
        }
        case GUI_EVENT_SCROLL:
            scroll_to(f.scroll + ev.wheel * ROW_H * 2);
            break;
        case GUI_EVENT_KEY:
            on_key(&ev);
            break;
        case GUI_EVENT_MENU:
            on_menu(ev.menu_id);
            break;
        case GUI_EVENT_DROP:
            on_drop(&ev);
            break;
        default:
            continue;
        }
        draw();
    }
    gui_close(f.g);
    return 0;
}
