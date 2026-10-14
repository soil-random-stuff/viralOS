/*
 * Dialogs: a small window over the program's window with an icon, a heading, a message, maybe a
 * checkbox, and buttons. gui_dialog_run waits for the answer with an event loop of its own.
 */

#include <image.h>
#include <sound.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include <paths.h>

#define WIDTH       460
#define PAD         24
#define ICON        48
#define TEXT_X      (PAD + ICON + 18)
#define TEXT_W      (WIDTH - TEXT_X - PAD)
#define HEAD_SIZE   15.0f
#define BODY_SIZE   13.0f
#define LINE_H      19
#define BUTTON_H    34
#define BUTTON_MIN  92
#define BUTTON_GAP  10
#define CHECK_H     30
#define MAX_LINES   40
#define KEEP_MAX    16

#define WHITE       GFX_RGB(255, 255, 255)
#define INK         GFX_RGB(0x1C, 0x1C, 0x22)
#define INK_SOFT    GFX_RGB(0x5E, 0x5E, 0x68)
#define LINE        GFX_RGB(0xD6, 0xD8, 0xDE)
#define FIELD       GFX_RGB(0xF0, 0xF1, 0xF4)
#define FIELD_HOT   GFX_RGB(0xE6, 0xE7, 0xEC)
#define FIELD_DOWN  GFX_RGB(0xDA, 0xDB, 0xE1)
#define ACCENT      GFX_RGB(0x2F, 0x6F, 0xEB)
#define ACCENT_HOT  GFX_RGB(0x45, 0x80, 0xF5)
#define ACCENT_DOWN GFX_RGB(0x20, 0x55, 0xC0)
#define DANGER      GFX_RGB(0xE0, 0x3E, 0x3E)
#define DANGER_HOT  GFX_RGB(0xEE, 0x50, 0x50)
#define DANGER_DOWN GFX_RGB(0xC0, 0x30, 0x30)
#define DISABLED    GFX_RGB(0xB8, 0xC6, 0xE6)

static const char *const icon_files[] = {
    [GUI_ICON_INFO] = _PATH_DDE_RES "/Icons/Dialogs/Info.png",
    [GUI_ICON_WARNING] = _PATH_DDE_RES "/Icons/Dialogs/Warning.png",
    [GUI_ICON_ERROR] = _PATH_DDE_RES "/Icons/Dialogs/Error.png",
    [GUI_ICON_QUESTION] = _PATH_DDE_RES "/Icons/Dialogs/Question.png",
};
static struct gfx_surface *icons[5];   /* loaded the first time they're needed */

/* The sound a dialog makes as it appears (/usr/share/dde-res/Sounds), by its icon. */
static const char *const icon_sounds[] = {
    [GUI_ICON_INFO] = "DialogInfo",
    [GUI_ICON_WARNING] = "DialogWarning",
    [GUI_ICON_ERROR] = "DialogError",
    [GUI_ICON_QUESTION] = "DialogQuestion",
};

struct line {
    const char *start;
    size_t len;
};

struct state {
    struct gui *g;
    struct gui_window *w;
    struct gui_dialog *d;
    struct ttf_font *regular, *bold;
    const char *labels[4];
    int count;                   /* buttons */
    struct line lines[MAX_LINES];
    int nlines;
    int height;
    int hover, pressed;          /* button under the mouse / held down (-1 none; 4: the checkbox) */
    int focus;                   /* the button Enter/Space presses */
};

static struct gfx_surface *icon(enum gui_dialog_icon which) {
    if (which <= GUI_ICON_NONE || which > GUI_ICON_QUESTION) {
        return NULL;
    }
    if (!icons[which]) {
        struct gfx_surface *img = image_load_png(icon_files[which], NULL);
        if (img) {
            icons[which] = gfx_surface_scaled(img, ICON, ICON);
            gfx_surface_destroy(img);
        }
    }
    return icons[which];
}

/* --- layout ----------------------------------------------------------------------------------------- */

static float width_of(struct state *st, const char *text, size_t len) {
    char buf[512];
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';
    return ttf_text_width(st->regular, BODY_SIZE, buf);
}

