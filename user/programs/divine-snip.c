/*
 * Snipping Tool: pictures of the screen.
 *
 *   New      take a snip: the screen freezes (dimmed) and you pick what to keep
 *   Mode     Free-form (draw around it), Rectangular (drag a box), Window (click one), Full-screen
 *   Delay    wait 1-5 seconds first (the tool hides meanwhile: open a menu, hover something...)
 *
 * The snip opens in the tool, where it can be marked up (Pen, Highlighter, Undo or Ctrl+Z) and
 * saved (Save or Ctrl+S) as a PNG in ~/Pictures/Screenshots. Escape cancels a snip.
 * `divine-snip --new` (Print Screen on the desktop) starts a rectangular snip right away.
 *
 * The desktop takes the picture (gui_screenshot): without this program's windows or the pointer.
 */

#include <fileops.h>
#include <gui.h>
#include <image.h>
#include <math.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TOOLBAR_H     52
#define STATUS_H      30
#define START_W       470
#define START_H       (TOOLBAR_H + 48)
#define BUTTON_H      34
#define MAX_STROKES   256
#define MAX_POINTS    4096        /* a free-form outline, and all strokes' points together */

#define WHITE         GFX_RGB(255, 255, 255)
#define INK           GFX_RGB(0x1C, 0x1C, 0x22)
#define INK_SOFT      GFX_RGB(0x6A, 0x6A, 0x74)
#define INK_FAINT     GFX_RGB(0xB8, 0xB8, 0xC0)
#define LINE          GFX_RGB(0xEA, 0xEA, 0xEE)
#define HOVER         GFX_RGB(0xEE, 0xEF, 0xF3)
#define PRESSED       GFX_RGB(0xE2, 0xE4, 0xEA)
#define ON            GFX_RGB(0xDD, 0xE8, 0xFC)
#define BACKDROP      GFX_RGB(0xE9, 0xEA, 0xEE)
#define ACCENT        GFX_RGB(0x2F, 0x6F, 0xEB)
#define PEN_COLOR     GFX_RGB(0xE0, 0x2E, 0x2E)
#define HIGHLIGHT     GFX_RGBA(0xFF, 0xD8, 0x00, 110)
#define SHADE         GFX_RGBA(0, 0, 0, 110)

enum mode { MODE_FREEFORM, MODE_RECT, MODE_WINDOW, MODE_FULLSCREEN, MODE_COUNT };
enum tool { TOOL_NONE, TOOL_PEN, TOOL_HIGHLIGHTER };
enum button { B_NEW, B_MODE, B_DELAY, B_SAVE, B_PEN, B_HIGHLIGHTER, B_UNDO, B_COUNT, B_NONE = -1 };
enum { MENU_MODE = 100, MENU_DELAY = 200 };

static const char *const mode_names[MODE_COUNT] = {
    "Free-form Snip", "Rectangular Snip", "Window Snip", "Full-screen Snip",
};

struct stroke {
    enum tool tool;
    int first, count;            /* in points[] */
};

static struct {
    struct gui *g;
    struct ttf_font *ui, *bold;
    struct gfx_surface *logo;    /* the app's icon, for New */
    enum mode mode;
    int delay;                   /* seconds */
    bool exit_on_cancel;         /* started by Print Screen with nothing else open */

    /* The tool's window: the toolbar, then the snip (when there's one). */
    struct gui_window *w;
    int hover, pressed;
    char status[160];
    int menu_x, menu_y;          /* the window's content corner on the screen (for menus) */

    /* Taking a snip: the frozen screen, in a full-screen window. */
    struct gui_window *overlay;
    struct gui_screenshot *shot;
    bool dragging;
    int ax, ay, bx, by;          /* the rectangle being dragged (screen) */
    int hover_window;            /* Window Snip: under the pointer, or -1 */
    int outline[MAX_POINTS][2];  /* Free-form: the path drawn */
    int outline_count;

    /* The snip, and the markup on it. */
    struct gfx_surface *snip;    /* as taken */
    struct gfx_surface *marked;  /* with the strokes */
    enum tool tool;
    struct stroke strokes[MAX_STROKES];
    int stroke_count;
    float points[MAX_POINTS][2]; /* in snip pixels */
    int point_count;
    bool drawing;
    bool saved;
} t;

/* --- the toolbar ---------------------------------------------------------------------------------- */

static int button_count(void) {
    return t.snip ? B_COUNT : B_SAVE;
}

static const char *button_label(int b) {
    static char delay[24];
    switch (b) {
    case B_NEW: return "New";
    case B_MODE: return "Mode";
    case B_DELAY:
        if (t.delay) {
            snprintf(delay, sizeof(delay), "Delay: %ds", t.delay);
            return delay;
        }
        return "Delay";
    case B_SAVE: return "Save";
    case B_PEN: return "Pen";
    case B_HIGHLIGHTER: return "Highlighter";
    default: return "Undo";
    }
}

