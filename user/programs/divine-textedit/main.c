/*
 * Text Editor: the window. A menu bar along the top (File, Edit, View),
 * an optional find / replace bar under it, the text with line numbers and
 * scroll bars, and a status bar.
 *
 *   Ctrl+N / O / S / Shift+S   new, open, save, save as
 *   Ctrl+Z / Y (Shift+Z)       undo, redo
 *   Ctrl+X / C / V / A         cut, copy, paste, select all
 *   Ctrl+F / H / G             find, replace, go to line
 *   Ctrl+= / - / 0             zoom in, out, back to 100%
 *   F3 / Shift+F3              next / previous match
 *   Shift + arrows, mouse drag select; double-click a word, triple-click a line
 *
 * Usage: textedit [file]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "editor.h"

struct editor ed;

#define TOOLBAR_H    34      /* the menu bar */
#define SCROLLBAR    12
#define FINDBAR_H    46
#define STATUS_H     28
#define TEXT_PAD     8
#define DEFAULT_SIZE 14.0f
#define MIN_SIZE     8.0f
#define MAX_SIZE     40.0f
#define DOUBLE_CLICK 450
#define WHEEL_LINES  3

#define CURRENT_LINE GFX_RGB(0xF5, 0xF7, 0xFB)
#define GUTTER       GFX_RGB(0xFA, 0xFA, 0xFC)
#define MATCH        GFX_RGB(0xFF, 0xE7, 0x8A)

/* Scan codes for Ctrl shortcuts. */
#define KEY_0      0x0B
#define KEY_MINUS  0x0C
#define KEY_EQUALS 0x0D
#define KEY_Y      0x15
#define KEY_O      0x18
#define KEY_A      0x1E
#define KEY_S      0x1F
#define KEY_F      0x21
#define KEY_G      0x22
#define KEY_H      0x23
#define KEY_Z      0x2C
#define KEY_X      0x2D
#define KEY_C      0x2E
#define KEY_V      0x2F
#define KEY_N      0x31
#define KEY_F3     0x3D
#define KEY_PGUP   0x149
#define KEY_PGDN   0x151

/* Actions (menu items), the find bar's controls, the menu bar's titles: one id space. */
enum control {
    T_NEW, T_OPEN, T_SAVE, T_SAVE_AS, T_EXIT, T_UNDO, T_REDO, T_CUT, T_COPY, T_PASTE, T_DELETE,
    T_SELECT_ALL, T_FIND, T_FIND_NEXT, T_REPLACE, T_GOTO, T_ZOOM_OUT, T_ZOOM, T_ZOOM_IN, T_LINE_NUMBERS,
    F_PREV = 100, F_NEXT, F_CASE, F_CLOSE, F_REPLACE, F_REPLACE_ALL,
    M_FILE = 200, M_EDIT, M_VIEW,
    C_NONE = -1,
};
#define MENU_BASE 1000   /* a menu item's id: MENU_BASE + its action */

static const char *const menu_titles[3] = { "File", "Edit", "View" };

/* Scroll bars: which one is being dragged, and where on its thumb it was grabbed. */
enum { BAR_NONE, BAR_VERTICAL, BAR_HORIZONTAL };
static int bar_drag, bar_hover;
static float bar_grab;

static bool line_numbers = true;
static bool quit;
static int mouse_x, mouse_y;   /* last pointer position in the window, for drag-scrolling */

/* --- the clipboard ------------------------------------------------------------------------------- */

