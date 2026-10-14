/*
 * The desktop's own dialog, for its icons: a card in the middle of the
 * screen that takes all input until it's answered. Rename (with a text
 * box), delete (asks first), and error messages; and while shutting down,
 * "... anyway?" (shutdown.c).
 */

#include <image.h>
#include <sound.h>
#include <string.h>

#include "desktop.h"
#include <paths.h>

#define WIDTH   460
#define LINES   5         /* of message at most */
#define ICON    44
#define FONT    13.0f
#define INK     GFX_RGB(0x1C, 0x1C, 0x22)
#define SOFT    GFX_RGB(0x78, 0x78, 0x82)
#define FIELD   GFX_RGB(0xF4, 0xF5, 0xF8)
#define DANGER  GFX_RGB(0xE0, 0x3E, 0x3E)

void icons_delete_confirmed(struct desktop *d);

/* Warning.png for "delete?", Error.png for errors (from /usr/share/dde-res/Icons/Dialogs), loaded when first shown. */
static struct gfx_surface *dialog_icon(const struct desktop *d) {
    static struct gfx_surface *warning, *error;
    struct gfx_surface **slot = d->dialog == DESK_DIALOG_DELETE || d->dialog == DESK_DIALOG_SHUTDOWN ? &warning
                              : d->dialog == DESK_DIALOG_ERROR ? &error : NULL;
    if (slot && !*slot) {
        struct gfx_surface *img = image_load_png(slot == &warning ? _PATH_DDE_RES "/Icons/Dialogs/Warning.png"
                                                                  : _PATH_DDE_RES "/Icons/Dialogs/Error.png", NULL);
        if (img) {
            *slot = gfx_surface_scaled(img, ICON, ICON);
            gfx_surface_destroy(img);
        }
    }
    return slot ? *slot : NULL;
}

static bool has_field(const struct desktop *d) {
    return d->dialog == DESK_DIALOG_RENAME;
}

/* The message broken into lines that fit `width`; returns how many. */
static int wrap(const struct desktop *d, const char *text, float width, char (*lines)[160], int max) {
    int n = 0;
    const char *p = text;
    while (*p && n < max && d->regular) {
        char line[160] = "";
        const char *end = p;
        for (;;) {
            const char *word = end;
            while (*word == ' ') {
                word++;
            }
            const char *stop = word;
            while (*stop && *stop != ' ') {
                stop++;
            }
            char tryline[160];
            size_t len = (size_t)(stop - p) < sizeof(tryline) - 1 ? (size_t)(stop - p) : sizeof(tryline) - 1;
            memcpy(tryline, p, len);
            tryline[len] = '\0';
            if (line[0] && ttf_text_width(d->regular, FONT, tryline) > width) {
                break;
            }
            strlcpy(line, tryline, sizeof(line));
            end = stop;
            if (!*stop) {
                break;
            }
        }
        if (end == p) {   /* (one word wider than the line: cut it) */
            end = p + strlen(p);
            snprintf(line, sizeof(line), "%s", p);
        }
        strlcpy(lines[n++], line, sizeof(lines[0]));
        p = end;
        while (*p == ' ') {
            p++;
        }
    }
    if (*p && n == max && n > 0) {
        strlcat(lines[n - 1], "...", sizeof(lines[0]));
    }
    return n;
}

static float text_width(void) {
    return WIDTH - 22 - ICON - 16 - 22;
}

struct gfx_rect desk_dialog_rect(const struct desktop *d) {
    int h = has_field(d) ? 160 : 150;
    if (!has_field(d)) {
        char lines[LINES][160];
        int n = wrap(d, d->dialog_message, text_width(), lines, LINES);
        h += (n > 1 ? n - 1 : 0) * 18;
    }
    struct gfx_rect area = work_area(d);
    return gfx_rect((area.w - WIDTH) / 2, (area.h - h) / 2, WIDTH, h);
}

/* Bounds including the shadow, for redrawing. */
static struct gfx_rect bounds(const struct desktop *d) {
    struct gfx_rect r = gfx_rect_inset(desk_dialog_rect(d), -24);
    r.h += 8;
    return r;
}

static int buttons(const struct desktop *d, const char **labels) {
    switch (d->dialog) {
    case DESK_DIALOG_RENAME: labels[0] = "Rename"; labels[1] = "Cancel"; return 2;
    case DESK_DIALOG_DELETE: labels[0] = "Delete"; labels[1] = "Cancel"; return 2;
    case DESK_DIALOG_SHUTDOWN: labels[0] = d->dialog_field; labels[1] = "Cancel"; return 2;   /* "Shut down anyway" */
    default:                 labels[0] = "OK"; return 1;
    }
}

static bool danger(const struct desktop *d) {
    return d->dialog == DESK_DIALOG_DELETE || d->dialog == DESK_DIALOG_SHUTDOWN;
}