static bool has_arrow(int b) {
    return b == B_MODE || b == B_DELAY;
}

static struct gfx_rect button_rect(int b) {
    int x = 10;
    for (int i = 0; i < b; i++) {
        x += (int)ttf_text_width(t.ui, 13, button_label(i)) + 46 + (has_arrow(i) ? 16 : 0) + 4;
        if (i == B_DELAY) {
            x += 14;   /* a gap before the snip's own tools */
        }
    }
    int w = (int)ttf_text_width(t.ui, 13, button_label(b)) + 46 + (has_arrow(b) ? 16 : 0);
    return gfx_rect(x, (TOOLBAR_H - BUTTON_H) / 2, w, BUTTON_H);
}

static bool button_enabled(int b) {
    return b != B_UNDO || t.stroke_count > 0;
}

static bool button_on(int b) {
    return (b == B_PEN && t.tool == TOOL_PEN) || (b == B_HIGHLIGHTER && t.tool == TOOL_HIGHLIGHTER);
}

/* The little pictures on the buttons. */
static void draw_glyph(struct gfx_surface *s, int b, float cx, float cy, gfx_color ink) {
    switch (b) {
    case B_NEW:
        if (t.logo) {
            gfx_blit(s, (int)cx - t.logo->width / 2, (int)cy - t.logo->height / 2, t.logo, NULL);
        }
        break;
    case B_MODE:   /* a dashed box */
        gfx_fill_rect(s, gfx_rect((int)cx - 8, (int)cy - 6, 16, 12), GFX_RGBA(0x2F, 0x6F, 0xEB, 50));
        for (int i = 0; i < 16; i += 4) {
            gfx_fill_rect(s, gfx_rect((int)cx - 8 + i, (int)cy - 6, 2, 1), ACCENT);
            gfx_fill_rect(s, gfx_rect((int)cx - 8 + i, (int)cy + 5, 2, 1), ACCENT);
        }
        for (int i = 0; i < 12; i += 4) {
            gfx_fill_rect(s, gfx_rect((int)cx - 8, (int)cy - 6 + i, 1, 2), ACCENT);
            gfx_fill_rect(s, gfx_rect((int)cx + 7, (int)cy - 6 + i, 1, 2), ACCENT);
        }
        break;
    case B_DELAY:  /* a clock */
        gfx_fill_circle(s, cx, cy, 8, ink);
        gfx_fill_circle(s, cx, cy, 6.6f, WHITE);
        gfx_draw_line(s, cx, cy, cx, cy - 4.5f, 1.4f, ink);
        gfx_draw_line(s, cx, cy, cx + 3.5f, cy + 1.5f, 1.4f, ink);
        break;
    case B_SAVE:   /* a floppy */
        gfx_fill_rounded_rect(s, gfx_rect((int)cx - 7, (int)cy - 7, 14, 14), 2, ACCENT);
        gfx_fill_rect(s, gfx_rect((int)cx - 4, (int)cy - 7, 8, 5), WHITE);
        gfx_fill_rect(s, gfx_rect((int)cx - 4, (int)cy + 2, 8, 5), GFX_RGB(0xC9, 0xDA, 0xFA));
        break;
    case B_PEN:
        gfx_draw_line(s, cx - 6, cy + 6, cx + 5, cy - 5, 3.2f, PEN_COLOR);
        gfx_draw_line(s, cx - 7, cy + 7, cx - 5, cy + 5, 1.5f, ink);
        break;
    case B_HIGHLIGHTER:
        gfx_fill_rounded_rect(s, gfx_rect((int)cx - 8, (int)cy - 3, 16, 7), 2, GFX_RGB(0xFF, 0xD0, 0x00));
        break;
    case B_UNDO:
        gfx_draw_line(s, cx - 6, cy - 1, cx + 3, cy - 1, 1.6f, ink);
        gfx_draw_line(s, cx + 3, cy - 1, cx + 6, cy + 3, 1.6f, ink);
        gfx_draw_line(s, cx - 6, cy - 1, cx - 2, cy - 5, 1.6f, ink);
        gfx_draw_line(s, cx - 6, cy - 1, cx - 2, cy + 3, 1.6f, ink);
        break;
    }
}

