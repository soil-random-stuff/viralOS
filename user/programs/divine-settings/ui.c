/*
 * The controls, immediate-mode: a page is a function that lays out and draws its rows top to bottom,
 * and each control both draws itself and reports what the current event did to it. Every event
 * runs the page twice: a layout pass (nothing drawn: it measures the cards, so their backgrounds can
 * be drawn before their rows) and the real one.
 *
 * Controls are known by a key that lasts between passes: the address of the value they change (a
 * toggle's bool, a text box's struct field) or, for buttons, their label.
 */

#include <string.h>

#include "settings.h"

struct ui ui;

#define ROW_PAD     16        /* inside a card, left and right */
#define TOGGLE_W    40
#define TOGGLE_H    24
#define FIELD_W     240
#define FIELD_H     28
#define CARD_RADIUS 12

/* --- text ------------------------------------------------------------------------------------------- */

void ui_text(float size, bool bold, float x, float y, gfx_color color, const char *text) {
    struct ttf_font *f = bold ? ui.bold : ui.font;
    if (!ui.measuring && f) {
        ttf_draw_text(ui.s, f, size, x, y, color, text);
    }
}

float ui_text_width(float size, bool bold, const char *text) {
    struct ttf_font *f = bold ? ui.bold : ui.font;
    return f ? ttf_text_width(f, size, text) : 0;
}

/* Cut with "..." to fit in max_w. */
void ui_text_clipped(float size, bool bold, float x, float y, float max_w, gfx_color color, const char *text) {
    if (ui_text_width(size, bold, text) <= max_w) {
        ui_text(size, bold, x, y, color, text);
        return;
    }
    char buf[256];
    strlcpy(buf, text, sizeof(buf));
    size_t n = strlen(buf);
    while (n > 0) {
        buf[--n] = '\0';
        char tmp[260];
        strlcpy(tmp, buf, sizeof(tmp));
        strlcat(tmp, "...", sizeof(tmp));
        if (ui_text_width(size, bold, tmp) <= max_w) {
            ui_text(size, bold, x, y, color, tmp);
            return;
        }
    }
}

/* --- input -------------------------------------------------------------------------------------------- */

bool ui_hover(struct gfx_rect r) {
    return !ui.measuring && gfx_rect_contains(ui.area, ui.mx, ui.my) && gfx_rect_contains(r, ui.mx, ui.my);
}

bool ui_clicked(struct gfx_rect r, const void *key) {
    if (ui.measuring) {
        return false;
    }
    bool over = ui_hover(r);
    if (over) {
        ui.hand = true;
    }
    if (ui.press && over) {
        ui.active = key;
    }
    return ui.release && over && ui.active == key;
}

/* --- layout -------------------------------------------------------------------------------------------- */

void ui_space(int h) {
    ui.y += h;
}

void ui_heading(const char *text) {
    ui.y += 6;
    ui_text(FONT_HEAD, true, ui.left + 4, ui.y + 17, INK, text);
    ui.y += 30;
}

void ui_note(const char *text) {
    /* Word-wrapped, grey, under the card above. */
    float max_w = ui.width - 8;
    char line[256] = "";
    const char *p = text;
    ui.y -= 8;
    while (*p) {
        const char *word_end = p;
        while (*word_end && *word_end != ' ') {
            word_end++;
        }
        char trial[256];
        strlcpy(trial, line, sizeof(trial));
        if (trial[0]) {
            strlcat(trial, " ", sizeof(trial));
        }
        size_t len = strlen(trial), n = (size_t)(word_end - p);
        if (len + n < sizeof(trial)) {
            memcpy(trial + len, p, n);
            trial[len + n] = '\0';
        }
        if (line[0] && ui_text_width(FONT_SMALL, false, trial) > max_w) {
            ui_text(FONT_SMALL, false, ui.left + 4, ui.y + 14, INK_SOFT, line);
            ui.y += 18;
            size_t m = (size_t)(word_end - p) < sizeof(line) - 1 ? (size_t)(word_end - p) : sizeof(line) - 1;
            memcpy(line, p, m);
            line[m] = '\0';
        } else {
            strlcpy(line, trial, sizeof(line));
        }
        p = *word_end ? word_end + 1 : word_end;
    }
    if (line[0]) {
        ui_text(FONT_SMALL, false, ui.left + 4, ui.y + 14, INK_SOFT, line);
        ui.y += 18;
    }
    ui.y += 18;
}

