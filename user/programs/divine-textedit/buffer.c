/*
 * The document: an array of lines. Positions are (line, byte offset);
 * text may be UTF-8, and cursor movement steps over whole characters.
 *
 * Every change goes through doc_insert/doc_delete, which record it for
 * undo. Typing a word, or a run of backspaces, merges into one undo step;
 * undo_begin_group/undo_end_group make bigger steps (paste over a
 * selection, replace all).
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "editor.h"

#define MERGE_MS 1500   /* keystrokes closer than this undo together */

/* --- lines ---------------------------------------------------------------------------- */

static bool line_reserve(struct line *l, int need) {
    if (need + 1 <= l->cap) {
        return true;
    }
    int cap = l->cap ? l->cap : 16;
    while (cap < need + 1) {
        cap *= 2;
    }
    char *text = realloc(l->text, (size_t)cap);
    if (!text) {
        return false;
    }
    l->text = text;
    l->cap = cap;
    return true;
}

static bool lines_reserve(int need) {
    if (need <= ed.cap) {
        return true;
    }
    int cap = ed.cap ? ed.cap : 64;
    while (cap < need) {
        cap *= 2;
    }
    struct line *lines = realloc(ed.lines, sizeof(*lines) * (size_t)cap);
    if (!lines) {
        return false;
    }
    ed.lines = lines;
    ed.cap = cap;
    return true;
}

/* Insert an empty line at index i. */
static bool insert_line(int i) {
    if (!lines_reserve(ed.count + 1)) {
        return false;
    }
    memmove(&ed.lines[i + 1], &ed.lines[i], sizeof(ed.lines[0]) * (size_t)(ed.count - i));
    ed.lines[i] = (struct line){ 0 };
    ed.count++;
    if (!line_reserve(&ed.lines[i], 0)) {
        return false;
    }
    ed.lines[i].text[0] = '\0';
    return true;
}

static void free_changes(struct change *list, int *len) {
    for (int i = 0; i < *len; i++) {
        free(list[i].text);
    }
    *len = 0;
}

void doc_reset(void) {
    for (int i = 0; i < ed.count; i++) {
        free(ed.lines[i].text);
    }
    ed.count = 0;
    insert_line(0);
    free_changes(ed.undo, &ed.undo_len);
    free_changes(ed.redo, &ed.redo_len);
    ed.saved_undo_len = 0;
    ed.modified = false;
    ed.cursor = ed.anchor = (struct pos){ 0, 0 };
    ed.scroll_line = 0;
    ed.scroll_x = 0;
    ed.goal_cells = 0;
    ed.merge_blocked = true;
}

/* --- positions ------------------------------------------------------------------------- */

int pos_cmp(struct pos a, struct pos b) {
    return a.line != b.line ? (a.line < b.line ? -1 : 1) : (a.col < b.col ? -1 : a.col > b.col);
}

struct pos pos_clamp(struct pos p) {
    if (p.line < 0) {
        return (struct pos){ 0, 0 };
    }
    if (p.line >= ed.count) {
        return doc_end();
    }
    p.col = p.col < 0 ? 0 : p.col > ed.lines[p.line].len ? ed.lines[p.line].len : p.col;
    return p;
}

struct pos doc_end(void) {
    return (struct pos){ ed.count - 1, ed.lines[ed.count - 1].len };
}

static bool continuation(char c) {
    return ((unsigned char)c & 0xC0) == 0x80;
}

struct pos pos_next(struct pos p) {
    const struct line *l = &ed.lines[p.line];
    if (p.col >= l->len) {
        return p.line + 1 < ed.count ? (struct pos){ p.line + 1, 0 } : p;
    }
    p.col++;
    while (p.col < l->len && continuation(l->text[p.col])) {
        p.col++;
    }
    return p;
}

struct pos pos_prev(struct pos p) {
    if (p.col == 0) {
        return p.line > 0 ? (struct pos){ p.line - 1, ed.lines[p.line - 1].len } : p;
    }
    const struct line *l = &ed.lines[p.line];
    p.col--;
    while (p.col > 0 && continuation(l->text[p.col])) {
        p.col--;
    }
    return p;
}

/* Columns on screen = characters, not bytes. */
int col_to_cells(int line, int col) {
    const struct line *l = &ed.lines[line];
    int cells = 0;
    for (int i = 0; i < col && i < l->len; i++) {
        cells += !continuation(l->text[i]);
    }
    return cells;
}

int cells_to_col(int line, int cells) {
    const struct line *l = &ed.lines[line];
    int col = 0;
    while (col < l->len && cells > 0) {
        col++;
        while (col < l->len && continuation(l->text[col])) {
            col++;
        }
        cells--;
    }
    return col;
}

/* --- raw changes (no undo) ------------------------------------------------------------------- */

static struct pos raw_insert(struct pos at, const char *text, int len) {
    at = pos_clamp(at);
    struct line *l = &ed.lines[at.line];
    /* The part of the line after the insertion point moves to the end of the inserted text. */
    int tail_len = l->len - at.col;
    char *tail = malloc((size_t)tail_len + 1);
    if (!tail) {
        return at;
    }
    memcpy(tail, l->text + at.col, (size_t)tail_len);
    l->len = at.col;
    l->text[l->len] = '\0';