/* Where the snip is shown, and how big. */
static void view(int *x, int *y, float *scale) {
    struct gfx_surface *s = gui_window_surface(t.w);
    int aw = s->width - 24, ah = s->height - TOOLBAR_H - STATUS_H - 24;
    float sc = 1;
    if (t.marked) {
        float sx = (float)aw / t.marked->width, sy = (float)ah / t.marked->height;
        sc = sx < sy ? sx : sy;
        sc = sc > 1 ? 1 : sc;
        *x = (s->width - (int)(t.marked->width * sc)) / 2;
        *y = TOOLBAR_H + 12 + (ah - (int)(t.marked->height * sc)) / 2;
    } else {
        *x = *y = 0;
    }
    *scale = sc;
}

static void draw(void) {
    struct gfx_surface *s = gui_window_surface(t.w);
    gfx_clear(s, t.snip ? BACKDROP : WHITE);
    gfx_fill_rect(s, gfx_rect(0, 0, s->width, TOOLBAR_H), WHITE);
    gfx_fill_rect(s, gfx_rect(0, TOOLBAR_H - 1, s->width, 1), LINE);
    for (int b = 0; b < button_count(); b++) {
        struct gfx_rect r = button_rect(b);
        bool enabled = button_enabled(b);
        if (button_on(b)) {
            gfx_fill_rounded_rect(s, r, 7, ON);
        } else if (enabled && t.pressed == b) {
            gfx_fill_rounded_rect(s, r, 7, PRESSED);
        } else if (enabled && t.hover == b) {
            gfx_fill_rounded_rect(s, r, 7, HOVER);
        }
        gfx_color ink = enabled ? INK : INK_FAINT;
        draw_glyph(s, b, r.x + 18, r.y + r.h / 2.0f, ink);
        ttf_draw_text(s, t.ui, 13, r.x + 34, r.y + r.h / 2.0f + 5, ink, button_label(b));
        if (has_arrow(b)) {
            float ax = r.x + r.w - 15, ay = r.y + r.h / 2.0f;
            gfx_draw_line(s, ax - 3.5f, ay - 1.5f, ax, ay + 2, 1.4f, INK_SOFT);
            gfx_draw_line(s, ax, ay + 2, ax + 3.5f, ay - 1.5f, 1.4f, INK_SOFT);
        }
    }
    if (!t.snip) {
        char hint[160];
        snprintf(hint, sizeof(hint), "%s: click New, or press Print Screen anywhere.", mode_names[t.mode]);
        ttf_draw_text(s, t.ui, 13, 16, TOOLBAR_H + 28, INK_SOFT, hint);
    } else {
        int x, y;
        float sc;
        view(&x, &y, &sc);
        struct gfx_rect dst = gfx_rect(x, y, (int)(t.marked->width * sc + 0.5f), (int)(t.marked->height * sc + 0.5f));
        gfx_fill_rect(s, gfx_rect_inset(dst, -1), GFX_RGBA(0, 0, 0, 40));
        if (sc == 1) {
            gfx_blit(s, x, y, t.marked, NULL);
        } else {
            gfx_draw_scaled(s, dst, t.marked, NULL);
        }
        gfx_fill_rect(s, gfx_rect(0, s->height - STATUS_H, s->width, STATUS_H), WHITE);
        gfx_fill_rect(s, gfx_rect(0, s->height - STATUS_H, s->width, 1), LINE);
        char size[48];
        snprintf(size, sizeof(size), "%d x %d", t.snip->width, t.snip->height);
        ttf_draw_text(s, t.ui, 12, 12, s->height - STATUS_H / 2.0f + 4, INK_SOFT, t.status[0] ? t.status : size);
        if (t.status[0]) {
            float w = ttf_text_width(t.ui, 12, size);
            ttf_draw_text(s, t.ui, 12, s->width - 12 - w, s->height - STATUS_H / 2.0f + 4, INK_SOFT, size);
        }
    }
    gui_window_invalidate(t.w, NULL);
}

/* The tool's window: small with just the toolbar, or big enough for the snip. */
static void make_window(void) {
    int w = START_W, h = START_H;
    if (t.snip) {
        int sw = gui_screen_width(t.g), sh = gui_screen_height(t.g);
        w = t.snip->width + 24;
        h = t.snip->height + TOOLBAR_H + STATUS_H + 24;
        int least = button_rect(B_COUNT - 1).x + button_rect(B_COUNT - 1).w + 12;   /* the whole toolbar */
        least = least < 640 ? 640 : least;
        w = w < least ? least : w > sw * 85 / 100 ? sw * 85 / 100 : w;
        h = h < 300 ? 300 : h > sh * 80 / 100 ? sh * 80 / 100 : h;
    }
    if (t.w) {
        gui_window_destroy(t.w);
    }
    t.w = gui_window_create(t.g, "Snipping Tool", -1, -1, w, h);
    t.hover = t.pressed = B_NONE;
    if (t.w) {
        draw();
    }
}