void ui_card_begin(void) {
    ui.in_card = true;
    ui.card_top = ui.y;
    ui.card_rows = 0;
    if (!ui.measuring && ui.card < 32) {
        gfx_fill_rounded_rect(ui.s, gfx_rect(ui.left, ui.y, ui.width, ui.card_heights[ui.card]), CARD_RADIUS, CARD);
    }
}

void ui_card_end(void) {
    if (ui.measuring && ui.card < 32) {
        ui.card_heights[ui.card] = ui.y - ui.card_top;
    }
    ui.card++;
    ui.in_card = false;
    ui.y += 18;
}

struct gfx_rect ui_row(int height) {
    if (ui.card_rows > 0 && !ui.measuring) {
        gfx_fill_rect(ui.s, gfx_rect(ui.left + ROW_PAD, ui.y, ui.width - 2 * ROW_PAD, 1), CARD_LINE);
    }
    struct gfx_rect r = gfx_rect(ui.left, ui.y, ui.width, height);
    ui.y += height;
    ui.card_rows++;
    return r;
}

/* The label (and its grey second line) on the left of a row. */
static void row_label(struct gfx_rect r, const char *label, const char *sub, float max_w) {
    if (sub) {
        ui_text_clipped(FONT_BODY, false, r.x + ROW_PAD, r.y + r.h / 2 - 3, max_w, INK, label);
        ui_text_clipped(FONT_SMALL, false, r.x + ROW_PAD, r.y + r.h / 2 + 15, max_w, INK_SOFT, sub);
    } else {
        ui_text_clipped(FONT_BODY, false, r.x + ROW_PAD, r.y + r.h / 2 + 5, max_w, INK, label);
    }
}

/* --- controls ------------------------------------------------------------------------------------------ */

bool ui_toggle_enabled(struct gfx_rect r, bool *value, bool enabled) {
    bool changed = enabled && ui_clicked(r, value);
    if (changed) {
        *value = !*value;
        ui.dirty = true;
    }
    if (!ui.measuring) {
        gfx_color on = enabled ? ACCENT : GFX_RGB(0xAC, 0xC6, 0xF4);
        gfx_color off = enabled ? GFX_RGB(0xD5, 0xD5, 0xDB) : GFX_RGB(0xE6, 0xE6, 0xEA);
        gfx_fill_rounded_rect(ui.s, r, r.h / 2.0f, *value ? on : off);
        float knob = r.h - 4;
        float cx = *value ? r.x + r.w - 2 - knob / 2 : r.x + 2 + knob / 2;
        gfx_fill_circle(ui.s, cx, r.y + r.h / 2.0f + 0.6f, knob / 2 + 0.6f, GFX_RGBA(0, 0, 0, enabled ? 30 : 12));
        gfx_fill_circle(ui.s, cx, r.y + r.h / 2.0f, knob / 2, GFX_RGB(255, 255, 255));
    }
    return changed;
}

bool ui_toggle(struct gfx_rect r, bool *value) {
    return ui_toggle_enabled(r, value, true);
}

bool ui_toggle_row_enabled(const char *label, const char *sub, bool *value, bool enabled) {
    struct gfx_rect r = ui_row(sub ? 58 : 44);
    row_label(r, label, sub, r.w - 2 * ROW_PAD - TOGGLE_W - 16);
    return ui_toggle_enabled(gfx_rect(r.x + r.w - ROW_PAD - TOGGLE_W, r.y + (r.h - TOGGLE_H) / 2, TOGGLE_W, TOGGLE_H),
                             value, enabled);
}