    int line = at.line;
    for (int i = 0; i < len;) {
        int j = i;
        while (j < len && text[j] != '\n') {
            j++;
        }
        struct line *cur = &ed.lines[line];
        if (line_reserve(cur, cur->len + (j - i))) {
            memcpy(cur->text + cur->len, text + i, (size_t)(j - i));
            cur->len += j - i;
            cur->text[cur->len] = '\0';
        }
        if (j < len) {   /* a newline: start the next line */
            if (!insert_line(line + 1)) {
                break;
            }
            line++;
        }
        i = j + 1;
        if (j == len) {
            break;
        }
    }
    struct pos end = { line, ed.lines[line].len };
    struct line *last = &ed.lines[line];
    if (line_reserve(last, last->len + tail_len)) {
        memcpy(last->text + last->len, tail, (size_t)tail_len);
        last->len += tail_len;
        last->text[last->len] = '\0';
    }
    free(tail);
    return end;
}

char *doc_get(struct pos a, struct pos b, int *len) {
    if (pos_cmp(a, b) > 0) {
        struct pos t = a;
        a = b;
        b = t;
    }
    int total = 0;
    for (int i = a.line; i <= b.line; i++) {
        int from = i == a.line ? a.col : 0, to = i == b.line ? b.col : ed.lines[i].len;
        total += to - from + (i < b.line);
    }
    char *out = malloc((size_t)total + 1);
    if (!out) {
        *len = 0;
        return NULL;
    }
    int n = 0;
    for (int i = a.line; i <= b.line; i++) {
        int from = i == a.line ? a.col : 0, to = i == b.line ? b.col : ed.lines[i].len;
        memcpy(out + n, ed.lines[i].text + from, (size_t)(to - from));
        n += to - from;
        if (i < b.line) {
            out[n++] = '\n';
        }
    }
    out[n] = '\0';
    *len = n;
    return out;
}

static void raw_delete(struct pos a, struct pos b) {
    struct line *first = &ed.lines[a.line];
    const struct line *last = &ed.lines[b.line];
    int tail_len = last->len - b.col;
    if (!line_reserve(first, a.col + tail_len)) {
        return;
    }
    memmove(first->text + a.col, last->text + b.col, (size_t)tail_len);
    first->len = a.col + tail_len;
    first->text[first->len] = '\0';
    /* The lines after the first, up to and including the last, go. */
    int gone = b.line - a.line;
    for (int i = a.line + 1; i <= b.line; i++) {
        free(ed.lines[i].text);
    }
    memmove(&ed.lines[a.line + 1], &ed.lines[b.line + 1],
            sizeof(ed.lines[0]) * (size_t)(ed.count - b.line - 1));
    ed.count -= gone;
}

/* --- recording changes ------------------------------------------------------------------------ */

static bool push_change(struct change **list, int *len, int *cap, struct change c) {
    if (*len == *cap) {
        int n = *cap ? *cap * 2 : 64;
        struct change *grown = realloc(*list, sizeof(**list) * (size_t)n);
        if (!grown) {
            free(c.text);
            return false;
        }
        *list = grown;
        *cap = n;
    }
    (*list)[(*len)++] = c;
    return true;
}

void undo_begin_group(void) {
    ed.open_group = ++ed.group;
    ed.merge_blocked = true;
}

void undo_end_group(void) {
    ed.open_group = 0;
    ed.merge_blocked = true;
}

void undo_break(void) {
    ed.merge_blocked = true;
}

static void touched(void) {
    ed.modified = ed.undo_len != ed.saved_undo_len;
}

static void record(bool insert, struct pos a, struct pos b, const char *text, int len) {
    free_changes(ed.redo, &ed.redo_len);
    uint64_t now = uptime_ms();
    struct change *last = ed.undo_len ? &ed.undo[ed.undo_len - 1] : NULL;
    bool word_char = len == 1 && text[0] != '\n' && text[0] != ' ';

    /* Typing: one more character right after the previous one. */
    if (!ed.open_group && !ed.merge_blocked && last && insert && last->insert && word_char &&
        pos_cmp(last->b, a) == 0 && now - last->time < MERGE_MS && ed.undo_len != ed.saved_undo_len) {
        char *grown = realloc(last->text, (size_t)last->len + 2);
        if (grown) {
            last->text = grown;
            last->text[last->len++] = text[0];
            last->text[last->len] = '\0';
            last->b = b;
            last->time = now;
            touched();
            return;
        }
    }
    /* Backspacing: one more character just before the previous deletion. */
    if (!ed.open_group && !ed.merge_blocked && last && !insert && !last->insert && len == 1 &&
        pos_cmp(b, last->a) == 0 && now - last->time < MERGE_MS && ed.undo_len != ed.saved_undo_len) {
        char *grown = malloc((size_t)last->len + 2);
        if (grown) {
            grown[0] = text[0];
            memcpy(grown + 1, last->text, (size_t)last->len + 1);
            free(last->text);
            last->text = grown;
            last->len++;
            last->a = a;
            last->time = now;
            touched();
            return;
        }
    }
    char *copy = malloc((size_t)len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, text, (size_t)len);
    copy[len] = '\0';
    struct change c = { insert, a, b, copy, len, ed.open_group ? ed.open_group : ++ed.group, now };
    if (ed.saved_undo_len > ed.undo_len) {
        ed.saved_undo_len = -1;   /* the saved state was undone past and is now gone */
    }
    push_change(&ed.undo, &ed.undo_len, &ed.undo_cap, c);
    ed.merge_blocked = !word_char && !(len == 1 && !insert);
    touched();
}