/* --- markup --------------------------------------------------------------------------------------- */

static void paint_segment(struct gfx_surface *s, enum tool tool, float x0, float y0, float x1, float y1) {
    if (tool == TOOL_PEN) {
        gfx_draw_line(s, x0, y0, x1, y1, 3.5f, PEN_COLOR);
        gfx_fill_circle(s, x1, y1, 1.75f, PEN_COLOR);
    } else {
        gfx_draw_line(s, x0, y0, x1, y1, 16, HIGHLIGHT);
    }
}

static void paint_stroke(struct gfx_surface *s, const struct stroke *st) {
    const float (*p)[2] = &t.points[st->first];
    if (st->count == 1) {
        paint_segment(s, st->tool, p[0][0], p[0][1], p[0][0] + 0.5f, p[0][1]);
    }
    for (int i = 1; i < st->count; i++) {
        paint_segment(s, st->tool, p[i - 1][0], p[i - 1][1], p[i][0], p[i][1]);
    }
}

/* The marked-up picture again from the snip and the strokes (after Undo). */
static void repaint(void) {
    struct gfx_rect all = gfx_rect(0, 0, t.snip->width, t.snip->height);
    gfx_copy(t.marked, 0, 0, t.snip, &all);
    for (int i = 0; i < t.stroke_count; i++) {
        paint_stroke(t.marked, &t.strokes[i]);
    }
}

static bool to_snip(int wx, int wy, float *x, float *y) {
    int vx, vy;
    float sc;
    view(&vx, &vy, &sc);
    *x = (wx - vx) / sc;
    *y = (wy - vy) / sc;
    return *x >= -8 && *y >= -8 && *x < t.snip->width + 8 && *y < t.snip->height + 8;
}

static void stroke_begin(int wx, int wy) {
    float x, y;
    if (!to_snip(wx, wy, &x, &y) || t.stroke_count == MAX_STROKES || t.point_count == MAX_POINTS) {
        return;
    }
    struct stroke *st = &t.strokes[t.stroke_count++];
    st->tool = t.tool;
    st->first = t.point_count;
    st->count = 1;
    t.points[t.point_count][0] = x;
    t.points[t.point_count][1] = y;
    t.point_count++;
    t.drawing = true;
    t.saved = false;
    paint_stroke(t.marked, st);
}

static void stroke_move(int wx, int wy) {
    float x, y;
    to_snip(wx, wy, &x, &y);
    struct stroke *st = &t.strokes[t.stroke_count - 1];
    const float *last = t.points[st->first + st->count - 1];
    if (t.point_count == MAX_POINTS || (fabsf(last[0] - x) < 1.5f && fabsf(last[1] - y) < 1.5f)) {
        return;
    }
    if (st->tool == TOOL_HIGHLIGHTER) {
        /* (redrawn whole: a highlighter's overlapping pieces would otherwise get darker) */
        t.points[t.point_count][0] = x;
        t.points[t.point_count][1] = y;
        t.point_count++;
        st->count++;
        repaint();
        return;
    }
    paint_segment(t.marked, st->tool, last[0], last[1], x, y);
    t.points[t.point_count][0] = x;
    t.points[t.point_count][1] = y;
    t.point_count++;
    st->count++;
}

static void undo(void) {
    if (t.stroke_count) {
        t.stroke_count--;
        t.point_count = t.strokes[t.stroke_count].first;
        repaint();
        t.saved = false;
    }
}

/* --- saving --------------------------------------------------------------------------------------- */