bool clipboard_read(char **text, int *len) {
    int fd = open(TEXT_CLIPBOARD, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    size_t cap = 1024, n = 0;
    char *buf = malloc(cap + 1);
    long got;
    while (buf && (got = read(fd, buf + n, cap - n)) > 0) {
        n += (size_t)got;
        if (n == cap) {
            char *grown = realloc(buf, cap * 2 + 1);
            if (!grown) {
                break;
            }
            buf = grown;
            cap *= 2;
        }
    }
    close(fd);
    if (!buf || n == 0) {
        free(buf);
        return false;
    }
    buf[n] = '\0';
    *text = buf;
    *len = (int)n;
    return true;
}

void clipboard_write(const char *text, int len) {
    int fd = open(TEXT_CLIPBOARD, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        write(fd, text, (size_t)len);
        close(fd);
    }
}

static bool clipboard_has_text(void) {
    struct os_file_info info;
    return stat(TEXT_CLIPBOARD, &info) == 0 && info.size > 0;
}

/* --- layout ---------------------------------------------------------------------------------------- */

static struct gfx_surface *surface(void) {
    return gui_window_surface(ed.w);
}

static int findbar_height(void) {
    return ed.find_open ? FINDBAR_H * (ed.replace_open ? 2 : 1) : 0;
}

/* The text area, gutter included. */
static struct gfx_rect text_area(void) {
    struct gfx_surface *s = surface();
    int top = TOOLBAR_H + findbar_height();
    return gfx_rect(0, top, s->width, s->height - top - STATUS_H);
}

static int gutter_width(void) {
    if (!line_numbers) {
        return 0;
    }
    int digits = 1;
    for (int n = ed.count; n >= 10; n /= 10) {
        digits++;
    }
    digits = digits < 3 ? 3 : digits;
    return (int)(digits * ed.cell_w) + 2 * TEXT_PAD + 4;
}

static float text_left(void) {
    return text_area().x + gutter_width() + TEXT_PAD;
}

static float max_scroll_x(void);

static int visible_lines(void) {
    int h = text_area().h - TEXT_PAD - (max_scroll_x() > 0 ? SCROLLBAR : 0);
    int n = (int)(h / ed.cell_h);
    return n < 1 ? 1 : n;
}

/* --- scroll bars ----------------------------------------------------------------------------------- */

static int longest_line_cells(void) {
    int longest = 0;
    for (int i = 0; i < ed.count; i++) {
        int cells = col_to_cells(i, ed.lines[i].len);
        longest = cells > longest ? cells : longest;
    }
    return longest;
}

static float text_width(void) {
    return text_area().w - gutter_width() - TEXT_PAD - SCROLLBAR;
}

static float max_scroll_x(void) {
    float m = (longest_line_cells() + 4) * ed.cell_w - text_width();
    return m > 0 ? m : 0;
}

static bool vbar_visible(void) {
    return ed.count > visible_lines();
}

static struct gfx_rect vbar_track(void) {
    struct gfx_rect a = text_area();
    return gfx_rect(a.x + a.w - SCROLLBAR, a.y, SCROLLBAR, a.h - (max_scroll_x() > 0 ? SCROLLBAR : 0));
}

static struct gfx_rect hbar_track(void) {
    struct gfx_rect a = text_area();
    int left = a.x + gutter_width();
    return gfx_rect(left, a.y + a.h - SCROLLBAR, a.w - gutter_width() - (vbar_visible() ? SCROLLBAR : 0), SCROLLBAR);
}

/* The thumb: as long as the visible part, as far along as the scroll position. */
static struct gfx_rect bar_thumb(int which) {
    if (which == BAR_VERTICAL) {
        struct gfx_rect t = vbar_track();
        int vis = visible_lines(), max = ed.count - vis;
        int len = t.h * vis / (ed.count ? ed.count : 1);
        len = len < 28 ? 28 : len > t.h ? t.h : len;
        int y = t.y + (max > 0 ? (t.h - len) * ed.scroll_line / max : 0);
        return gfx_rect(t.x, y, t.w, len);
    }
    struct gfx_rect t = hbar_track();
    float max = max_scroll_x(), total = max + text_width();
    int len = (int)(t.w * text_width() / total);
    len = len < 28 ? 28 : len > t.w ? t.w : len;
    int x = t.x + (max > 0 ? (int)((t.w - len) * ed.scroll_x / max) : 0);
    return gfx_rect(x, t.y, len, t.h);
}

static int bar_at(int x, int y) {
    if (vbar_visible() && gfx_rect_contains(vbar_track(), x, y)) {
        return BAR_VERTICAL;
    }
    if (max_scroll_x() > 0 && gfx_rect_contains(hbar_track(), x, y)) {
        return BAR_HORIZONTAL;
    }
    return BAR_NONE;
}

static void metrics(void) {
    struct ttf_metrics m;
    ttf_get_metrics(ed.mono, ed.font_size, &m);
    ed.cell_w = ttf_text_width(ed.mono, ed.font_size, "M");
    ed.cell_h = (float)(int)(m.line_height * 1.1f + 0.99f);
    ed.ascent = m.ascent + (ed.cell_h - (m.ascent + m.descent)) / 2;
}

static struct gfx_rect menu_title_rect(int i) {
    int x = 6;
    for (int k = 0; k < i; k++) {
        x += (int)ttf_text_width(ed.ui, UI_SIZE, menu_titles[k]) + 22;
    }
    return gfx_rect(x, 3, (int)ttf_text_width(ed.ui, UI_SIZE, menu_titles[i]) + 20, TOOLBAR_H - 6);
}

static struct gfx_rect find_field_rect(int row) {
    return gfx_rect(12, TOOLBAR_H + row * FINDBAR_H + 7, 260, 32);
}

static struct gfx_rect find_control_rect(int c) {
    int y0 = TOOLBAR_H + 7, y1 = TOOLBAR_H + FINDBAR_H + 7;
    int x = 12 + 260 + 10;
    switch (c) {
    case F_PREV:        return gfx_rect(x, y0, 84, 32);
    case F_NEXT:        return gfx_rect(x + 90, y0, 64, 32);
    case F_CASE:        return gfx_rect(x + 166, y0, 112, 32);
    case F_CLOSE:       return gfx_rect(surface()->width - 44, y0, 32, 32);
    case F_REPLACE:     return gfx_rect(x, y1, 84, 32);
    case F_REPLACE_ALL: return gfx_rect(x + 90, y1, 100, 32);
    default:            return gfx_rect(0, 0, 0, 0);
    }
}

/* --- selection and cursor ---------------------------------------------------------------------------- */

static bool has_selection(void) {
    return pos_cmp(ed.cursor, ed.anchor) != 0;
}

static void selection(struct pos *a, struct pos *b) {
    bool forward = pos_cmp(ed.anchor, ed.cursor) <= 0;
    *a = forward ? ed.anchor : ed.cursor;
    *b = forward ? ed.cursor : ed.anchor;
}

static void ensure_visible(void) {
    int vis = visible_lines();
    if (ed.cursor.line < ed.scroll_line) {
        ed.scroll_line = ed.cursor.line;
    } else if (ed.cursor.line >= ed.scroll_line + vis) {
        ed.scroll_line = ed.cursor.line - vis + 1;
    }
    float x = col_to_cells(ed.cursor.line, ed.cursor.col) * ed.cell_w;
    float width = surface()->width - text_left() - TEXT_PAD;
    if (x < ed.scroll_x) {
        ed.scroll_x = x - 8 * ed.cell_w;
    } else if (x > ed.scroll_x + width - ed.cell_w) {
        ed.scroll_x = x - width + 8 * ed.cell_w;
    }
    ed.scroll_x = ed.scroll_x < 0 ? 0 : ed.scroll_x;
}

static void clamp_scroll(void) {
    int max = ed.count - visible_lines();
    max = max < 0 ? 0 : max;
    ed.scroll_line = ed.scroll_line > max ? max : ed.scroll_line;
    ed.scroll_line = ed.scroll_line < 0 ? 0 : ed.scroll_line;
}

/* Put the cursor somewhere; extend keeps the selection's other end. */
static void move_to(struct pos p, bool extend) {
    ed.cursor = pos_clamp(p);
    if (!extend) {
        ed.anchor = ed.cursor;
    }
    ed.goal_cells = col_to_cells(ed.cursor.line, ed.cursor.col);
    undo_break();
    ensure_visible();
}

static bool is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           (unsigned char)c >= 0x80;
}

static char char_at(struct pos p) {
    const struct line *l = &ed.lines[p.line];
    return p.col < l->len ? l->text[p.col] : '\n';
}

static struct pos word_left(struct pos p) {
    p = pos_prev(p);
    while ((p.line || p.col) && !is_word(char_at(p))) {
        p = pos_prev(p);
    }
    while (p.col > 0 && is_word(ed.lines[p.line].text[p.col - 1])) {
        p.col--;
    }
    return p;
}

static struct pos word_right(struct pos p) {
    struct pos end = doc_end();
    while (pos_cmp(p, end) < 0 && !is_word(char_at(p))) {
        p = pos_next(p);
    }
    while (pos_cmp(p, end) < 0 && is_word(char_at(p)) && p.col < ed.lines[p.line].len) {
        p = pos_next(p);
    }
    return p;
}

static void select_word_at(struct pos p) {
    const struct line *l = &ed.lines[p.line];
    int a = p.col, b = p.col;
    if (a < l->len && is_word(l->text[a])) {
        while (a > 0 && is_word(l->text[a - 1])) {
            a--;
        }
        while (b < l->len && is_word(l->text[b])) {
            b++;
        }
    } else if (a < l->len) {
        b = a + 1;
    }
    ed.anchor = (struct pos){ p.line, a };
    ed.cursor = (struct pos){ p.line, b };
}

static void select_line_at(int line) {
    ed.anchor = (struct pos){ line, 0 };
    ed.cursor = line + 1 < ed.count ? (struct pos){ line + 1, 0 } : (struct pos){ line, ed.lines[line].len };
}

/* --- editing ------------------------------------------------------------------------------------------ */

static void delete_selection(void) {
    struct pos a, b;
    selection(&a, &b);
    doc_delete(a, b);
    ed.cursor = ed.anchor = a;
}

/* Type or paste: replaces the selection, as one undo step. */
static void insert_text(const char *text, int len) {
    bool grouped = has_selection() || len > 1;
    if (grouped) {
        undo_begin_group();
    }
    if (has_selection()) {
        delete_selection();
    }
    struct pos end = doc_insert(ed.cursor, text, len);
    if (grouped) {
        undo_end_group();
    }
    ed.cursor = ed.anchor = end;
    ed.goal_cells = col_to_cells(end.line, end.col);
    ensure_visible();
}