bool ui_toggle_row(const char *label, const char *sub, bool *value) {
    return ui_toggle_row_enabled(label, sub, value, true);
}

bool ui_button(struct gfx_rect r, const char *label, bool primary, bool enabled) {
    bool clicked = enabled && ui_clicked(r, label);
    if (!ui.measuring) {
        bool hot = enabled && ui_hover(r);
        gfx_color fill = primary ? (hot ? GFX_RGB(0x25, 0x5E, 0xD6) : ACCENT) : (hot ? CONTROL_HOT : CONTROL);
        if (!enabled) {
            fill = GFX_RGB(0xEE, 0xEE, 0xF1);
        }
        gfx_fill_rounded_rect(ui.s, r, 7, fill);
        float tw = ui_text_width(FONT_BODY - 0.5f, false, label);
        ui_text(FONT_BODY - 0.5f, false, r.x + (r.w - tw) / 2, r.y + r.h / 2 + 5,
                !enabled ? INK_FAINT : primary ? GFX_RGB(255, 255, 255) : INK, label);
    }
    return clicked;
}

bool ui_button_row_enabled(const char *label, const char *sub, const char *button, bool primary, bool enabled) {
    struct gfx_rect r = ui_row(sub ? 58 : 46);
    int bw = (int)ui_text_width(FONT_BODY - 0.5f, false, button) + 28;
    row_label(r, label, sub, r.w - 2 * ROW_PAD - bw - 16);
    return ui_button(gfx_rect(r.x + r.w - ROW_PAD - bw, r.y + (r.h - 28) / 2, bw, 28), button, primary, enabled);
}

bool ui_button_row(const char *label, const char *sub, const char *button, bool primary) {
    return ui_button_row_enabled(label, sub, button, primary, true);
}

void ui_info_row(const char *label, const char *value) {
    struct gfx_rect r = ui_row(40);
    float vw = ui_text_width(FONT_BODY, false, value);
    float max_v = r.w * 0.6f;
    row_label(r, label, NULL, r.w - 2 * ROW_PAD - (vw < max_v ? vw : max_v) - 16);
    if (vw > max_v) {
        ui_text_clipped(FONT_BODY, false, r.x + r.w - ROW_PAD - max_v, r.y + r.h / 2 + 5, max_v, INK_SOFT, value);
    } else {
        ui_text(FONT_BODY, false, r.x + r.w - ROW_PAD - vw, r.y + r.h / 2 + 5, INK_SOFT, value);
    }
}

void ui_bar(struct gfx_rect r, float fraction, gfx_color fill) {
    if (ui.measuring) {
        return;
    }
    fraction = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
    gfx_fill_rounded_rect(ui.s, r, r.h / 2.0f, GFX_RGB(0xE1, 0xE1, 0xE6));
    int w = (int)(r.w * fraction);
    if (w > 0) {
        gfx_fill_rounded_rect(ui.s, gfx_rect(r.x, r.y, w < r.h ? r.h : w, r.h), r.h / 2.0f, fill);
    }
}