static void save(void) {
    char dir[OS_PATH_MAX], path[OS_PATH_MAX], name[64];
    snprintf(dir, sizeof(dir), "%s/Pictures", home_dir());
    mkdir(dir);
    strlcat(dir, "/Screenshots", sizeof(dir));
    mkdir(dir);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(name, sizeof(name), "Screenshot_%04d-%02d-%02d_%02d-%02d-%02d.png", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    free_name(dir, name, false, path, sizeof(path));
    int r = image_save_png(path, t.marked);
    if (r < 0) {
        snprintf(t.status, sizeof(t.status), "Couldn't save it: %s", strerror(r));
        return;
    }
    const char *home = home_dir();
    size_t hl = strlen(home);
    snprintf(t.status, sizeof(t.status), "Saved as %s%s", strncmp(path, home, hl) == 0 ? "~" : "",
             strncmp(path, home, hl) == 0 ? path + hl : path);
    t.saved = true;
}

/* --- taking a snip -------------------------------------------------------------------------------- */

static void set_snip(struct gfx_surface *snip) {
    gfx_surface_destroy(t.snip);
    gfx_surface_destroy(t.marked);
    t.snip = snip;
    t.marked = gfx_surface_create(snip->width, snip->height);
    t.stroke_count = t.point_count = 0;
    t.tool = TOOL_NONE;
    t.status[0] = '\0';
    t.saved = false;
    repaint();
    make_window();
}

static struct gfx_rect drag_rect(void) {
    int x0 = t.ax < t.bx ? t.ax : t.bx, y0 = t.ay < t.by ? t.ay : t.by;
    return gfx_rect(x0, y0, abs(t.bx - t.ax) + 1, abs(t.by - t.ay) + 1);
}

static void close_overlay(void) {
    if (t.overlay) {
        gui_window_destroy(t.overlay);
        t.overlay = NULL;
    }
    gui_screenshot_free(t.shot);
    t.shot = NULL;
    t.dragging = false;
}

static void cancel_snip(void) {
    close_overlay();
    if (t.exit_on_cancel && !t.snip) {
        gui_close(t.g);
        exit(0);
    }
    gui_window_show(t.w, true);
    gui_window_raise(t.w);
}

/* A copy of part of the screen. */
static struct gfx_surface *cut(struct gfx_rect r) {
    struct gfx_surface *img = t.shot->image;
    r = gfx_rect_intersect(r, gfx_rect(0, 0, img->width, img->height));
    if (r.w < 2 || r.h < 2) {
        return NULL;
    }
    struct gfx_surface *out = gfx_surface_create(r.w, r.h);
    if (out) {
        gfx_copy(out, 0, 0, img, &r);
    }
    return out;
}

/* Free-form: the path's bounding box, white outside the path (filled row by row: even-odd). */
static struct gfx_surface *cut_freeform(void) {
    int n = t.outline_count;
    if (n < 3) {
        return NULL;
    }
    int x0 = t.outline[0][0], x1 = x0, y0 = t.outline[0][1], y1 = y0;
    for (int i = 1; i < n; i++) {
        x0 = t.outline[i][0] < x0 ? t.outline[i][0] : x0;
        x1 = t.outline[i][0] > x1 ? t.outline[i][0] : x1;
        y0 = t.outline[i][1] < y0 ? t.outline[i][1] : y0;
        y1 = t.outline[i][1] > y1 ? t.outline[i][1] : y1;
    }
    struct gfx_surface *out = cut(gfx_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1));
    if (!out) {
        return NULL;
    }
    static float cross[MAX_POINTS];
    for (int row = 0; row < out->height; row++) {
        float y = y0 + row + 0.5f;
        int k = 0;
        for (int i = 0; i < n; i++) {
            const int *a = t.outline[i], *b = t.outline[(i + 1) % n];
            if ((a[1] <= y && b[1] > y) || (b[1] <= y && a[1] > y)) {
                cross[k++] = a[0] + (y - a[1]) / (float)(b[1] - a[1]) * (b[0] - a[0]) - x0;
            }
        }
        for (int i = 1; i < k; i++) {   /* (few: insertion sort) */
            for (int j = i; j > 0 && cross[j - 1] > cross[j]; j--) {
                float tmp = cross[j];
                cross[j] = cross[j - 1];
                cross[j - 1] = tmp;
            }
        }
        uint32_t *px = gfx_row(out, row);
        int x = 0;
        for (int i = 0; i + 1 < k; i += 2) {
            int in0 = (int)ceilf(cross[i] - 0.5f), in1 = (int)floorf(cross[i + 1] - 0.5f);
            for (; x < in0 && x < out->width; x++) {
                px[x] = WHITE;
            }
            x = in1 + 1 > x ? in1 + 1 : x;
        }
        for (; x < out->width; x++) {
            px[x] = WHITE;
        }
    }
    return out;
}

static void finish_snip(struct gfx_surface *snip) {
    close_overlay();
    if (!snip) {
        cancel_snip();
        return;
    }
    set_snip(snip);
}