/* Break the message into lines that fit TEXT_W: at spaces, or anywhere in a word too long for a line. */
static void wrap(struct state *st) {
    st->nlines = 0;
    const char *p = st->d->message;
    if (!p) {
        return;
    }
    while (*p && st->nlines < MAX_LINES) {
        const char *end = p;
        const char *fit = NULL;     /* the last place it can break */
        while (*end && *end != '\n') {
            const char *next = end + 1;
            while ((*next & 0xC0) == 0x80) {
                next++;             /* whole UTF-8 characters */
            }
            if (width_of(st, p, (size_t)(next - p)) > TEXT_W) {
                break;
            }
            if (*end == ' ') {
                fit = end;
            }
            end = next;
        }
        if (*end && *end != '\n') {
            if (fit && fit > p) {
                end = fit;          /* break at the last space */
            } else if (end == p) {
                end++;              /* not even one character fits: take it anyway */
            }
        }
        st->lines[st->nlines++] = (struct line){ p, (size_t)(end - p) };
        p = end;
        if (*p == '\n' || *p == ' ') {
            p++;
        }
    }
}

static int text_bottom(const struct state *st) {
    int y = PAD + 22;                                    /* under the heading */
    if (st->nlines) {
        y += 10 + st->nlines * LINE_H;
    }
    if (st->d->check) {
        y += 8 + CHECK_H;
    }
    return y;
}

static void layout(struct state *st) {
    wrap(st);
    int bottom = text_bottom(st);
    if (bottom < PAD + ICON) {
        bottom = PAD + ICON;
    }
    st->height = bottom + 24 + BUTTON_H + 20;
}

static int button_width(const struct state *st, int i) {
    int w = (int)ttf_text_width(st->regular, BODY_SIZE, st->labels[i]) + 36;
    return w < BUTTON_MIN ? BUTTON_MIN : w;
}

/* Buttons from the right: [0] the main one is rightmost. */
static struct gfx_rect button_rect(const struct state *st, int i) {
    int x = WIDTH - PAD;
    for (int k = 0; k <= i; k++) {
        x -= button_width(st, k) + (k ? BUTTON_GAP : 0);
    }
    return gfx_rect(x, st->height - 20 - BUTTON_H, button_width(st, i), BUTTON_H);
}

static struct gfx_rect check_rect(const struct state *st) {
    int y = text_bottom(st) - CHECK_H;
    float w = ttf_text_width(st->regular, BODY_SIZE, st->d->check);
    return gfx_rect(TEXT_X - 2, y, (int)w + 34, CHECK_H);
}

static bool main_enabled(const struct state *st) {
    return !st->d->require_check || st->d->checked;
}

/* --- drawing ------------------------------------------------------------------------------------- */