bool ui_slider_row(const char *label, int *value, int min, int max, const char *low, const char *high) {
    bool marks = low || high;
    struct gfx_rect r = ui_row(marks ? 62 : 48);
    int track_x = r.x + (int)(r.w * 0.40f), track_w = r.x + r.w - ROW_PAD - track_x;
    int track_y = r.y + (marks ? 22 : r.h / 2);
    row_label(gfx_rect(r.x, r.y, r.w, marks ? 44 : r.h), label, NULL, track_x - r.x - ROW_PAD - 12);
    struct gfx_rect hit = gfx_rect(track_x - 10, track_y - 14, track_w + 20, 28);
    bool changed = false;
    ui_clicked(hit, value);
    if (!ui.measuring && ui.active == value && ui.held) {
        int v = min + (int)((float)(ui.mx - track_x) * (max - min) / track_w + 0.5f);
        v = v < min ? min : v > max ? max : v;
        if (v != *value) {
            *value = v;
            changed = true;
            ui.dirty = true;
        }
    }
    if (!ui.measuring) {
        float t = max > min ? (float)(*value - min) / (max - min) : 0;
        gfx_fill_rounded_rect(ui.s, gfx_rect(track_x, track_y - 2, track_w, 4), 2, GFX_RGB(0xD5, 0xD5, 0xDB));
        gfx_fill_rounded_rect(ui.s, gfx_rect(track_x, track_y - 2, (int)(track_w * t) + 1, 4), 2, ACCENT);
        float kx = track_x + track_w * t;
        gfx_fill_circle(ui.s, kx, track_y + 0.8f, 10.5f, GFX_RGBA(0, 0, 0, 40));
        gfx_fill_circle(ui.s, kx, track_y, 10, GFX_RGB(255, 255, 255));
        if (low) {
            ui_text(FONT_SMALL - 1, false, track_x, track_y + 28, INK_SOFT, low);
        }
        if (high) {
            ui_text(FONT_SMALL - 1, false, track_x + track_w - ui_text_width(FONT_SMALL - 1, false, high),
                    track_y + 28, INK_SOFT, high);
        }
    }
    return changed;
}

static void chevrons(float cx, float cy) {
    if (ui.measuring) {
        return;
    }
    gfx_draw_line(ui.s, cx - 3.5f, cy - 2, cx, cy - 5.5f, 1.4f, INK);
    gfx_draw_line(ui.s, cx, cy - 5.5f, cx + 3.5f, cy - 2, 1.4f, INK);
    gfx_draw_line(ui.s, cx - 3.5f, cy + 2, cx, cy + 5.5f, 1.4f, INK);
    gfx_draw_line(ui.s, cx, cy + 5.5f, cx + 3.5f, cy + 2, 1.4f, INK);
}

bool ui_choice_row(const char *label, const char *const *options, int count, int *selected) {
    struct gfx_rect r = ui_row(46);
    const char *current = *selected >= 0 && *selected < count ? options[*selected] : "";
    int cw = (int)ui_text_width(FONT_BODY, false, current) + 44;
    struct gfx_rect cap = gfx_rect(r.x + r.w - ROW_PAD - cw, r.y + (r.h - 28) / 2, cw, 28);
    row_label(r, label, NULL, cap.x - r.x - ROW_PAD - 12);
    bool changed = false;
    if (!ui.measuring && ui.menu_for == selected && ui.menu_choice >= 1 && ui.menu_choice <= count) {
        if (*selected != ui.menu_choice - 1) {
            *selected = ui.menu_choice - 1;
            changed = true;
            ui.dirty = true;
        }
        ui.menu_choice = -1;
        ui.menu_for = NULL;
    }
    if (ui_clicked(cap, selected)) {
        struct gui_menu_item items[16];
        int n = count < 16 ? count : 16;
        for (int i = 0; i < n; i++) {
            items[i] = (struct gui_menu_item){ options[i], i + 1, false, i == *selected };
        }
        ui.menu_for = selected;
        /* (ui.mx/my are window coordinates; the menu wants the screen's) */
        gui_show_menu(ui.g, ui.mx + (ui.screen_dx), cap.y + cap.h + 4 + ui.screen_dy, items, n, ui.win);
    }
    if (!ui.measuring) {
        gfx_fill_rounded_rect(ui.s, cap, 7, ui_hover(cap) ? CONTROL_HOT : CONTROL);
        ui_text(FONT_BODY, false, cap.x + 12, cap.y + 19, INK, current);
        chevrons(cap.x + cap.w - 16, cap.y + cap.h / 2.0f);
    }
    return changed;
}