static void draw_overlay(void) {
    if (!t.overlay || !t.shot) {
        return;
    }
    struct gfx_surface *s = gui_window_surface(t.overlay);
    struct gfx_surface *img = t.shot->image;
    struct gfx_rect all = gfx_rect(0, 0, img->width, img->height);
    gfx_copy(s, 0, 0, img, &all);
    /* Everything dimmed but what's being picked. */
    struct gfx_rect keep = gfx_rect(0, 0, 0, 0);
    if (t.mode == MODE_RECT && t.dragging) {
        keep = drag_rect();
    } else if (t.mode == MODE_WINDOW && t.hover_window >= 0) {
        keep = gfx_rect_intersect(t.shot->windows[t.hover_window].frame, all);
    }
    if (t.mode == MODE_FREEFORM) {
        gfx_fill_rect(s, all, SHADE);
        for (int i = 1; i < t.outline_count; i++) {
            gfx_draw_line(s, t.outline[i - 1][0], t.outline[i - 1][1], t.outline[i][0], t.outline[i][1], 2.5f,
                          PEN_COLOR);
        }
    } else {
        struct gfx_rect bands[4] = {
            { 0, 0, all.w, keep.y },
            { 0, keep.y, keep.x, keep.h },
            { keep.x + keep.w, keep.y, all.w - keep.x - keep.w, keep.h },
            { 0, keep.y + keep.h, all.w, all.h - keep.y - keep.h },
        };
        if (gfx_rect_empty(keep)) {
            gfx_fill_rect(s, all, SHADE);
        } else {
            for (int i = 0; i < 4; i++) {
                if (!gfx_rect_empty(bands[i])) {
                    gfx_fill_rect(s, bands[i], SHADE);
                }
            }
            gfx_stroke_rounded_rect(s, gfx_rect_inset(keep, -1), 0, 2, ACCENT);
            if (t.mode == MODE_RECT) {
                char size[32];
                snprintf(size, sizeof(size), "%d x %d", keep.w, keep.h);
                float w = ttf_text_width(t.ui, 12, size);
                int ly = keep.y + keep.h + 26 < all.h ? keep.y + keep.h + 8 : keep.y - 28;
                struct gfx_rect tag = gfx_rect(keep.x, ly, (int)w + 16, 22);
                gfx_fill_rounded_rect(s, tag, 6, GFX_RGBA(0, 0, 0, 170));
                ttf_draw_text(s, t.ui, 12, tag.x + 8, tag.y + 15, WHITE, size);
            }
        }
    }
    /* What to do, at the top. */
    const char *hint = t.mode == MODE_FREEFORM ? "Draw around what you want to snip.  Esc: cancel"
                     : t.mode == MODE_WINDOW   ? "Click the window to snip.  Esc: cancel"
                                               : "Drag to snip an area.  Esc: cancel";
    float w = ttf_text_width(t.ui, 13.5f, hint);
    struct gfx_rect pill = gfx_rect((all.w - (int)w) / 2 - 18, 18, (int)w + 36, 36);
    gfx_fill_rounded_rect(s, pill, 18, GFX_RGBA(0x20, 0x20, 0x26, 220));
    ttf_draw_text(s, t.ui, 13.5f, pill.x + 18, pill.y + 23, WHITE, hint);
    gui_window_invalidate(t.overlay, NULL);
}

static int window_at(int x, int y) {
    for (int i = 0; i < t.shot->window_count; i++) {   /* (top first) */
        if (gfx_rect_contains(t.shot->windows[i].frame, x, y)) {
            return i;
        }
    }
    return -1;
}

/* Wait `seconds`, still answering the desktop (the tool's window is hidden meanwhile). */
static void wait_seconds(int seconds) {
    uint64_t end = uptime_ms() + (uint64_t)seconds * 1000;
    struct gui_event ev;
    for (uint64_t now; (now = uptime_ms()) < end;) {
        if (gui_wait_event(t.g, &ev, (int)(end - now)) && ev.type == GUI_EVENT_QUIT) {
            exit(0);
        }
    }
}

static void new_snip(void) {
    if (t.delay > 0) {
        gui_window_show(t.w, false);
        wait_seconds(t.delay);
    }
    t.shot = gui_screenshot(t.g);
    if (!t.shot) {
        gui_window_show(t.w, true);
        gui_message(t.g, t.w, GUI_ICON_ERROR, "Couldn't take a snip", "The desktop didn't give a picture of the screen.");
        return;
    }
    if (t.mode == MODE_FULLSCREEN) {
        struct gfx_surface *img = t.shot->image;
        t.shot->image = NULL;
        finish_snip(img);
        return;
    }
    gui_window_show(t.w, false);
    int sw = gui_screen_width(t.g), sh = gui_screen_height(t.g);
    t.overlay = gui_window_create(t.g, "Snip", 0, 0, sw, sh);
    if (!t.overlay) {
        cancel_snip();
        return;
    }
    gui_window_set_fullscreen(t.overlay, true);
    t.outline_count = 0;
    t.hover_window = -1;
    t.dragging = false;
    draw_overlay();
}