static void draw(struct state *st) {
    struct gfx_surface *s = gui_window_surface(st->w);
    gfx_clear(s, WHITE);
    struct gfx_surface *pic = icon(st->d->icon);
    if (pic) {
        gfx_blit(s, PAD, PAD, pic, NULL);
    }
    int x = pic ? TEXT_X : PAD;
    gfx_set_clip(s, gfx_rect(x, 0, WIDTH - x - PAD + 4, st->height));
    ttf_draw_text(s, st->bold, HEAD_SIZE, (float)x, PAD + 15, INK, st->d->title ? st->d->title : "");
    int y = PAD + 22 + 10;
    for (int i = 0; i < st->nlines; i++) {
        char buf[512];
        size_t len = st->lines[i].len < sizeof(buf) - 1 ? st->lines[i].len : sizeof(buf) - 1;
        memcpy(buf, st->lines[i].start, len);
        buf[len] = '\0';
        ttf_draw_text(s, st->regular, BODY_SIZE, (float)x, (float)y + 14, INK_SOFT, buf);
        y += LINE_H;
    }
    gfx_reset_clip(s);

    if (st->d->check) {
        struct gfx_rect r = check_rect(st);
        int by = r.y + (r.h - 18) / 2;
        bool on = st->d->checked;
        gfx_fill_rounded_rect(s, gfx_rect(r.x + 2, by, 18, 18), 5, on ? ACCENT : LINE);
        if (on) {
            gfx_draw_line(s, r.x + 6.0f, by + 9.0f, r.x + 10.0f, by + 13.0f, 2, WHITE);
            gfx_draw_line(s, r.x + 10.0f, by + 13.0f, r.x + 16.0f, by + 4.0f, 2, WHITE);
        } else {
            gfx_fill_rounded_rect(s, gfx_rect(r.x + 4, by + 2, 14, 14), 4, WHITE);
        }
        ttf_draw_text(s, st->regular, BODY_SIZE, r.x + 30.0f, by + 14.0f, INK, st->d->check);
    }

    for (int i = 0; i < st->count; i++) {
        struct gfx_rect b = button_rect(st, i);
        bool main = i == 0 && st->count > 1;
        bool enabled = i != 0 || main_enabled(st);
        gfx_color fill;
        if (main || (st->count == 1)) {
            bool red = st->d->danger && i == 0;
            fill = !enabled ? DISABLED
                 : st->pressed == i ? (red ? DANGER_DOWN : ACCENT_DOWN)
                 : st->hover == i ? (red ? DANGER_HOT : ACCENT_HOT)
                 : (red ? DANGER : ACCENT);
        } else {
            fill = st->pressed == i ? FIELD_DOWN : st->hover == i ? FIELD_HOT : FIELD;
        }
        gfx_fill_rounded_rect(s, b, 8, fill);
        if (st->focus == i && enabled) {
            gfx_stroke_rounded_rect(s, gfx_rect_inset(b, -3), 10, 1.5f, GFX_RGBA(0x2F, 0x6F, 0xEB, 110));
        }
        float w = ttf_text_width(st->regular, BODY_SIZE, st->labels[i]);
        bool light = main || st->count == 1;
        ttf_draw_text(s, st->regular, BODY_SIZE, b.x + (b.w - w) / 2, (float)b.y + 22, light ? WHITE : INK,
                      st->labels[i]);
    }
    gui_window_invalidate(st->w, NULL);
}

/* --- running -------------------------------------------------------------------------------------- */

static int hit(const struct state *st, int x, int y) {
    for (int i = 0; i < st->count; i++) {
        if (gfx_rect_contains(button_rect(st, i), x, y)) {
            return i;
        }
    }
    if (st->d->check && gfx_rect_contains(check_rect(st), x, y)) {
        return 4;
    }
    return -1;
}

/* Is this event something to keep for after the dialog (not input for a window that's waiting)? */
static bool worth_keeping(const struct gui_event *ev) {
    return ev->type == GUI_EVENT_RESIZE || ev->type == GUI_EVENT_FOCUS || ev->type == GUI_EVENT_QUIT;
}

/* What pressing button i does: its index, or -2 if it does nothing right now. */
static int press(struct state *st, int i) {
    if (i == 0 && !main_enabled(st)) {
        return -2;
    }
    return st->count == 1 && !st->d->buttons[0] ? 0 : i;
}