bool ui_segmented_row(const char *label, const char *const *options, int count, int *selected) {
    struct gfx_rect r = ui_row(48);
    int widths[8], total = 0;
    count = count > 8 ? 8 : count;
    for (int i = 0; i < count; i++) {
        widths[i] = (int)ui_text_width(FONT_BODY - 0.5f, false, options[i]) + 28;
        total += widths[i];
    }
    struct gfx_rect box = gfx_rect(r.x + r.w - ROW_PAD - total - 4, r.y + (r.h - 30) / 2, total + 4, 30);
    row_label(r, label, NULL, box.x - r.x - ROW_PAD - 12);
    if (!ui.measuring) {
        gfx_fill_rounded_rect(ui.s, box, 8, CONTROL);
    }
    bool changed = false;
    int x = box.x + 2;
    for (int i = 0; i < count; i++) {
        struct gfx_rect seg = gfx_rect(x, box.y + 2, widths[i], box.h - 4);
        if (ui_clicked(seg, &options[i]) && *selected != i) {
            *selected = i;
            changed = true;
            ui.dirty = true;
        }
        if (!ui.measuring) {
            if (i == *selected) {
                gfx_fill_rounded_rect(ui.s, gfx_rect(seg.x, seg.y + 1, seg.w, seg.h), 7, GFX_RGBA(0, 0, 0, 22));
                gfx_fill_rounded_rect(ui.s, seg, 7, GFX_RGB(255, 255, 255));
            }
            float tw = ui_text_width(FONT_BODY - 0.5f, i == *selected, options[i]);
            ui_text(FONT_BODY - 0.5f, i == *selected, seg.x + (seg.w - tw) / 2, seg.y + seg.h / 2 + 5, INK, options[i]);
        }
        x += widths[i];
    }
    return changed;
}

/* --- text boxes ---------------------------------------------------------------------------------------- */

void field_set(struct field *f, const char *text) {
    strlcpy(f->text, text, sizeof(f->text));
    f->len = (int)strlen(f->text);
    f->cursor = f->len;
    f->scroll = 0;
}

/* What the box shows: the text, or stars for a password. */
static void shown(const struct field *f, char *out, int upto) {
    for (int i = 0; i < upto; i++) {
        out[i] = f->secret ? '*' : f->text[i];
    }
    out[upto] = '\0';
}

static float caret_x(struct field *f, int index) {
    char before[sizeof(f->text)];
    shown(f, before, index);
    return ui_text_width(FONT_BODY, false, before);
}

/* A key for the focused box: true if it was Enter. */
static bool field_key(struct field *f, const struct gui_event *ev) {
    switch (ev->key) {
    case OS_KEY_LEFT:      f->cursor = f->cursor > 0 ? f->cursor - 1 : 0; break;
    case OS_KEY_RIGHT:     f->cursor = f->cursor < f->len ? f->cursor + 1 : f->len; break;
    case OS_KEY_HOME:      f->cursor = 0; break;
    case OS_KEY_END:       f->cursor = f->len; break;
    case OS_KEY_ENTER:     return true;
    case OS_KEY_ESCAPE:    ui.focus = NULL; break;
    case OS_KEY_BACKSPACE:
        if (f->cursor > 0) {
            memmove(f->text + f->cursor - 1, f->text + f->cursor, (size_t)(f->len - f->cursor + 1));
            f->cursor--;
            f->len--;
        }
        break;
    case OS_KEY_DELETE:
        if (f->cursor < f->len) {
            memmove(f->text + f->cursor, f->text + f->cursor + 1, (size_t)(f->len - f->cursor));
            f->len--;
        }
        break;
    default:
        if (ev->ch >= ' ' && ev->ch < 0x7F && !(ev->modifiers & OS_MOD_CTRL) && f->len < (int)sizeof(f->text) - 1) {
            memmove(f->text + f->cursor + 1, f->text + f->cursor, (size_t)(f->len - f->cursor + 1));
            f->text[f->cursor++] = (char)ev->ch;
            f->len++;
        }
        break;
    }
    return false;
}