static void overlay_event(const struct gui_event *ev) {
    switch (ev->type) {
    case GUI_EVENT_RESIZE:
        draw_overlay();
        break;
    case GUI_EVENT_KEY:
        if (ev->key == OS_KEY_ESCAPE) {
            cancel_snip();
        }
        break;
    case GUI_EVENT_CLOSE:
        cancel_snip();
        break;
    case GUI_EVENT_MOUSE_DOWN:
        if (ev->button == OS_MOUSE_RIGHT) {
            cancel_snip();
            break;
        }
        if (ev->button != OS_MOUSE_LEFT) {
            break;
        }
        if (t.mode == MODE_WINDOW) {
            int i = window_at(ev->screen_x, ev->screen_y);
            if (i >= 0) {
                struct gfx_surface *snip = cut(t.shot->windows[i].frame);
                if (snip) {
                    /* The rounded corners: white, not what was behind the window. */
                    struct gfx_surface *round = gfx_surface_create(snip->width, snip->height);
                    if (round) {
                        gfx_clear(round, WHITE);
                        struct gfx_surface *mask = gfx_surface_create(snip->width, snip->height);
                        if (mask) {
                            gfx_clear(mask, GFX_RGBA(0, 0, 0, 0));
                            gfx_fill_rounded_rect(mask, gfx_rect(0, 0, snip->width, snip->height), 15,
                                                  GFX_RGB(255, 255, 255));
                            for (int y = 0; y < snip->height; y++) {
                                uint32_t *src = gfx_row(snip, y), *m = gfx_row(mask, y), *dst = gfx_row(round, y);
                                for (int x = 0; x < snip->width; x++) {
                                    uint32_t a = m[x] >> 24, p = src[x];
                                    uint32_t out = 0xFF000000;
                                    for (int sh = 0; sh < 24; sh += 8) {
                                        uint32_t c = ((p >> sh) & 0xFF) * a / 255 + 255 * (255 - a) / 255;
                                        out |= c << sh;
                                    }
                                    dst[x] = out;
                                }
                            }
                            gfx_surface_destroy(mask);
                            gfx_surface_destroy(snip);
                            snip = round;
                        } else {
                            gfx_surface_destroy(round);
                        }
                    }
                }
                finish_snip(snip);
            }
            break;
        }
        t.dragging = true;
        t.ax = t.bx = ev->screen_x;
        t.ay = t.by = ev->screen_y;
        t.outline_count = 0;
        if (t.mode == MODE_FREEFORM) {
            t.outline[t.outline_count][0] = ev->screen_x;
            t.outline[t.outline_count][1] = ev->screen_y;
            t.outline_count++;
        }
        draw_overlay();
        break;
    case GUI_EVENT_MOUSE_MOVE:
        if (t.mode == MODE_WINDOW) {
            int i = window_at(ev->screen_x, ev->screen_y);
            if (i != t.hover_window) {
                t.hover_window = i;
                draw_overlay();
            }
        } else if (t.dragging) {
            t.bx = ev->screen_x;
            t.by = ev->screen_y;
            if (t.mode == MODE_FREEFORM && t.outline_count < MAX_POINTS) {
                const int *last = t.outline[t.outline_count - 1];
                if (abs(last[0] - ev->screen_x) + abs(last[1] - ev->screen_y) >= 2) {
                    t.outline[t.outline_count][0] = ev->screen_x;
                    t.outline[t.outline_count][1] = ev->screen_y;
                    t.outline_count++;
                }
            }
            draw_overlay();
        }
        break;
    case GUI_EVENT_MOUSE_UP:
        if (ev->button == OS_MOUSE_LEFT && t.dragging) {
            t.dragging = false;
            struct gfx_surface *snip = t.mode == MODE_FREEFORM ? cut_freeform() : cut(drag_rect());
            if (snip) {
                finish_snip(snip);
            } else {
                t.outline_count = 0;   /* too small: try again */
                draw_overlay();
            }
        }
        break;
    default:
        break;
    }
}

/* --- the tool's window ---------------------------------------------------------------------------- */

static int button_at(int x, int y) {
    for (int b = 0; b < button_count(); b++) {
        if (gfx_rect_contains(button_rect(b), x, y)) {
            return b;
        }
    }
    return B_NONE;
}

static void show_menu(int b) {
    struct gfx_rect r = button_rect(b);
    struct gui_menu_item items[8];
    int count = 0;
    if (b == B_MODE) {
        for (int i = 0; i < MODE_COUNT; i++) {
            items[count++] = (struct gui_menu_item){ mode_names[i], MENU_MODE + i, false, (int)t.mode == i };
        }
    } else {
        static const char *const delays[6] = { "No delay", "1 second", "2 seconds", "3 seconds", "4 seconds",
                                               "5 seconds" };
        for (int i = 0; i <= 5; i++) {
            items[count++] = (struct gui_menu_item){ delays[i], MENU_DELAY + i, false, t.delay == i };
        }
    }
    /* Under the button. */
    gui_show_menu(t.g, t.menu_x + r.x, t.menu_y + r.y + r.h + 2, items, count, t.w);
}