/* Right to left: the main button, then Cancel; each as wide as its label needs (at least 88). */
static struct gfx_rect button_rect(const struct desktop *d, int i) {
    struct gfx_rect r = desk_dialog_rect(d);
    const char *labels[2];
    buttons(d, labels);
    int right = r.x + r.w - 18;
    for (int k = 0;; k++) {
        int w = d->regular ? (int)ttf_text_width(d->regular, FONT, labels[k]) + 32 : 88;
        w = w < 88 ? 88 : w;
        if (k == i) {
            return gfx_rect(right - w, r.y + r.h - 50, w, 32);
        }
        right -= w + 10;
    }
}

void desk_dialog_open(struct desktop *d, enum desk_dialog kind, const char *title, const char *message,
                      const char *field, const char *target) {
    if (d->dialog != DESK_DIALOG_NONE) {
        damage(d, bounds(d));
    }
    menu_close(d);
    launcher_close(d);
    d->dialog = kind;
    strlcpy(d->dialog_title, title, sizeof(d->dialog_title));
    strlcpy(d->dialog_message, message ? message : "", sizeof(d->dialog_message));
    strlcpy(d->dialog_field, field ? field : "", sizeof(d->dialog_field));
    strlcpy(d->dialog_target, target ? target : "", sizeof(d->dialog_target));
    d->dialog_cursor = (int)strlen(d->dialog_field);
    /* Renaming "name.txt": start with the cursor before the extension. */
    const char *dot = strrchr(d->dialog_field, '.');
    if (dot && dot != d->dialog_field) {
        d->dialog_cursor = (int)(dot - d->dialog_field);
    }
    d->dialog_hover = d->dialog_pressed = -1;
    damage(d, bounds(d));
    if (kind == DESK_DIALOG_DELETE || kind == DESK_DIALOG_ERROR || kind == DESK_DIALOG_SHUTDOWN) {
        system_sound(kind == DESK_DIALOG_ERROR ? "DialogError" : "DialogWarning");
    }
}

static void close_dialog(struct desktop *d) {
    damage(d, bounds(d));
    d->dialog = DESK_DIALOG_NONE;
}

void desk_dialog_compose(struct desktop *d, struct gfx_rect area) {
    if (d->dialog == DESK_DIALOG_NONE || gfx_rect_empty(gfx_rect_intersect(bounds(d), area)) || !d->regular) {
        return;
    }
    struct gfx_surface *s = &d->screen;
    struct gfx_rect r = desk_dialog_rect(d);
    for (int i = 3; i >= 1; i--) {   /* a soft shadow */
        gfx_fill_rounded_rect(s, gfx_rect(r.x - i * 2, r.y - i * 2 + 4, r.w + i * 4, r.h + i * 4), 14 + i * 2,
                              GFX_RGBA(0, 0, 0, 22));
    }
    gfx_fill_rounded_rect(s, r, 14, GFX_RGB(255, 255, 255));
    int x = r.x + 22, y = r.y + 36;
    struct gfx_surface *icon = dialog_icon(d);
    if (icon) {
        gfx_blit(s, x, r.y + 22, icon, NULL);
        x += ICON + 16;
    }
    ttf_draw_text(s, d->bold, 16, x, y, INK, d->dialog_title);
    if (has_field(d)) {
        struct gfx_rect box = gfx_rect(x, y + 16, r.w - 44, 36);
        gfx_fill_rounded_rect(s, box, 8, FIELD);
        gfx_stroke_rounded_rect(s, box, 8, 1.5f, ACCENT);
        struct gfx_rect clip = gfx_rect_intersect(area, gfx_rect_inset(box, 4));
        gfx_set_clip(s, clip);
        ttf_draw_text(s, d->regular, FONT, box.x + 12, box.y + 23, INK, d->dialog_field);
        char before[OS_NAME_MAX];
        strlcpy(before, d->dialog_field, sizeof(before));
        before[d->dialog_cursor] = '\0';
        float cx = box.x + 12 + ttf_text_width(d->regular, FONT, before);
        gfx_fill_rect(s, gfx_rect((int)cx, box.y + 9, 1, 18), INK);
        gfx_set_clip(s, area);
    } else {
        char lines[LINES][160];
        int n = wrap(d, d->dialog_message, text_width(), lines, LINES);
        gfx_set_clip(s, gfx_rect_intersect(area, gfx_rect(x, r.y, r.x + r.w - 18 - x, r.h)));
        for (int i = 0; i < n; i++) {
            ttf_draw_text(s, d->regular, FONT, x, y + 30 + i * 18, SOFT, lines[i]);
        }
        gfx_set_clip(s, area);
    }
    const char *labels[2];
    int n = buttons(d, labels);
    for (int i = 0; i < n; i++) {
        struct gfx_rect b = button_rect(d, i);
        bool main = i == 0;
        gfx_color fill = main ? (danger(d) ? DANGER : ACCENT) : FIELD;
        if (d->dialog_hover == i) {
            fill = main ? (danger(d) ? GFX_RGB(0xEE, 0x50, 0x50) : GFX_RGB(0x45, 0x80, 0xF5))
                        : GFX_RGB(0xEB, 0xEC, 0xF0);
        }
        gfx_fill_rounded_rect(s, b, 8, fill);
        float w = ttf_text_width(d->regular, FONT, labels[i]);
        ttf_draw_text(s, d->regular, FONT, b.x + (b.w - w) / 2, b.y + 21,
                      main ? GFX_RGB(255, 255, 255) : INK, labels[i]);
    }
}