struct pos doc_insert(struct pos at, const char *text, int len) {
    at = pos_clamp(at);
    struct pos end = raw_insert(at, text, len);
    if (len > 0) {
        record(true, at, end, text, len);
    }
    return end;
}

void doc_delete(struct pos a, struct pos b) {
    a = pos_clamp(a);
    b = pos_clamp(b);
    if (pos_cmp(a, b) > 0) {
        struct pos t = a;
        a = b;
        b = t;
    }
    if (pos_cmp(a, b) == 0) {
        return;
    }
    int len;
    char *text = doc_get(a, b, &len);
    raw_delete(a, b);
    if (text) {
        record(false, a, b, text, len);
        free(text);
    }
}

/* --- undo and redo ----------------------------------------------------------------------------- */

static void apply(const struct change *c, bool reverse) {
    bool insert = c->insert != reverse;
    if (insert) {
        raw_insert(c->a, c->text, c->len);
        ed.cursor = ed.anchor = c->b;
    } else {
        raw_delete(c->a, c->b);
        ed.cursor = ed.anchor = c->a;
    }
}

bool doc_undo(void) {
    if (!ed.undo_len) {
        return false;
    }
    int group = ed.undo[ed.undo_len - 1].group;
    while (ed.undo_len && ed.undo[ed.undo_len - 1].group == group) {
        struct change c = ed.undo[--ed.undo_len];
        apply(&c, true);
        push_change(&ed.redo, &ed.redo_len, &ed.redo_cap, c);
    }
    ed.merge_blocked = true;
    touched();
    return true;
}

bool doc_redo(void) {
    if (!ed.redo_len) {
        return false;
    }
    int group = ed.redo[ed.redo_len - 1].group;
    while (ed.redo_len && ed.redo[ed.redo_len - 1].group == group) {
        struct change c = ed.redo[--ed.redo_len];
        apply(&c, false);
        push_change(&ed.undo, &ed.undo_len, &ed.undo_cap, c);
    }
    ed.merge_blocked = true;
    touched();
    return true;
}

void doc_mark_saved(void) {
    ed.saved_undo_len = ed.undo_len;
    ed.merge_blocked = true;
    ed.modified = false;
}

/* --- files ---------------------------------------------------------------------------------------- */

int doc_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return fd;
    }
    struct os_file_info info;
    if (stat(path, &info) == 0 && info.type == OS_DIRECTORY) {
        close(fd);
        return -EISDIR;
    }
    size_t cap = 4096, len = 0;
    char *data = malloc(cap);
    long n;
    while (data && (n = read(fd, data + len, cap - len)) > 0) {
        len += (size_t)n;
        if (len == cap) {
            char *grown = realloc(data, cap * 2);
            if (!grown) {
                free(data);
                data = NULL;
                break;
            }
            data = grown;
            cap *= 2;
        }
    }
    close(fd);
    if (!data) {
        return -ENOMEM;
    }

    doc_reset();
    /* Line by line: CRLF becomes LF, tabs become spaces (to the next multiple of TAB_WIDTH). */
    int line = 0;
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        struct line *l = &ed.lines[line];
        if (c == '\n') {
            if (!insert_line(line + 1)) {
                break;
            }
            line++;
        } else if (c == '\r' && i + 1 < len && data[i + 1] == '\n') {
            continue;
        } else if (c == '\t') {
            int spaces = TAB_WIDTH - col_to_cells(line, l->len) % TAB_WIDTH;
            if (line_reserve(l, l->len + spaces)) {
                memset(l->text + l->len, ' ', (size_t)spaces);
                l->len += spaces;
                l->text[l->len] = '\0';
            }
        } else if (line_reserve(l, l->len + 1)) {
            l->text[l->len++] = c ? c : ' ';   /* NULs would end the line early on screen */
            l->text[l->len] = '\0';
        }
    }
    free(data);
    doc_mark_saved();
    return 0;
}

int doc_save(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        return fd;
    }
    for (int i = 0; i < ed.count; i++) {
        const struct line *l = &ed.lines[i];
        if ((l->len && write(fd, l->text, (size_t)l->len) != l->len) ||
            (i + 1 < ed.count && write(fd, "\n", 1) != 1)) {
            close(fd);
            return -ENOSPC;
        }
    }
    close(fd);
    doc_mark_saved();
    return 0;
}