static void press_button(int b, const struct gui_event *ev) {
    t.menu_x = ev->screen_x - ev->x;   /* (the window's content corner on the screen) */
    t.menu_y = ev->screen_y - ev->y;
    switch (b) {
    case B_NEW: new_snip(); break;
    case B_MODE:
    case B_DELAY: show_menu(b); break;
    case B_SAVE: save(); break;
    case B_PEN: t.tool = t.tool == TOOL_PEN ? TOOL_NONE : TOOL_PEN; break;
    case B_HIGHLIGHTER: t.tool = t.tool == TOOL_HIGHLIGHTER ? TOOL_NONE : TOOL_HIGHLIGHTER; break;
    case B_UNDO: undo(); break;
    }
}

#define SC_N 0x31
#define SC_S 0x1F
#define SC_Z 0x2C

static void window_event(const struct gui_event *ev) {
    switch (ev->type) {
    case GUI_EVENT_CLOSE:
        if (t.snip && !t.saved &&
            !gui_confirm(t.g, t.w, GUI_ICON_QUESTION, "Close without saving the snip?",
                         "Save keeps it in your Pictures folder, in Screenshots.", "Close", true)) {
            return;
        }
        gui_close(t.g);
        exit(0);
    case GUI_EVENT_MOUSE_MOVE: {
        int b = button_at(ev->x, ev->y);
        if (t.drawing) {
            stroke_move(ev->x, ev->y);
        } else if (b == t.hover) {
            return;
        }
        t.hover = b;
        gui_window_set_cursor(t.w, b >= 0 ? GUI_CURSOR_HAND : GUI_CURSOR_ARROW);
        break;
    }
    case GUI_EVENT_MOUSE_DOWN:
        if (ev->button != OS_MOUSE_LEFT) {
            return;
        }
        t.pressed = button_at(ev->x, ev->y);
        if (t.pressed == B_NONE && t.snip && t.tool != TOOL_NONE) {
            stroke_begin(ev->x, ev->y);
        }
        break;
    case GUI_EVENT_MOUSE_UP: {
        int b = t.pressed;
        t.pressed = B_NONE;
        t.drawing = false;
        if (ev->button == OS_MOUSE_LEFT && b != B_NONE && b == button_at(ev->x, ev->y) && button_enabled(b)) {
            press_button(b, ev);
            if (!t.w) {
                return;
            }
        }
        break;
    }
    case GUI_EVENT_MENU:
        if (ev->menu_id >= MENU_MODE && ev->menu_id < MENU_MODE + MODE_COUNT) {
            t.mode = (enum mode)(ev->menu_id - MENU_MODE);
            new_snip();   /* (choosing a mode starts a snip, as on Windows) */
        } else if (ev->menu_id >= MENU_DELAY && ev->menu_id <= MENU_DELAY + 5) {
            t.delay = ev->menu_id - MENU_DELAY;
        }
        break;
    case GUI_EVENT_KEY:
        if ((ev->modifiers & OS_MOD_CTRL) && ev->key == SC_N) {
            new_snip();
        } else if ((ev->modifiers & OS_MOD_CTRL) && ev->key == SC_S && t.snip) {
            save();
        } else if ((ev->modifiers & OS_MOD_CTRL) && ev->key == SC_Z) {
            undo();
        } else {
            return;
        }
        break;
    case GUI_EVENT_RESIZE:
        break;
    default:
        return;
    }
    if (t.w && gui_window_visible(t.w)) {
        draw();
    }
}

int main(int argc, char **argv) {
    t.g = gui_open();
    if (!t.g) {
        return 1;
    }
    t.ui = gui_font(t.g, GUI_FONT_REGULAR);
    t.bold = gui_font(t.g, GUI_FONT_BOLD);
    if (!t.ui || !t.bold) {
        return 1;
    }
    struct gfx_surface *logo = image_load(_PATH_DDE_RES "/Icons/Apps/SnipTool.png", NULL);
    if (logo) {
        t.logo = gfx_surface_scaled(logo, 20, 20);
        gfx_surface_destroy(logo);
    }
    t.mode = MODE_RECT;
    bool now = argc > 1 && strcmp(argv[1], "--new") == 0;
    make_window();
    if (!t.w) {
        return 1;
    }
    if (now) {
        t.exit_on_cancel = true;
        new_snip();
    }

    struct gui_event ev;
    for (;;) {
        if (!gui_wait_event(t.g, &ev, -1)) {
            continue;
        }
        if (ev.type == GUI_EVENT_QUIT) {
            return 0;
        }
        if (t.overlay && ev.window == t.overlay) {
            overlay_event(&ev);
        } else if (ev.window == t.w || ev.type == GUI_EVENT_MENU) {
            window_event(&ev);
        }
    }
}