/* The box itself (also the sidebar's search). True on Enter. */
bool ui_field(struct gfx_rect box, struct field *f, const char *placeholder, bool enabled) {
    bool enter = false;
    if (!ui.measuring) {
        bool over = enabled && ui_hover(box);
        if (ui.press && over) {
            ui.focus = f;
            ui.focus_claimed = true;
            /* The caret goes to the nearest place to the click. */
            int best = f->len;
            float bx = ui.mx - (box.x + 10) + f->scroll, best_d = 1e9f;
            for (int i = 0; i <= f->len; i++) {
                float d = caret_x(f, i) - bx;
                d = d < 0 ? -d : d;
                if (d < best_d) {
                    best_d = d;
                    best = i;
                }
            }
            f->cursor = best;
            ui.dirty = true;
        }
        if (ui.focus == f && ui.key) {
            enter = field_key(f, ui.key);
            ui.key = NULL;
            ui.dirty = true;
        }
        if (!enabled && ui.focus == f) {
            ui.focus = NULL;
        }
        bool focused = ui.focus == f;
        gfx_fill_rounded_rect(ui.s, box, 7, enabled ? GFX_RGB(255, 255, 255) : GFX_RGB(0xF0, 0xF0, 0xF3));
        gfx_stroke_rounded_rect(ui.s, box, 7, focused ? 2 : 1, focused ? ACCENT : CARD_LINE);
        float inner = box.w - 20;
        float cx = caret_x(f, f->cursor);
        if (cx - f->scroll > inner) {
            f->scroll = cx - inner;
        } else if (cx < f->scroll) {
            f->scroll = cx;
        }
        struct gfx_rect inside = gfx_rect(box.x + 4, box.y, box.w - 8, box.h);
        gfx_set_clip(ui.s, ui.area.w ? gfx_rect_intersect(ui.area, inside) : inside);
        float baseline = box.y + box.h / 2.0f + 5;
        if (!f->len && placeholder) {
            ui_text(FONT_BODY, false, box.x + 10, baseline, INK_FAINT, placeholder);
        }
        char text[sizeof(f->text)];
        shown(f, text, f->len);
        ui_text(FONT_BODY, false, box.x + 10 - f->scroll, baseline, enabled ? INK : INK_SOFT, text);
        if (focused) {
            gfx_fill_rect(ui.s, gfx_rect(box.x + 10 + (int)(cx - f->scroll), box.y + 6, 1, box.h - 12), INK);
        }
        if (ui.area.w) {
            gfx_set_clip(ui.s, ui.area);
        } else {
            gfx_reset_clip(ui.s);
        }
    }
    return enter;
}

bool ui_field_row(const char *label, struct field *f, const char *placeholder, bool enabled) {
    struct gfx_rect r = ui_row(46);
    int fw = r.w / 2 < FIELD_W ? r.w / 2 : FIELD_W;
    struct gfx_rect box = gfx_rect(r.x + r.w - ROW_PAD - fw, r.y + (r.h - FIELD_H) / 2, fw, FIELD_H);
    row_label(r, label, NULL, box.x - r.x - ROW_PAD - 12);
    return ui_field(box, f, placeholder, enabled);
}

bool ui_wants_key(void) {
    return ui.focus != NULL;
}

/* --- running a page ------------------------------------------------------------------------------------- */

static void column(struct gfx_rect area, int scroll) {
    ui.area = area;
    ui.width = area.w - 56 < CONTENT_MAX ? area.w - 56 : CONTENT_MAX;
    ui.left = area.x + (area.w - ui.width) / 2;
    ui.y = area.y + 8 - scroll;
    ui.card = 0;
    ui.in_card = false;
}

void ui_run_page(void (*page)(void), struct gfx_rect area, int scroll, int *content_height) {
    /* The layout pass: sizes only. */
    ui.measuring = true;
    column(area, scroll);
    const struct gui_event *key = ui.key;
    int choice = ui.menu_choice;
    page();
    *content_height = ui.y + scroll - area.y + 16;

    /* The real one. */
    ui.measuring = false;
    ui.key = key;
    ui.menu_choice = choice;
    column(area, scroll);
    gfx_set_clip(ui.s, area);
    page();
    gfx_reset_clip(ui.s);
    ui.area = gfx_rect(0, 0, 0, 0);
}
