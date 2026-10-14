/* A one-line text box: typing, cursor keys, selection, clipboard. ASCII only. */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "editor.h"

#define KEY_A 0x1E
#define KEY_C 0x2E
#define KEY_V 0x2F
#define KEY_X 0x2D

void field_set(struct textfield *f, const char *text) {
    strlcpy(f->text, text, sizeof(f->text));
    f->len = (int)strlen(f->text);
    f->anchor = 0;
    f->cursor = f->len;   /* all selected: typing replaces it */
    f->scroll = 0;
}

static void delete_selection(struct textfield *f) {
    int a = f->anchor < f->cursor ? f->anchor : f->cursor;
    int b = f->anchor < f->cursor ? f->cursor : f->anchor;
    memmove(f->text + a, f->text + b, (size_t)(f->len - b + 1));
    f->len -= b - a;
    f->cursor = f->anchor = a;
}

static void insert(struct textfield *f, const char *s, int n) {
    if (f->anchor != f->cursor) {
        delete_selection(f);
    }
    int room = (int)sizeof(f->text) - 1 - f->len;
    n = n > room ? room : n;
    memmove(f->text + f->cursor + n, f->text + f->cursor, (size_t)(f->len - f->cursor + 1));
    memcpy(f->text + f->cursor, s, (size_t)n);
    f->len += n;
    f->cursor = f->anchor = f->cursor + n;
}

bool field_key(struct textfield *f, const struct gui_event *ev) {
    bool shift = ev->modifiers & OS_MOD_SHIFT, ctrl = ev->modifiers & OS_MOD_CTRL;
    int a = f->anchor < f->cursor ? f->anchor : f->cursor;
    int b = f->anchor < f->cursor ? f->cursor : f->anchor;
    if (ctrl) {
        if (ev->key == KEY_A) {
            f->anchor = 0;
            f->cursor = f->len;
        } else if ((ev->key == KEY_C || ev->key == KEY_X) && a != b) {
            clipboard_write(f->text + a, b - a);
            if (ev->key == KEY_X) {
                delete_selection(f);
                return true;
            }
        } else if (ev->key == KEY_V) {
            char *text;
            int len;
            if (clipboard_read(&text, &len)) {
                /* One line only: stop at the first line break. */
                int n = 0;
                while (n < len && text[n] != '\n' && text[n] != '\r') {
                    n++;
                }
                insert(f, text, n);
                free(text);
                return true;
            }
        }
        return false;
    }
    switch (ev->key) {
    case OS_KEY_LEFT:
        f->cursor = !shift && a != b ? a : f->cursor > 0 ? f->cursor - 1 : 0;
        break;
    case OS_KEY_RIGHT:
        f->cursor = !shift && a != b ? b : f->cursor < f->len ? f->cursor + 1 : f->len;
        break;
    case OS_KEY_HOME:
        f->cursor = 0;
        break;
    case OS_KEY_END:
        f->cursor = f->len;
        break;
    case OS_KEY_BACKSPACE:
        if (a != b) {
            delete_selection(f);
        } else if (f->cursor > 0) {
            f->anchor = f->cursor - 1;
            delete_selection(f);
        }
        return true;
    case OS_KEY_DELETE:
        if (a != b) {
            delete_selection(f);
        } else if (f->cursor < f->len) {
            f->anchor = f->cursor + 1;
            delete_selection(f);
        }
        return true;
    default:
        if (ev->ch >= ' ' && ev->ch < 0x7F) {
            char c = (char)ev->ch;
            insert(f, &c, 1);
            return true;
        }
        return false;
    }
    if (!shift) {
        f->anchor = f->cursor;
    }
    return false;
}

static float text_x(struct textfield *f, struct gfx_rect box, int index) {
    char before[sizeof(f->text)];
    memcpy(before, f->text, (size_t)index);
    before[index] = '\0';
    return box.x + 10 - f->scroll + ttf_text_width(ed.ui, UI_SIZE, before);
}

void field_click(struct textfield *f, struct gfx_rect box, int x, bool extend) {
    int best = 0;
    float best_d = 1e9f;
    for (int i = 0; i <= f->len; i++) {
        float d = text_x(f, box, i) - (float)x;
        d = d < 0 ? -d : d;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    f->cursor = best;
    if (!extend) {
        f->anchor = best;
    }
}

void field_draw(struct gfx_surface *s, struct textfield *f, struct gfx_rect box, bool focused,
                const char *placeholder) {
    gfx_fill_rounded_rect(s, box, 7, FIELD);
    if (focused) {
        gfx_stroke_rounded_rect(s, box, 7, 1.5f, ACCENT);
    }
    /* Keep the cursor inside the box. */
    float inner = box.w - 20;
    float cx = text_x(f, box, f->cursor) - box.x - 10 + f->scroll;
    if (cx - f->scroll > inner) {
        f->scroll = cx - inner;
    } else if (cx < f->scroll) {
        f->scroll = cx;
    }
    gfx_set_clip(s, gfx_rect(box.x + 4, box.y, box.w - 8, box.h));
    float baseline = box.y + box.h / 2.0f + 4.5f;
    if (f->anchor != f->cursor) {
        int a = f->anchor < f->cursor ? f->anchor : f->cursor;
        int b = f->anchor < f->cursor ? f->cursor : f->anchor;
        float xa = text_x(f, box, a), xb = text_x(f, box, b);
        gfx_fill_rect(s, gfx_rect((int)xa, box.y + 6, (int)(xb - xa) + 1, box.h - 12), SELECTION);
    }
    if (!f->len && placeholder) {
        ttf_draw_text(s, ed.ui, UI_SIZE, box.x + 10, baseline, INK_FAINT, placeholder);
    }
    ttf_draw_text(s, ed.ui, UI_SIZE, box.x + 10 - f->scroll, baseline, INK, f->text);
    if (focused) {
        float x = text_x(f, box, f->cursor);
        gfx_fill_rect(s, gfx_rect((int)x, box.y + 7, 1, box.h - 14), INK);
    }
    gfx_reset_clip(s);
}