static void error(struct desktop *d, const char *title, const char *what, int err) {
    char message[160];
    snprintf(message, sizeof(message), "%s: %s", what, strerror(err));
    desk_dialog_open(d, DESK_DIALOG_ERROR, title, message, NULL, NULL);
}

/* Cancel, Escape: the dialog goes (and shutting down stops). */
static void cancel(struct desktop *d) {
    enum desk_dialog kind = d->dialog;
    close_dialog(d);
    if (kind == DESK_DIALOG_SHUTDOWN) {
        shutdown_answer(d, false);
    }
}

static void accept(struct desktop *d) {
    enum desk_dialog kind = d->dialog;
    close_dialog(d);
    if (kind == DESK_DIALOG_SHUTDOWN) {
        shutdown_answer(d, true);
    } else if (kind == DESK_DIALOG_DELETE) {
        icons_delete_confirmed(d);
    } else if (kind == DESK_DIALOG_RENAME) {
        const char *name = d->dialog_field;
        if (!name[0] || strchr(name, '/') || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            error(d, "Can't rename", name[0] ? name : "(empty)", -EINVAL);
            return;
        }
        char dir[FILE_PATH_MAX], path[FILE_PATH_MAX];
        path_parent(d->dialog_target, dir, sizeof(dir));
        path_join(path, sizeof(path), dir, name);
        int err = strcmp(path, d->dialog_target) == 0 ? 0 : rename(d->dialog_target, path);
        if (err < 0) {
            error(d, "Can't rename", name, err);
            return;
        }
        icons_renamed(d, path_base(d->dialog_target), name);   /* it keeps its place */
        icons_reload(d);
    }
}

static int button_at(struct desktop *d) {
    const char *labels[2];
    int n = buttons(d, labels);
    for (int i = 0; i < n; i++) {
        if (gfx_rect_contains(button_rect(d, i), d->mouse_x, d->mouse_y)) {
            return i;
        }
    }
    return -1;
}

void desk_dialog_mouse(struct desktop *d, bool down, bool up) {
    int b = button_at(d);
    if (b != d->dialog_hover) {
        d->dialog_hover = b;
        damage(d, desk_dialog_rect(d));
    }
    if (down) {
        d->dialog_pressed = b;
    } else if (up) {
        int pressed = d->dialog_pressed;
        d->dialog_pressed = -1;
        if (pressed >= 0 && pressed == b) {
            if (b == 0) {
                accept(d);
            } else {
                cancel(d);
            }
        }
    }
}

void desk_dialog_key(struct desktop *d, const struct os_input_event *in) {
    if (in->key == OS_KEY_ESCAPE) {
        cancel(d);
        return;
    }
    if (in->key == OS_KEY_ENTER) {
        accept(d);
        return;
    }
    if (!has_field(d)) {
        return;
    }
    char *f = d->dialog_field;
    int len = (int)strlen(f), *c = &d->dialog_cursor;
    switch (in->key) {
    case OS_KEY_LEFT:  *c -= *c > 0; break;
    case OS_KEY_RIGHT: *c += *c < len; break;
    case OS_KEY_HOME:  *c = 0; break;
    case OS_KEY_END:   *c = len; break;
    case OS_KEY_BACKSPACE:
        if (*c > 0) {
            memmove(f + *c - 1, f + *c, (size_t)(len - *c + 1));
            (*c)--;
        }
        break;
    case OS_KEY_DELETE:
        if (*c < len) {
            memmove(f + *c, f + *c + 1, (size_t)(len - *c));
        }
        break;
    default:
        if (in->ch >= ' ' && in->ch < 0x7F && in->ch != '/' && len < OS_NAME_MAX - 1 &&
            !(in->modifiers & OS_MOD_CTRL)) {
            memmove(f + *c + 1, f + *c, (size_t)(len - *c + 1));
            f[(*c)++] = (char)in->ch;
        }
        break;
    }
    damage(d, desk_dialog_rect(d));
}