static void do_copy(bool cut) {
    if (!has_selection()) {
        return;
    }
    struct pos a, b;
    selection(&a, &b);
    int len;
    char *text = doc_get(a, b, &len);
    if (text) {
        clipboard_write(text, len);
        free(text);
    }
    if (cut) {
        undo_break();
        delete_selection();
        undo_break();
        ensure_visible();
    }
}

static void do_paste(void) {
    char *text;
    int len;
    if (!clipboard_read(&text, &len)) {
        return;
    }
    /* Clean it the way files are cleaned when opened: no CR, tabs as spaces. */
    int n = 0;
    for (int i = 0; i < len; i++) {
        if (text[i] != '\r') {
            text[n++] = text[i] == '\t' ? ' ' : text[i];
        }
    }
    undo_break();
    insert_text(text, n);
    undo_break();
    free(text);
}

static void do_backspace(bool ctrl) {
    if (has_selection()) {
        undo_break();
        delete_selection();
        undo_break();
    } else {
        struct pos from = ctrl ? word_left(ed.cursor) : pos_prev(ed.cursor);
        /* In leading spaces, step back to the previous tab stop. */
        const struct line *l = &ed.lines[ed.cursor.line];
        if (!ctrl && ed.cursor.col > 0 && ed.cursor.col % TAB_WIDTH == 0) {
            bool spaces = true;
            for (int i = 0; i < ed.cursor.col; i++) {
                spaces &= l->text[i] == ' ';
            }
            if (spaces) {
                from = (struct pos){ ed.cursor.line, ed.cursor.col - TAB_WIDTH };
            }
        }
        if (ctrl || pos_cmp(from, pos_prev(ed.cursor)) != 0) {
            undo_break();
        }
        doc_delete(from, ed.cursor);
        ed.cursor = ed.anchor = from;
    }
    ed.goal_cells = col_to_cells(ed.cursor.line, ed.cursor.col);
    ensure_visible();
}

static void do_delete_key(bool ctrl) {
    if (has_selection()) {
        undo_break();
        delete_selection();
        undo_break();
    } else {
        undo_break();
        doc_delete(ed.cursor, ctrl ? word_right(ed.cursor) : pos_next(ed.cursor));
        undo_break();
    }
    ensure_visible();
}

static void do_enter(void) {
    /* Keep the indentation of the line we're on. */
    const struct line *l = &ed.lines[ed.cursor.line];
    int indent = 0;
    while (indent < l->len && indent < ed.cursor.col && l->text[indent] == ' ' && indent < 120) {
        indent++;
    }
    char text[128];
    text[0] = '\n';
    memset(text + 1, ' ', (size_t)indent);
    undo_break();
    insert_text(text, indent + 1);
    undo_break();
}

/* Tab: spaces to the next stop, or with several lines selected, indent (Shift: unindent) them. */
static void do_tab(bool shift) {
    struct pos a, b;
    selection(&a, &b);
    if (a.line == b.line && !shift) {
        int cells = col_to_cells(ed.cursor.line, ed.cursor.col);
        int n = TAB_WIDTH - cells % TAB_WIDTH;
        undo_break();
        insert_text("        ", n);
        undo_break();
        return;
    }
    int last = b.line - (b.col == 0 && b.line > a.line);
    undo_begin_group();
    for (int i = a.line; i <= last; i++) {
        if (shift) {
            int n = 0;
            while (n < TAB_WIDTH && n < ed.lines[i].len && ed.lines[i].text[n] == ' ') {
                n++;
            }
            doc_delete((struct pos){ i, 0 }, (struct pos){ i, n });
        } else if (ed.lines[i].len) {
            doc_insert((struct pos){ i, 0 }, "    ", TAB_WIDTH);
        }
    }
    undo_end_group();
    ed.anchor = (struct pos){ a.line, 0 };
    ed.cursor = pos_clamp((struct pos){ last, ed.lines[last].len });
}

static void select_all(void) {
    ed.anchor = (struct pos){ 0, 0 };
    ed.cursor = doc_end();
    ensure_visible();
}

static void zoom(float step) {
    float size = step == 0 ? DEFAULT_SIZE : ed.font_size + step;
    ed.font_size = size < MIN_SIZE ? MIN_SIZE : size > MAX_SIZE ? MAX_SIZE : size;
    metrics();
    clamp_scroll();
    ensure_visible();
}

void editor_goto_line(int line) {
    if (line < 1) {
        line = 1;
    }
    if (line > ed.count) {
        line = ed.count;
    }
    move_to((struct pos){ line - 1, 0 }, false);
    /* Put it near the middle of the view, not the bottom edge. */
    ed.scroll_line = line - 1 - visible_lines() / 3;
    clamp_scroll();
}

/* --- find and replace ----------------------------------------------------------------------------------- */