int gui_dialog_run(struct gui *g, struct gui_window *owner, struct gui_dialog *dlg) {
    struct state st = { .g = g, .d = dlg, .hover = -1, .pressed = -1, .focus = 0 };
    st.regular = gui_font(g, GUI_FONT_REGULAR);
    st.bold = gui_font(g, GUI_FONT_BOLD);
    if (!st.regular || !st.bold) {
        return -1;
    }
    for (int i = 0; i < 4 && dlg->buttons[i]; i++) {
        st.labels[st.count++] = dlg->buttons[i];
    }
    if (!st.count) {
        st.labels[st.count++] = "OK";
    }
    if ((!main_enabled(&st) || dlg->danger) && st.count > 1) {
        st.focus = 1;   /* Enter shouldn't destroy anything by itself */
    }
    layout(&st);
    st.w = gui_dialog_window_create(g, owner, dlg->title ? dlg->title : "", WIDTH, st.height);
    if (!st.w) {
        return -1;
    }
    draw(&st);
    if (dlg->icon > GUI_ICON_NONE && dlg->icon <= GUI_ICON_QUESTION) {
        system_sound(icon_sounds[dlg->icon]);
    }

    struct gui_event kept[KEEP_MAX];
    int nkept = 0;
    int answer = -2;
    while (answer == -2) {
        struct gui_event ev;
        if (!gui_wait_event(g, &ev, -1)) {
            continue;
        }
        if (ev.type == GUI_EVENT_QUIT) {
            kept[nkept < KEEP_MAX ? nkept++ : KEEP_MAX - 1] = ev;
            answer = -1;
            break;
        }
        if (ev.window != st.w) {
            if (worth_keeping(&ev) && nkept < KEEP_MAX) {
                kept[nkept++] = ev;
            }
            continue;
        }
        switch (ev.type) {
        case GUI_EVENT_CLOSE:
            answer = -1;
            break;
        case GUI_EVENT_MOUSE_MOVE: {
            int h = ev.buttons ? (hit(&st, ev.x, ev.y) == st.pressed ? st.pressed : -1) : hit(&st, ev.x, ev.y);
            if (h != st.hover) {
                st.hover = h;
                gui_window_set_cursor(st.w, h >= 0 ? GUI_CURSOR_HAND : GUI_CURSOR_ARROW);
                draw(&st);
            }
            break;
        }
        case GUI_EVENT_MOUSE_DOWN:
            if (ev.button == OS_MOUSE_LEFT) {
                st.pressed = hit(&st, ev.x, ev.y);
                draw(&st);
            }
            break;
        case GUI_EVENT_MOUSE_UP:
            if (ev.button == OS_MOUSE_LEFT && st.pressed >= 0) {
                int h = hit(&st, ev.x, ev.y);
                int was = st.pressed;
                st.pressed = -1;
                if (h == was && h == 4) {
                    dlg->checked = !dlg->checked;
                } else if (h == was) {
                    answer = press(&st, h);
                }
                draw(&st);
            }
            break;
        case GUI_EVENT_KEY:
            if (ev.key == OS_KEY_ESCAPE) {
                answer = -1;
            } else if (ev.key == OS_KEY_ENTER) {
                answer = press(&st, st.focus == 4 ? 0 : st.focus);
            } else if (ev.key == OS_KEY_SPACE) {
                if (st.focus == 4) {
                    dlg->checked = !dlg->checked;
                } else {
                    answer = press(&st, st.focus);
                }
                draw(&st);
            } else if (ev.key == OS_KEY_TAB || ev.key == OS_KEY_LEFT || ev.key == OS_KEY_RIGHT) {
                /* Around the buttons (and the checkbox): left goes to the next button over. */
                int order[6], n = 0;
                if (dlg->check) {
                    order[n++] = 4;
                }
                for (int i = st.count - 1; i >= 0; i--) {
                    order[n++] = i;
                }
                int at = 0;
                for (int i = 0; i < n; i++) {
                    if (order[i] == st.focus) {
                        at = i;
                    }
                }
                bool back = ev.key == OS_KEY_LEFT || (ev.key == OS_KEY_TAB && (ev.modifiers & OS_MOD_SHIFT));
                st.focus = order[(at + (back ? n - 1 : 1)) % n];
                draw(&st);
            }
            break;
        default:
            break;
        }
    }
    gui_window_destroy(st.w);
    for (int i = 0; i < nkept; i++) {
        gui_push_event(g, &kept[i]);
    }
    return answer;
}

void gui_message(struct gui *g, struct gui_window *owner, enum gui_dialog_icon icon, const char *title,
                 const char *message) {
    struct gui_dialog dlg = { .icon = icon, .title = title, .message = message };
    gui_dialog_run(g, owner, &dlg);
}

bool gui_confirm(struct gui *g, struct gui_window *owner, enum gui_dialog_icon icon, const char *title,
                 const char *message, const char *yes, bool danger) {
    struct gui_dialog dlg = { .icon = icon, .title = title, .message = message, .buttons = { yes, "Cancel" },
                              .danger = danger };
    return gui_dialog_run(g, owner, &dlg) == 0;
}