static bool match_at(const struct line *l, int col) {
    const char *q = ed.find.text;
    int n = ed.find.len;
    if (!n || col + n > l->len) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        char a = l->text[col + i], b = q[i];
        if (!ed.match_case) {
            a = (char)(a >= 'A' && a <= 'Z' ? a + 32 : a);
            b = (char)(b >= 'A' && b <= 'Z' ? b + 32 : b);
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

/* Is the selection exactly a match? */
static bool selection_is_match(void) {
    struct pos a, b;
    selection(&a, &b);
    return a.line == b.line && b.col - a.col == ed.find.len && match_at(&ed.lines[a.line], a.col);
}

static int count_matches(int *current) {
    int total = 0;
    *current = 0;
    struct pos a, b;
    selection(&a, &b);
    for (int i = 0; i < ed.count && ed.find.len; i++) {
        for (int c = 0; c + ed.find.len <= ed.lines[i].len;) {
            if (match_at(&ed.lines[i], c)) {
                total++;
                if (i == a.line && c == a.col && selection_is_match()) {
                    *current = total;
                }
                c += ed.find.len;
            } else {
                c++;
            }
        }
    }
    return total;
}

/* Select the next (or previous) match, wrapping around. `from_start`: search from the selection's start. */
static bool find(bool forward, bool from_start) {
    if (!ed.find.len) {
        return false;
    }
    struct pos a, b;
    selection(&a, &b);
    struct pos from = forward ? (from_start ? a : b) : a;
    for (int pass = 0; pass <= ed.count; pass++) {
        int line = forward ? (from.line + pass) % ed.count : ((from.line - pass) % ed.count + ed.count) % ed.count;
        const struct line *l = &ed.lines[line];
        if (forward) {
            int start = pass == 0 ? from.col : 0;
            for (int c = start; c + ed.find.len <= l->len; c++) {
                if (match_at(l, c)) {
                    ed.anchor = (struct pos){ line, c };
                    ed.cursor = (struct pos){ line, c + ed.find.len };
                    ensure_visible();
                    return true;
                }
            }
        } else {
            int start = pass == 0 ? from.col - 1 : l->len - ed.find.len;
            for (int c = start; c >= 0; c--) {
                if (match_at(l, c)) {
                    ed.anchor = (struct pos){ line, c + ed.find.len };
                    ed.cursor = (struct pos){ line, c };
                    ensure_visible();
                    return true;
                }
            }
        }
    }
    /* The wrap-around also checks the start line's other half. */
    if (forward) {
        for (int c = 0; c < from.col && c + ed.find.len <= ed.lines[from.line].len; c++) {
            if (match_at(&ed.lines[from.line], c)) {
                ed.anchor = (struct pos){ from.line, c };
                ed.cursor = (struct pos){ from.line, c + ed.find.len };
                ensure_visible();
                return true;
            }
        }
    }
    snprintf(ed.status, sizeof(ed.status), "\"%s\" not found", ed.find.text);
    return false;
}

static void open_find(bool replace) {
    ed.find_open = true;
    ed.replace_open = replace;
    ed.focus = FOCUS_FIND;
    /* A short selection on one line becomes the search text. */
    struct pos a, b;
    selection(&a, &b);
    if (has_selection() && a.line == b.line && b.col - a.col < (int)sizeof(ed.find.text) - 1) {
        char text[sizeof(ed.find.text)];
        memcpy(text, ed.lines[a.line].text + a.col, (size_t)(b.col - a.col));
        text[b.col - a.col] = '\0';
        field_set(&ed.find, text);
    } else {
        field_set(&ed.find, ed.find.text);
    }
    clamp_scroll();
    ensure_visible();
}

static void close_find(void) {
    ed.find_open = ed.replace_open = false;
    ed.focus = FOCUS_TEXT;
    clamp_scroll();
}

static void replace_one(void) {
    if (selection_is_match()) {
        undo_break();
        insert_text(ed.replace.text, ed.replace.len);
        undo_break();
    }
    find(true, false);
}

static void replace_all(void) {
    if (!ed.find.len) {
        return;
    }
    int n = 0;
    undo_begin_group();
    for (int i = 0; i < ed.count; i++) {
        for (int c = 0; c + ed.find.len <= ed.lines[i].len;) {
            if (match_at(&ed.lines[i], c)) {
                doc_delete((struct pos){ i, c }, (struct pos){ i, c + ed.find.len });
                doc_insert((struct pos){ i, c }, ed.replace.text, ed.replace.len);
                c += ed.replace.len;
                n++;
            } else {
                c++;
            }
        }
    }
    undo_end_group();
    ed.cursor = ed.anchor = pos_clamp(ed.cursor);
    snprintf(ed.status, sizeof(ed.status), "Replaced %d match%s", n, n == 1 ? "" : "es");
}

/* --- files ------------------------------------------------------------------------------------------------ */

static const char *file_name(void) {
    const char *slash = strrchr(ed.path, '/');
    return ed.path[0] ? (slash ? slash + 1 : ed.path) : "Untitled";
}

void update_title(void) {
    static char shown[96];
    char title[96];
    snprintf(title, sizeof(title), "%s%s - Text Editor", ed.modified ? "*" : "", file_name());
    if (strcmp(title, shown) != 0) {   /* only tell the desktop when it changes */
        strlcpy(shown, title, sizeof(shown));
        gui_window_set_title(ed.w, title);
    }
}

void editor_open_path(const char *path) {
    int err = doc_load(path);
    if (err < 0) {
        char message[200];
        snprintf(message, sizeof(message), "%s: %s", path, strerror(err));
        dialog_message("Can't open the file", message);
        return;
    }
    strlcpy(ed.path, path, sizeof(ed.path));
    snprintf(ed.status, sizeof(ed.status), "Opened %s", file_name());
    update_title();
}

bool editor_save_to(const char *path) {
    int err = doc_save(path);
    if (err < 0) {
        char message[200];
        snprintf(message, sizeof(message), "%s: %s", path, strerror(err));
        dialog_message("Can't save the file", message);
        ed.pending = PENDING_NONE;
        return false;
    }
    strlcpy(ed.path, path, sizeof(ed.path));
    snprintf(ed.status, sizeof(ed.status), "Saved %s", file_name());
    update_title();
    return true;
}

static void new_now(void) {
    doc_reset();
    ed.path[0] = '\0';
    ed.status[0] = '\0';
    update_title();
}

void editor_after_pending(void) {
    enum pending p = ed.pending;
    ed.pending = PENDING_NONE;
    switch (p) {
    case PENDING_NEW:  new_now(); break;
    case PENDING_OPEN: dialog_open_file(DIALOG_OPEN); break;
    case PENDING_QUIT: quit = true; break;
    default:           break;
    }
}

/* Do `then` now, or first ask about unsaved changes. */
static void unless_unsaved(enum pending then) {
    if (ed.modified) {
        dialog_save_changes(then);
    } else {
        ed.pending = then;
        editor_after_pending();
    }
}

void editor_new(void) {
    unless_unsaved(PENDING_NEW);
}

static void save(void) {
    if (ed.path[0]) {
        editor_save_to(ed.path);
    } else {
        dialog_open_file(DIALOG_SAVE_AS);
    }
}

/* --- drawing ---------------------------------------------------------------------------------------------- */

static bool control_enabled(int c) {
    switch (c) {
    case T_UNDO:     return ed.undo_len > 0;
    case T_REDO:     return ed.redo_len > 0;
    case T_CUT:
    case T_COPY:
    case T_DELETE:   return has_selection();
    case T_PASTE:    return clipboard_has_text();
    case T_ZOOM_OUT: return ed.font_size > MIN_SIZE;
    case T_ZOOM_IN:  return ed.font_size < MAX_SIZE;
    default:         return true;
    }
}

static void draw_button(struct gfx_surface *s, struct gfx_rect r, int id, const char *label, bool on,
                        bool active) {
    bool enabled = control_enabled(id);
    if (enabled && ed.pressed_button == id && ed.hover_button == id) {
        gfx_fill_rounded_rect(s, r, 7, GFX_RGBA(0, 0, 0, 30));
    } else if ((enabled && ed.hover_button == id) || active) {
        gfx_fill_rounded_rect(s, r, 7, GFX_RGBA(0, 0, 0, 15));
    } else if (on) {
        gfx_fill_rounded_rect(s, r, 7, FIELD);
    }
    float w = ttf_text_width(ed.ui, UI_SIZE, label);
    ttf_draw_text(s, ed.ui, UI_SIZE, r.x + (r.w - w) / 2, r.y + r.h / 2.0f + 4.5f,
                  enabled ? INK : INK_FAINT, label);
}

static void draw_toolbar(struct gfx_surface *s) {
    gfx_fill_rect(s, gfx_rect(0, 0, s->width, TOOLBAR_H), WHITE);
    for (int i = 0; i < 3; i++) {
        draw_button(s, menu_title_rect(i), M_FILE + i, menu_titles[i], false, false);
    }
    gfx_fill_rect(s, gfx_rect(0, TOOLBAR_H - 1, s->width, 1), LINE_COLOR);
}

static void draw_findbar(struct gfx_surface *s) {
    if (!ed.find_open) {
        return;
    }
    int h = findbar_height();
    gfx_fill_rect(s, gfx_rect(0, TOOLBAR_H, s->width, h), GFX_RGB(0xFB, 0xFB, 0xFD));
    field_draw(s, &ed.find, find_field_rect(0), ed.focus == FOCUS_FIND && ed.focused, "Find");
    draw_button(s, find_control_rect(F_PREV), F_PREV, "Previous", true, false);
    draw_button(s, find_control_rect(F_NEXT), F_NEXT, "Next", true, false);

    /* Match case: a check box. */
    struct gfx_rect c = find_control_rect(F_CASE);
    if (ed.hover_button == F_CASE) {
        gfx_fill_rounded_rect(s, c, 7, GFX_RGBA(0, 0, 0, 15));
    }
    struct gfx_rect box = gfx_rect(c.x + 8, c.y + 8, 16, 16);
    if (ed.match_case) {
        gfx_fill_rounded_rect(s, box, 4, ACCENT);
        gfx_draw_line(s, box.x + 4, box.y + 8.5f, box.x + 7, box.y + 11.5f, 1.8f, WHITE);
        gfx_draw_line(s, box.x + 7, box.y + 11.5f, box.x + 12.5f, box.y + 5, 1.8f, WHITE);
    } else {
        gfx_stroke_rounded_rect(s, box, 4, 1.3f, INK_SOFT);
    }
    ttf_draw_text(s, ed.ui, UI_SIZE, c.x + 32, c.y + 20.5f, INK, "Match case");

    /* "3 of 12" */
    int current, total = count_matches(&current);
    char text[48] = "";
    if (ed.find.len) {
        if (!total) {
            strlcpy(text, "No results", sizeof(text));
        } else if (current) {
            snprintf(text, sizeof(text), "%d of %d", current, total);
        } else {
            snprintf(text, sizeof(text), "%d result%s", total, total == 1 ? "" : "s");
        }
    }
    struct gfx_rect close = find_control_rect(F_CLOSE);
    float w = ttf_text_width(ed.ui, UI_SIZE, text);
    if (c.x + c.w + 16 + w < close.x - 10) {
        ttf_draw_text(s, ed.ui, UI_SIZE, close.x - 14 - w, c.y + 20.5f, total ? INK_SOFT : DANGER, text);
    }

    if (ed.hover_button == F_CLOSE) {
        gfx_fill_rounded_rect(s, close, 7, GFX_RGBA(0, 0, 0, 15));
    }
    float cx = close.x + 16, cy = close.y + 16;
    gfx_draw_line(s, cx - 4.5f, cy - 4.5f, cx + 4.5f, cy + 4.5f, 1.5f, INK_SOFT);
    gfx_draw_line(s, cx - 4.5f, cy + 4.5f, cx + 4.5f, cy - 4.5f, 1.5f, INK_SOFT);

    if (ed.replace_open) {
        field_draw(s, &ed.replace, find_field_rect(1), ed.focus == FOCUS_REPLACE && ed.focused, "Replace with");
        draw_button(s, find_control_rect(F_REPLACE), F_REPLACE, "Replace", true, false);
        draw_button(s, find_control_rect(F_REPLACE_ALL), F_REPLACE_ALL, "Replace all", true, false);
    }
    gfx_fill_rect(s, gfx_rect(0, TOOLBAR_H + h - 1, s->width, 1), LINE_COLOR);
}

static void draw_text(struct gfx_surface *s) {
    struct gfx_rect area = text_area();
    int gutter = gutter_width();
    float left = text_left();
    gfx_fill_rect(s, area, WHITE);
    gfx_fill_rect(s, gfx_rect(area.x, area.y, gutter, area.h), GUTTER);
    gfx_fill_rect(s, gfx_rect(area.x + gutter, area.y, 1, area.h), LINE_COLOR);

    struct pos a, b;
    selection(&a, &b);
    bool sel = has_selection();
    struct gfx_rect text_clip = gfx_rect(area.x + gutter + 1, area.y, area.w - gutter - 1 - SCROLLBAR, area.h);
    int vis = visible_lines() + 1;
    for (int i = 0; i < vis && ed.scroll_line + i < ed.count; i++) {
        int n = ed.scroll_line + i;
        const struct line *l = &ed.lines[n];
        float top = area.y + TEXT_PAD / 2 + i * ed.cell_h;
        float baseline = top + ed.ascent;

        if (line_numbers) {
            gfx_set_clip(s, gfx_rect(area.x, area.y, gutter, area.h));
            char num[16];
            snprintf(num, sizeof(num), "%d", n + 1);
            float nw = ttf_text_width(ed.mono, ed.font_size, num);
            ttf_draw_text(s, ed.mono, ed.font_size, area.x + gutter - TEXT_PAD - nw, baseline,
                          n == ed.cursor.line ? INK_SOFT : INK_FAINT, num);
        }

        gfx_set_clip(s, text_clip);
        if (!sel && n == ed.cursor.line) {
            gfx_fill_rect(s, gfx_rect(text_clip.x, (int)top, text_clip.w, (int)ed.cell_h), CURRENT_LINE);
        }
        /* Search matches. */
        if (ed.find_open && ed.find.len) {
            for (int c = 0; c + ed.find.len <= l->len;) {
                if (match_at(l, c)) {
                    float x0 = left - ed.scroll_x + col_to_cells(n, c) * ed.cell_w;
                    float x1 = left - ed.scroll_x + col_to_cells(n, c + ed.find.len) * ed.cell_w;
                    gfx_fill_rect(s, gfx_rect((int)x0, (int)top, (int)(x1 - x0), (int)ed.cell_h), MATCH);
                    c += ed.find.len;
                } else {
                    c++;
                }
            }
        }
        /* The selection on this line (a little past the end when the line break is included). */
        if (sel && n >= a.line && n <= b.line) {
            int from = n == a.line ? col_to_cells(n, a.col) : 0;
            int to = n == b.line ? col_to_cells(n, b.col) : col_to_cells(n, l->len) + 1;
            float x0 = left - ed.scroll_x + from * ed.cell_w, x1 = left - ed.scroll_x + to * ed.cell_w;
            gfx_fill_rect(s, gfx_rect((int)x0, (int)top, (int)(x1 - x0 + 0.5f), (int)ed.cell_h), SELECTION);
        }
        ttf_draw_text(s, ed.mono, ed.font_size, left - ed.scroll_x, baseline, INK, l->text);
        if (n == ed.cursor.line && ed.focused && ed.focus == FOCUS_TEXT && ed.dialog.kind == DIALOG_NONE) {
            float x = left - ed.scroll_x + col_to_cells(n, ed.cursor.col) * ed.cell_w;
            gfx_fill_rect(s, gfx_rect((int)x, (int)top + 1, 2, (int)ed.cell_h - 2), INK);
        }
    }
    gfx_reset_clip(s);

    /* Scroll bars, when it doesn't all fit: a light track and a thumb to drag. */
    for (int which = BAR_VERTICAL; which <= BAR_HORIZONTAL; which++) {
        bool shown = which == BAR_VERTICAL ? vbar_visible() : max_scroll_x() > 0;
        if (!shown) {
            continue;
        }
        struct gfx_rect track = which == BAR_VERTICAL ? vbar_track() : hbar_track();
        gfx_fill_rect(s, track, GFX_RGB(0xFA, 0xFA, 0xFC));
        struct gfx_rect thumb = gfx_rect_inset(bar_thumb(which), 3);
        bool active = bar_drag == which || bar_hover == which;
        gfx_fill_rounded_rect(s, thumb, 3, active ? GFX_RGBA(0, 0, 0, 120) : GFX_RGBA(0, 0, 0, 60));
    }
}

static void draw_status(struct gfx_surface *s) {
    int y = s->height - STATUS_H;
    gfx_fill_rect(s, gfx_rect(0, y, s->width, STATUS_H), WHITE);
    gfx_fill_rect(s, gfx_rect(0, y, s->width, 1), LINE_COLOR);
    char left[96];
    int line = ed.cursor.line + 1, col = col_to_cells(ed.cursor.line, ed.cursor.col) + 1;
    if (has_selection()) {
        struct pos a, b;
        selection(&a, &b);
        int len;
        char *text = doc_get(a, b, &len);
        int chars = 0;
        for (int i = 0; text && i < len; i++) {
            chars += ((unsigned char)text[i] & 0xC0) != 0x80;
        }
        free(text);
        snprintf(left, sizeof(left), "Ln %d, Col %d   (%d selected)", line, col, chars);
    } else {
        snprintf(left, sizeof(left), "Ln %d, Col %d", line, col);
    }
    ttf_draw_text(s, ed.ui, 12, 14, y + 18, INK_SOFT, left);
    ttf_draw_text(s, ed.ui, 12, 240, y + 18, INK_SOFT, ed.status);
    char right[64];
    snprintf(right, sizeof(right), "%d line%s   UTF-8   %d%%", ed.count, ed.count == 1 ? "" : "s",
             (int)(ed.font_size * 100 / DEFAULT_SIZE + 0.5f));
    float w = ttf_text_width(ed.ui, 12, right);
    ttf_draw_text(s, ed.ui, 12, s->width - 14 - w, y + 18, INK_SOFT, right);
}

static void draw(void) {
    struct gfx_surface *s = surface();
    draw_text(s);
    draw_toolbar(s);
    draw_findbar(s);
    draw_status(s);
    if (ed.dialog.kind != DIALOG_NONE) {
        dialog_draw(s);
    }
    gui_window_invalidate(ed.w, NULL);
}

/* --- actions ---------------------------------------------------------------------------------------------- */

static void act(int c) {
    if (!control_enabled(c)) {
        return;
    }
    switch (c) {
    case T_NEW:      editor_new(); break;
    case T_OPEN:     unless_unsaved(PENDING_OPEN); break;
    case T_SAVE:     save(); break;
    case T_SAVE_AS:  dialog_open_file(DIALOG_SAVE_AS); break;
    case T_EXIT:     unless_unsaved(PENDING_QUIT); break;
    case T_DELETE:   do_delete_key(false); break;
    case T_SELECT_ALL: select_all(); break;
    case T_FIND_NEXT:
        if (!ed.find_open) {
            open_find(false);
        }
        find(true, false);
        break;
    case T_LINE_NUMBERS:
        line_numbers = !line_numbers;
        ensure_visible();
        break;
    case T_UNDO:     doc_undo(); ensure_visible(); break;
    case T_REDO:     doc_redo(); ensure_visible(); break;
    case T_CUT:      do_copy(true); break;
    case T_COPY:     do_copy(false); break;
    case T_PASTE:    do_paste(); break;
    case T_FIND:     open_find(false); break;
    case T_REPLACE:  open_find(true); break;
    case T_GOTO:     dialog_goto(); break;
    case T_ZOOM_OUT: zoom(-2); break;
    case T_ZOOM:     zoom(0); break;
    case T_ZOOM_IN:  zoom(2); break;
    case F_PREV:     find(false, false); break;
    case F_NEXT:     find(true, false); break;
    case F_CASE:     ed.match_case = !ed.match_case; break;
    case F_CLOSE:    close_find(); break;
    case F_REPLACE:  replace_one(); break;
    case F_REPLACE_ALL: replace_all(); break;
    }
}

static int control_at(int x, int y) {
    for (int i = 0; i < 3; i++) {
        if (gfx_rect_contains(menu_title_rect(i), x, y)) {
            return M_FILE + i;
        }
    }
    if (ed.find_open) {
        for (int c = F_PREV; c <= F_REPLACE_ALL; c++) {
            if ((c < F_REPLACE || ed.replace_open) && gfx_rect_contains(find_control_rect(c), x, y)) {
                return c;
            }
        }
    }
    return C_NONE;
}

/* --- keyboard ---------------------------------------------------------------------------------------------- */

static void vertical(int lines, bool extend) {
    int line = ed.cursor.line + lines;
    line = line < 0 ? 0 : line >= ed.count ? ed.count - 1 : line;
    int goal = ed.goal_cells;
    struct pos p = { line, cells_to_col(line, goal) };
    if (lines < 0 && ed.cursor.line == 0) {
        p = (struct pos){ 0, 0 };
    } else if (lines > 0 && ed.cursor.line == ed.count - 1) {
        p = doc_end();
    }
    move_to(p, extend);
    ed.goal_cells = goal;   /* keep aiming for the same column */
}

static void text_key(const struct gui_event *ev) {
    bool shift = ev->modifiers & OS_MOD_SHIFT, ctrl = ev->modifiers & OS_MOD_CTRL;
    struct pos a, b;
    selection(&a, &b);
    switch (ev->key) {
    case OS_KEY_LEFT:
        move_to(ctrl ? word_left(ed.cursor) : !shift && has_selection() ? a : pos_prev(ed.cursor), shift);
        return;
    case OS_KEY_RIGHT:
        move_to(ctrl ? word_right(ed.cursor) : !shift && has_selection() ? b : pos_next(ed.cursor), shift);
        return;
    case OS_KEY_UP:
        if (ctrl) {
            ed.scroll_line--;
            clamp_scroll();
        } else {
            vertical(-1, shift);
        }
        return;
    case OS_KEY_DOWN:
        if (ctrl) {
            ed.scroll_line++;
            clamp_scroll();
        } else {
            vertical(1, shift);
        }
        return;
    case KEY_PGUP:
        vertical(-visible_lines(), shift);
        return;
    case KEY_PGDN:
        vertical(visible_lines(), shift);
        return;
    case OS_KEY_HOME: {
        if (ctrl) {
            move_to((struct pos){ 0, 0 }, shift);
            return;
        }
        /* First press: to the first non-space; again: to the very start. */
        const struct line *l = &ed.lines[ed.cursor.line];
        int first = 0;
        while (first < l->len && l->text[first] == ' ') {
            first++;
        }
        move_to((struct pos){ ed.cursor.line, ed.cursor.col == first ? 0 : first }, shift);
        return;
    }
    case OS_KEY_END:
        move_to(ctrl ? doc_end() : (struct pos){ ed.cursor.line, ed.lines[ed.cursor.line].len }, shift);
        return;
    case OS_KEY_BACKSPACE:
        do_backspace(ctrl);
        return;
    case OS_KEY_DELETE:
        do_delete_key(ctrl);
        return;
    case OS_KEY_ENTER:
        do_enter();
        return;
    case OS_KEY_TAB:
        do_tab(shift);
        return;
    case OS_KEY_ESCAPE:
        if (has_selection()) {
            ed.anchor = ed.cursor;
        } else if (ed.find_open) {
            close_find();
        }
        return;
    }
    if (ev->ch >= ' ' && ev->ch < 0x7F && !ctrl) {
        char c = (char)ev->ch;
        if (c == ' ' || has_selection()) {
            undo_break();
        }
        insert_text(&c, 1);
    }
}

static void on_key(const struct gui_event *ev) {
    bool shift = ev->modifiers & OS_MOD_SHIFT, ctrl = ev->modifiers & OS_MOD_CTRL;
    ed.status[0] = '\0';
    if (ctrl) {
        switch (ev->key) {
        case KEY_N:      act(T_NEW); return;
        case KEY_O:      act(T_OPEN); return;
        case KEY_S:      act(shift ? T_SAVE_AS : T_SAVE); return;
        case KEY_F:      open_find(false); return;
        case KEY_H:      open_find(true); return;
        case KEY_G:      act(T_GOTO); return;
        case KEY_EQUALS: act(T_ZOOM_IN); return;
        case KEY_MINUS:  act(T_ZOOM_OUT); return;
        case KEY_0:      act(T_ZOOM); return;
        }
    }
    if (ev->key == KEY_F3) {
        if (!ed.find_open) {
            open_find(false);
        }
        find(!shift, false);
        return;
    }
    if (ed.focus != FOCUS_TEXT) {
        struct textfield *f = ed.focus == FOCUS_FIND ? &ed.find : &ed.replace;
        if (ev->key == OS_KEY_ESCAPE) {
            close_find();
        } else if (ev->key == OS_KEY_ENTER) {
            if (ed.focus == FOCUS_REPLACE) {
                replace_one();
            } else {
                find(!shift, false);
            }
        } else if (ev->key == OS_KEY_TAB) {
            ed.focus = ed.replace_open && ed.focus == FOCUS_FIND ? FOCUS_REPLACE : FOCUS_FIND;
        } else if (field_key(f, ev) && f == &ed.find) {
            find(true, true);   /* search as you type */
        }
        return;
    }
    if (ctrl) {
        switch (ev->key) {
        case KEY_Z: act(shift ? T_REDO : T_UNDO); return;
        case KEY_Y: act(T_REDO); return;
        case KEY_X: do_copy(true); return;
        case KEY_C: do_copy(false); return;
        case KEY_V: do_paste(); return;
        case KEY_A: select_all(); return;
        }
    }
    text_key(ev);
}

/* --- mouse ----------------------------------------------------------------------------------------------- */

static struct pos pos_at(int x, int y) {
    struct gfx_rect area = text_area();
    int line = ed.scroll_line + (int)((y - area.y - TEXT_PAD / 2) / ed.cell_h);
    if (y < area.y + TEXT_PAD / 2) {
        line = ed.scroll_line - 1;
    }
    if (line < 0) {
        return (struct pos){ 0, 0 };
    }
    if (line >= ed.count) {
        return doc_end();
    }
    float cells = (x - text_left() + ed.scroll_x) / ed.cell_w;
    int c = cells < 0 ? 0 : (int)(cells + 0.5f);
    return (struct pos){ line, cells_to_col(line, c) };
}

#define ITEM(label, action) { label, MENU_BASE + (action), !control_enabled(action), false }
#define LINE { NULL, 0, false, false }

static void context_menu(const struct gui_event *ev) {
    const struct gui_menu_item items[] = {
        ITEM("Undo", T_UNDO), ITEM("Redo", T_REDO), LINE,
        ITEM("Cut", T_CUT), ITEM("Copy", T_COPY), ITEM("Paste", T_PASTE), ITEM("Delete", T_DELETE), LINE,
        ITEM("Select all", T_SELECT_ALL),
    };
    draw();
    gui_show_menu(ed.g, ev->screen_x, ev->screen_y, items, 9, ed.w);
}

/* A menu from the menu bar, opened just under its title. */
static void bar_menu(int which, const struct gui_event *ev) {
    struct gfx_rect r = menu_title_rect(which);
    int x = ev->screen_x - ev->x + r.x - 2, y = ev->screen_y - ev->y + r.y + r.h;
    draw();
    if (which == 0) {
        const struct gui_menu_item items[] = {
            ITEM("New\tCtrl+N", T_NEW), ITEM("Open...\tCtrl+O", T_OPEN), LINE,
            ITEM("Save\tCtrl+S", T_SAVE), ITEM("Save as...\tCtrl+Shift+S", T_SAVE_AS), LINE,
            ITEM("Exit", T_EXIT),
        };
        gui_show_menu(ed.g, x, y, items, (int)(sizeof(items) / sizeof(items[0])), ed.w);
    } else if (which == 1) {
        const struct gui_menu_item items[] = {
            ITEM("Undo\tCtrl+Z", T_UNDO), ITEM("Redo\tCtrl+Y", T_REDO), LINE,
            ITEM("Cut\tCtrl+X", T_CUT), ITEM("Copy\tCtrl+C", T_COPY), ITEM("Paste\tCtrl+V", T_PASTE),
            ITEM("Delete\tDel", T_DELETE), LINE,
            ITEM("Select all\tCtrl+A", T_SELECT_ALL), LINE,
            ITEM("Find\tCtrl+F", T_FIND), ITEM("Find next\tF3", T_FIND_NEXT), ITEM("Replace\tCtrl+H", T_REPLACE),
            ITEM("Go to line\tCtrl+G", T_GOTO),
        };
        gui_show_menu(ed.g, x, y, items, (int)(sizeof(items) / sizeof(items[0])), ed.w);
    } else {
        const struct gui_menu_item items[] = {
            ITEM("Zoom in\tCtrl+=", T_ZOOM_IN), ITEM("Zoom out\tCtrl+-", T_ZOOM_OUT),
            ITEM("Reset zoom\tCtrl+0", T_ZOOM), LINE,
            { "Line numbers", MENU_BASE + T_LINE_NUMBERS, false, line_numbers },
        };
        gui_show_menu(ed.g, x, y, items, (int)(sizeof(items) / sizeof(items[0])), ed.w);
    }
}

static void on_menu(int id) {
    if (id >= MENU_BASE) {
        act(id - MENU_BASE);
    }
}

/* Click on a scroll bar: grab the thumb, or jump a page towards the click. */
static bool bar_mouse_down(const struct gui_event *ev) {
    int which = bar_at(ev->x, ev->y);
    if (which == BAR_NONE || ev->button != OS_MOUSE_LEFT) {
        return which != BAR_NONE;
    }
    struct gfx_rect thumb = bar_thumb(which);
    if (gfx_rect_contains(thumb, ev->x, ev->y)) {
        bar_drag = which;
        bar_grab = which == BAR_VERTICAL ? ev->y - thumb.y : ev->x - thumb.x;
    } else if (which == BAR_VERTICAL) {
        ed.scroll_line += ev->y < thumb.y ? -visible_lines() : visible_lines();
        clamp_scroll();
    } else {
        float page = text_width() * 0.8f, max = max_scroll_x();
        ed.scroll_x += ev->x < thumb.x ? -page : page;
        ed.scroll_x = ed.scroll_x < 0 ? 0 : ed.scroll_x > max ? max : ed.scroll_x;
    }
    return true;
}

static void bar_mouse_move(int x, int y) {
    if (bar_drag == BAR_VERTICAL) {
        struct gfx_rect t = vbar_track(), thumb = bar_thumb(BAR_VERTICAL);
        int max = ed.count - visible_lines();
        float span = t.h - thumb.h;
        float f = span > 0 ? (y - bar_grab - t.y) / span : 0;
        ed.scroll_line = (int)(f * max + 0.5f);
        clamp_scroll();
    } else if (bar_drag == BAR_HORIZONTAL) {
        struct gfx_rect t = hbar_track(), thumb = bar_thumb(BAR_HORIZONTAL);
        float span = t.w - thumb.w, max = max_scroll_x();
        float f = span > 0 ? (x - bar_grab - t.x) / span : 0;
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        ed.scroll_x = f * max;
    }
}

static void on_mouse_down(const struct gui_event *ev) {
    int c = control_at(ev->x, ev->y);
    if (c >= M_FILE && c <= M_VIEW) {
        if (ev->button == OS_MOUSE_LEFT) {
            bar_menu(c - M_FILE, ev);
        }
        return;
    }
    if (bar_mouse_down(ev)) {
        return;
    }
    if (c != C_NONE) {
        if (ev->button == OS_MOUSE_LEFT) {
            ed.pressed_button = c;
        }
        return;
    }
    if (ed.find_open && gfx_rect_contains(find_field_rect(0), ev->x, ev->y)) {
        ed.focus = FOCUS_FIND;
        field_click(&ed.find, find_field_rect(0), ev->x, ev->modifiers & OS_MOD_SHIFT);
        return;
    }
    if (ed.replace_open && gfx_rect_contains(find_field_rect(1), ev->x, ev->y)) {
        ed.focus = FOCUS_REPLACE;
        field_click(&ed.replace, find_field_rect(1), ev->x, ev->modifiers & OS_MOD_SHIFT);
        return;
    }
    if (!gfx_rect_contains(text_area(), ev->x, ev->y)) {
        return;
    }
    ed.focus = FOCUS_TEXT;
    struct pos p = pos_at(ev->x, ev->y);
    if (ev->button == OS_MOUSE_RIGHT) {
        /* Right click outside the selection moves the cursor there first. */
        struct pos a, b;
        selection(&a, &b);
        if (!has_selection() || pos_cmp(p, a) < 0 || pos_cmp(p, b) > 0) {
            move_to(p, false);
        }
        context_menu(ev);
        return;
    }
    if (ev->button != OS_MOUSE_LEFT) {
        return;
    }
    uint64_t now = uptime_ms();
    bool again = now - ed.last_click_ms < DOUBLE_CLICK && pos_cmp(p, ed.last_click_pos) == 0;
    ed.clicks = again ? ed.clicks % 3 + 1 : 1;
    ed.last_click_ms = now;
    ed.last_click_pos = p;
    if (ed.clicks == 2) {
        select_word_at(p);
    } else if (ed.clicks == 3) {
        select_line_at(p.line);
    } else {
        move_to(p, ev->modifiers & OS_MOD_SHIFT);
        ed.selecting = true;
    }
    undo_break();
}

static void drag_to(int x, int y) {
    struct gfx_rect area = text_area();
    /* Past the top or bottom edge: scroll that way. */
    if (y < area.y && ed.scroll_line > 0) {
        ed.scroll_line--;
    } else if (y >= area.y + area.h) {
        ed.scroll_line++;
        clamp_scroll();
    }
    ed.cursor = pos_at(x, y < area.y ? area.y : y >= area.y + area.h ? area.y + area.h - 1 : y);
    ensure_visible();
}

/* --- start and main loop ------------------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    ed.g = gui_open();
    if (!ed.g) {
        return 1;
    }
    ed.mono = gui_font(ed.g, GUI_FONT_MONO);
    ed.ui = gui_font(ed.g, GUI_FONT_REGULAR);
    ed.ui_bold = gui_font(ed.g, GUI_FONT_BOLD);
    if (!ed.mono || !ed.ui || !ed.ui_bold) {
        return 1;
    }
    ed.font_size = DEFAULT_SIZE;
    metrics();
    doc_reset();
    ed.hover_button = ed.pressed_button = C_NONE;
    ed.focused = true;

    ed.w = gui_window_create(ed.g, "Text Editor", -1, -1, 860, 560);
    if (!ed.w) {
        return 1;
    }
    update_title();
    if (argc > 1) {
        editor_open_path(argv[1]);
    }
    draw();

    while (!quit) {
        struct gui_event ev;
        /* While dragging a selection past the edge, keep scrolling. */
        struct gfx_rect area = text_area();
        bool autoscroll = ed.selecting && (mouse_y < area.y || mouse_y >= area.y + area.h);
        if (!gui_wait_event(ed.g, &ev, autoscroll ? 50 : -1)) {
            if (autoscroll) {
                drag_to(mouse_x, mouse_y);
                draw();
            }
            continue;
        }
        if (ev.type == GUI_EVENT_QUIT) {
            break;
        }
        if (ev.type == GUI_EVENT_CLOSE) {
            unless_unsaved(PENDING_QUIT);
        } else if (ev.type == GUI_EVENT_FOCUS) {
            ed.focused = ev.focused;
        } else if (ev.type == GUI_EVENT_RESIZE) {
            clamp_scroll();
        } else if (ed.dialog.kind != DIALOG_NONE) {
            if (ev.type == GUI_EVENT_MENU) {
                continue;
            }
            dialog_event(&ev);
        } else {
            switch (ev.type) {
            case GUI_EVENT_KEY:
                on_key(&ev);
                break;
            case GUI_EVENT_MOUSE_DOWN:
                ed.status[0] = '\0';
                on_mouse_down(&ev);
                break;
            case GUI_EVENT_MOUSE_UP: {
                int pressed = ed.pressed_button;
                ed.pressed_button = C_NONE;
                ed.selecting = false;
                bar_drag = BAR_NONE;
                if (pressed != C_NONE && control_at(ev.x, ev.y) == pressed) {
                    act(pressed);
                }
                break;
            }
            case GUI_EVENT_MOUSE_MOVE: {
                mouse_x = ev.x;
                mouse_y = ev.y;
                if (bar_drag != BAR_NONE && (ev.buttons & OS_MOUSE_LEFT)) {
                    bar_mouse_move(ev.x, ev.y);
                    break;
                }
                if (ed.selecting && (ev.buttons & OS_MOUSE_LEFT)) {
                    drag_to(ev.x, ev.y);
                    break;
                }
                int hover = control_at(ev.x, ev.y), bar = bar_at(ev.x, ev.y);
                if (hover == ed.hover_button && bar == bar_hover) {
                    continue;   /* nothing changed */
                }
                ed.hover_button = hover;
                bar_hover = bar;
                break;
            }
            case GUI_EVENT_SCROLL:
                ed.scroll_line += ev.wheel * WHEEL_LINES;
                clamp_scroll();
                break;
            case GUI_EVENT_MENU:
                on_menu(ev.menu_id);
                break;
            default:
                continue;
            }
        }
        if (!quit) {
            update_title();
            draw();
        }
    }
    gui_close(ed.g);
    return 0;
}
