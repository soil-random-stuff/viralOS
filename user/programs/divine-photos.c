/*
 * Photos: look at pictures (PNG, JPEG, WebP, GIF, BMP, TIFF).
 *
 *   photos [file]
 *
 * Zoom with the wheel, the toolbar or + / - (0: fit, 1: actual size;
 * double-click switches between the two) and drag to move around.
 * Left/right (buttons or arrow keys) step through the other pictures in
 * the same folder. F11 or the toolbar button: full screen (Escape leaves).
 * The toolbar also shows the file's properties and deletes it.
 */

#include <fileops.h>
#include <gui.h>
#include <image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PATH_MAX     256
#define TOOLBAR_H    52
#define BUTTON       34
#define ARROW_R      22          /* the round previous/next buttons over the picture */
#define KEY_F11      0x57
#define MIN_ZOOM     0.01f
#define MAX_ZOOM     32.0f
#define ZOOM_STEP    1.25f
#define DOUBLE_CLICK_MS 400

#define WHITE       GFX_RGB(255, 255, 255)
#define INK         GFX_RGB(0x1C, 0x1C, 0x22)
#define INK_SOFT    GFX_RGB(0x78, 0x78, 0x82)
#define INK_FAINT   GFX_RGB(0xC0, 0xC0, 0xC8)
#define LINE_COLOR  GFX_RGB(0xEC, 0xEC, 0xF0)
#define FIELD       GFX_RGB(0xF4, 0xF5, 0xF8)
#define HOVER       GFX_RGB(0xEB, 0xEC, 0xF0)
#define PRESSED     GFX_RGB(0xE0, 0xE1, 0xE6)
#define ACCENT      GFX_RGB(0x2F, 0x6F, 0xEB)
#define DANGER      GFX_RGB(0xE0, 0x3E, 0x3E)
#define BACKDROP    GFX_RGB(0x1E, 0x1F, 0x24)
#define UI_SIZE     13.0f

enum control {
    C_NONE = -1,
    C_PREV, C_NEXT, C_ZOOM_OUT, C_ZOOM_IN, C_FIT, C_FULLSCREEN, C_INFO, C_DELETE,
    C_TOOLBAR_COUNT,
    C_ARROW_LEFT = C_TOOLBAR_COUNT, C_ARROW_RIGHT,
    C_COUNT,
};

enum dialog { DIALOG_NONE, DIALOG_PROPERTIES, DIALOG_DELETE, DIALOG_MESSAGE };

static struct {
    struct gui *g;
    struct gui_window *w;
    struct ttf_font *ui, *bold;

    /* The picture. */
    char path[PATH_MAX];
    struct gfx_surface *image;       /* NULL: nothing open, or it couldn't be read */
    bool opaque;                     /* no see-through pixels: no checkerboard needed */
    const char *error;
    char format[16];

    /* The other pictures in its folder (names, sorted), and where this one is. */
    char (*siblings)[OS_NAME_MAX];
    int sibling_count, index;

    /* The view: zoom, and the picture point at the centre of the view. */
    bool fit;                        /* follow the window size */
    float zoom, cx, cy;

    bool fullscreen;
    int hover, pressed;
    bool panning;
    int pan_x, pan_y;
    float pan_cx, pan_cy;
    uint64_t last_click;
    int last_click_x, last_click_y;

    enum dialog dialog;
    char dialog_title[96], message[160];
    char lines[8][2][96];
    int line_count;
    int dialog_hover, dialog_pressed;
} p;

/* --- the folder ------------------------------------------------------------------------------- */

static int compare_names(const void *a, const void *b) {
    return name_compare((const char *)a, (const char *)b);
}

static void load_siblings(void) {
    free(p.siblings);
    p.siblings = NULL;
    p.sibling_count = 0;
    p.index = -1;
    if (!p.path[0]) {
        return;
    }
    char dir[PATH_MAX];
    path_parent(p.path, dir, sizeof(dir));
    int fd = open(dir, O_RDONLY);
    if (fd < 0) {
        return;
    }
    int cap = 0;
    struct os_file_info info;
    for (uint64_t i = 0; readdir(fd, i, &info) >= 0; i++) {
        if (info.type != OS_FILE || !image_supported_name(info.name)) {
            continue;
        }
        if (p.sibling_count == cap) {
            cap = cap ? cap * 2 : 32;
            void *bigger = realloc(p.siblings, (size_t)cap * sizeof(*p.siblings));
            if (!bigger) {
                break;
            }
            p.siblings = bigger;
        }
        strlcpy(p.siblings[p.sibling_count++], info.name, OS_NAME_MAX);
    }
    close(fd);
    if (p.sibling_count) {
        qsort(p.siblings, (size_t)p.sibling_count, sizeof(*p.siblings), compare_names);
    }
    const char *name = path_base(p.path);
    for (int i = 0; i < p.sibling_count; i++) {
        if (strcmp(p.siblings[i], name) == 0) {
            p.index = i;
        }
    }
}

/* --- the view ---------------------------------------------------------------------------------- */

static struct gfx_rect view_rect(void) {
    struct gfx_surface *s = gui_window_surface(p.w);
    int top = p.fullscreen ? 0 : TOOLBAR_H;
    return gfx_rect(0, top, s->width, s->height - top);
}

static float fit_zoom(void) {
    struct gfx_rect v = view_rect();
    if (!p.image) {
        return 1;
    }
    int margin = p.fullscreen ? 0 : 16;
    float zx = (float)(v.w - 2 * margin) / p.image->width, zy = (float)(v.h - 2 * margin) / p.image->height;
    float z = zx < zy ? zx : zy;
    z = z > 1 ? 1 : z;   /* small pictures at their real size */
    return z < MIN_ZOOM ? MIN_ZOOM : z;
}

/* Keep the picture in view: centred along a side where it fits, no empty space where it doesn't. */
static void clamp_view(void) {
    if (!p.image) {
        return;
    }
    if (p.fit) {
        p.zoom = fit_zoom();
    }
    struct gfx_rect v = view_rect();
    float half_w = v.w / (2 * p.zoom), half_h = v.h / (2 * p.zoom);
    float w = (float)p.image->width, h = (float)p.image->height;
    p.cx = w <= 2 * half_w ? w / 2 : p.cx < half_w ? half_w : p.cx > w - half_w ? w - half_w : p.cx;
    p.cy = h <= 2 * half_h ? h / 2 : p.cy < half_h ? half_h : p.cy > h - half_h ? h - half_h : p.cy;
}

static bool can_pan(void) {
    if (!p.image) {
        return false;
    }
    struct gfx_rect v = view_rect();
    return p.image->width * p.zoom > v.w + 0.5f || p.image->height * p.zoom > v.h + 0.5f;
}

/* Zoom to z keeping the picture point under (x, y) (window coordinates) where it is. */
static void zoom_at(float z, int x, int y) {
    if (!p.image) {
        return;
    }
    z = z < MIN_ZOOM ? MIN_ZOOM : z > MAX_ZOOM ? MAX_ZOOM : z;
    struct gfx_rect v = view_rect();
    float dx = x - (v.x + v.w / 2.0f), dy = y - (v.y + v.h / 2.0f);
    float ix = p.cx + dx / p.zoom, iy = p.cy + dy / p.zoom;
    p.fit = false;
    p.zoom = z;
    p.cx = ix - dx / z;
    p.cy = iy - dy / z;
    clamp_view();
}

static void zoom_step(int steps, int x, int y) {
    float z = p.zoom;
    for (int i = 0; i < steps; i++) {
        z *= ZOOM_STEP;
    }
    for (int i = 0; i > steps; i--) {
        z /= ZOOM_STEP;
    }
    /* Snap to 100% on the way past it. */
    if ((p.zoom < 1 && z > 1) || (p.zoom > 1 && z < 1)) {
        z = 1;
    }
    zoom_at(z, x, y);
}

static void zoom_center(int steps) {
    struct gfx_rect v = view_rect();
    zoom_step(steps, v.x + v.w / 2, v.y + v.h / 2);
}

static void set_fit(void) {
    p.fit = true;
    clamp_view();
}

static void set_actual_size(int x, int y) {
    zoom_at(1, x, y);
}

/* --- opening -------------------------------------------------------------------------------- */

static void update_title(void) {
    char title[96];
    if (p.path[0]) {
        snprintf(title, sizeof(title), "%s - Photos", path_base(p.path));
    } else {
        strlcpy(title, "Photos", sizeof(title));
    }
    gui_window_set_title(p.w, title);
}

static void draw(void);

static void open_path(const char *path) {
    gfx_surface_destroy(p.image);
    p.image = NULL;
    p.error = NULL;
    p.format[0] = '\0';
    strlcpy(p.path, path, sizeof(p.path));
    update_title();

    /* Big pictures take a moment: say so first. */
    p.error = "Loading...";
    draw();
    gui_flush(p.g);
    p.error = NULL;

    size_t size = 0;
    uint8_t *data = image_read_file(path, &size, &p.error);
    if (data) {
        const char *format = image_format(data, size);
        strlcpy(p.format, format ? format : "", sizeof(p.format));
        p.image = image_decode(data, size, &p.error);
        free(data);
    }
    p.opaque = true;
    for (int y = 0; p.image && y < p.image->height && p.opaque; y++) {
        const uint32_t *row = gfx_row(p.image, y);
        for (int x = 0; x < p.image->width; x++) {
            if (row[x] >> 24 != 255) {
                p.opaque = false;
                break;
            }
        }
    }
    if (!p.image && !p.error) {
        p.error = "This picture can't be shown";
    }
    p.fit = true;
    p.zoom = 1;
    clamp_view();
}

static void open_sibling(int delta) {
    if (p.sibling_count < 2 || p.index < 0) {
        return;
    }
    int i = (p.index + delta + p.sibling_count) % p.sibling_count;
    char dir[PATH_MAX], path[PATH_MAX];
    path_parent(p.path, dir, sizeof(dir));
    path_join(path, sizeof(path), dir, p.siblings[i]);
    p.index = i;
    open_path(path);
}

/* --- dialogs ---------------------------------------------------------------------------------- */

static void format_size(uint64_t size, char *out, size_t n) {
    if (size < 1024) {
        snprintf(out, n, "%lu byte%s", (unsigned long)size, size == 1 ? "" : "s");
    } else if (size < 1024 * 1024) {
        snprintf(out, n, "%.1f KB (%lu bytes)", size / 1024.0, (unsigned long)size);
    } else {
        snprintf(out, n, "%.1f MB (%lu bytes)", size / (1024.0 * 1024.0), (unsigned long)size);
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
    snprintf(out, n, "%d %s %d, %02d:%02d", tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour,
             tm.tm_min);
}

static void add_line(const char *label, const char *value) {
    if (p.line_count < 8) {
        strlcpy(p.lines[p.line_count][0], label, sizeof(p.lines[0][0]));
        strlcpy(p.lines[p.line_count][1], value, sizeof(p.lines[0][1]));
        p.line_count++;
    }
}

static void open_dialog(enum dialog kind, const char *title) {
    p.dialog = kind;
    strlcpy(p.dialog_title, title, sizeof(p.dialog_title));
    p.message[0] = '\0';
    p.line_count = 0;
    p.dialog_hover = p.dialog_pressed = -1;
}

static void show_properties(void) {
    struct os_file_info info;
    if (!p.path[0] || stat(p.path, &info) < 0) {
        return;
    }
    open_dialog(DIALOG_PROPERTIES, info.name);
    char value[96];
    snprintf(value, sizeof(value), "%s image", p.format[0] ? p.format : "Unknown");
    add_line("Type", value);
    if (p.image) {
        snprintf(value, sizeof(value), "%d x %d pixels", p.image->width, p.image->height);
        add_line("Dimensions", value);
    }
    format_size(info.size, value, sizeof(value));
    add_line("Size", value);
    path_parent(p.path, value, sizeof(value));
    add_line("Location", value);
    format_date(info.ctime, value, sizeof(value));
    add_line("Created", value);
    format_date(info.mtime, value, sizeof(value));
    add_line("Modified", value);
}

static void ask_delete(void) {
    if (!p.path[0]) {
        return;
    }
    open_dialog(DIALOG_DELETE, "Delete photo?");
    snprintf(p.message, sizeof(p.message), "\"%s\" will be deleted.", path_base(p.path));
}

static void delete_current(void) {
    if (unlink(p.path) < 0) {
        open_dialog(DIALOG_MESSAGE, "Couldn't delete the photo");
        snprintf(p.message, sizeof(p.message), "\"%s\" could not be deleted.", path_base(p.path));
        return;
    }
    /* On to the next picture in the folder (the one that takes its place), if any. */
    int index = p.index;
    load_siblings();
    if (p.sibling_count == 0) {
        gfx_surface_destroy(p.image);
        p.image = NULL;
        p.error = NULL;
        p.path[0] = '\0';
        update_title();
        return;
    }
    index = index < 0 ? 0 : index >= p.sibling_count ? p.sibling_count - 1 : index;
    char dir[PATH_MAX], path[PATH_MAX];
    path_parent(p.path, dir, sizeof(dir));
    path_join(path, sizeof(path), dir, p.siblings[index]);
    p.index = index;
    open_path(path);
}

static int dialog_buttons(const char **labels) {
    if (p.dialog == DIALOG_DELETE) {
        labels[0] = "Delete";
        labels[1] = "Cancel";
        return 2;
    }
    labels[0] = "OK";
    return 1;
}

static struct gfx_rect dialog_card(void) {
    struct gfx_surface *s = gui_window_surface(p.w);
    int w = 440, h = p.dialog == DIALOG_PROPERTIES ? 110 + 28 * p.line_count : 150;
    w = w > s->width - 20 ? s->width - 20 : w;
    h = h > s->height - 20 ? s->height - 20 : h;
    return gfx_rect((s->width - w) / 2, (s->height - h) / 2, w, h);
}

static struct gfx_rect dialog_button_rect(int i) {
    struct gfx_rect r = dialog_card();
    int bw = 96, bh = 32;
    return gfx_rect(r.x + r.w - 20 - bw - i * (bw + 10), r.y + r.h - 20 - bh, bw, bh);
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

static void dialog_choose(int i) {
    enum dialog kind = p.dialog;
    p.dialog = DIALOG_NONE;
    if (kind == DIALOG_DELETE && i == 0) {
        delete_current();
    }
}

static void draw_dialog(struct gfx_surface *s) {
    gfx_fill_rect(s, gfx_rect(0, 0, s->width, s->height), GFX_RGBA(0, 0, 0, 90));
    struct gfx_rect r = dialog_card();
    gfx_fill_rounded_rect(s, gfx_rect(r.x - 1, r.y, r.w + 2, r.h + 3), 15, GFX_RGBA(0, 0, 0, 40));
    gfx_fill_rounded_rect(s, r, 14, WHITE);
    gfx_set_clip(s, gfx_rect_inset(r, 12));
    ttf_draw_text(s, p.bold, 16, r.x + 22, r.y + 36, INK, p.dialog_title);
    if (p.dialog == DIALOG_PROPERTIES) {
        for (int i = 0; i < p.line_count; i++) {
            float y = r.y + 70 + 28 * i;
            ttf_draw_text(s, p.ui, UI_SIZE, r.x + 22, y, INK_SOFT, p.lines[i][0]);
            ttf_draw_text(s, p.ui, UI_SIZE, r.x + 120, y, INK, p.lines[i][1]);
        }
    } else {
        ttf_draw_text(s, p.ui, UI_SIZE, r.x + 22, r.y + 66, INK_SOFT, p.message);
    }
    gfx_reset_clip(s);

    const char *labels[2];
    int n = dialog_buttons(labels);
    for (int i = 0; i < n; i++) {
        struct gfx_rect b = dialog_button_rect(i);
        bool main = i == 0, danger = main && p.dialog == DIALOG_DELETE;
        gfx_color fill = main ? (danger ? DANGER : ACCENT) : FIELD;
        if (p.dialog_pressed == i) {
            fill = main ? (danger ? GFX_RGB(0xC4, 0x30, 0x30) : GFX_RGB(0x20, 0x55, 0xC0)) : PRESSED;
        } else if (p.dialog_hover == i) {
            fill = main ? (danger ? GFX_RGB(0xEE, 0x50, 0x50) : GFX_RGB(0x45, 0x80, 0xF5)) : HOVER;
        }
        gfx_fill_rounded_rect(s, b, 8, fill);
        float w = ttf_text_width(p.ui, UI_SIZE, labels[i]);
        ttf_draw_text(s, p.ui, UI_SIZE, b.x + (b.w - w) / 2, b.y + 21, main ? WHITE : INK, labels[i]);
    }
}

/* --- controls --------------------------------------------------------------------------------- */

static bool control_enabled(int c) {
    switch (c) {
    case C_PREV:
    case C_NEXT:
    case C_ARROW_LEFT:
    case C_ARROW_RIGHT:
        return p.sibling_count > 1 && p.index >= 0;
    case C_ZOOM_OUT:
        return p.image && p.zoom > MIN_ZOOM;
    case C_ZOOM_IN:
        return p.image && p.zoom < MAX_ZOOM;
    case C_FIT:
        return p.image && !p.fit;
    case C_INFO:
    case C_DELETE:
        return p.path[0] != '\0';
    default:
        return true;
    }
}

/* Toolbar buttons: previous/next on the left; zoom, full screen, info, delete on the right. */
static struct gfx_rect control_rect(int c) {
    struct gfx_surface *s = gui_window_surface(p.w);
    int y = (TOOLBAR_H - BUTTON) / 2;
    switch (c) {
    case C_PREV:       return gfx_rect(12, y, BUTTON, BUTTON);
    case C_NEXT:       return gfx_rect(12 + BUTTON + 4, y, BUTTON, BUTTON);
    case C_DELETE:     return gfx_rect(s->width - 12 - BUTTON, y, BUTTON, BUTTON);
    case C_INFO:       return gfx_rect(s->width - 12 - 2 * BUTTON - 4, y, BUTTON, BUTTON);
    case C_FULLSCREEN: return gfx_rect(s->width - 12 - 3 * BUTTON - 8, y, BUTTON, BUTTON);
    case C_FIT:        return gfx_rect(s->width - 12 - 4 * BUTTON - 24, y, BUTTON, BUTTON);
    case C_ZOOM_IN:    return gfx_rect(s->width - 12 - 5 * BUTTON - 28, y, BUTTON, BUTTON);
    case C_ZOOM_OUT:   return gfx_rect(s->width - 12 - 6 * BUTTON - 28 - 56, y, BUTTON, BUTTON);
    case C_ARROW_LEFT:
    case C_ARROW_RIGHT: {
        struct gfx_rect v = view_rect();
        int cx = c == C_ARROW_LEFT ? v.x + 16 + ARROW_R : v.x + v.w - 16 - ARROW_R;
        return gfx_rect(cx - ARROW_R, v.y + v.h / 2 - ARROW_R, 2 * ARROW_R, 2 * ARROW_R);
    }
    default:
        return gfx_rect(0, 0, 0, 0);
    }
}

static int control_at(int x, int y) {
    for (int c = 0; c < C_COUNT; c++) {
        if (p.fullscreen && c < C_TOOLBAR_COUNT) {
            continue;   /* no toolbar in full screen */
        }
        if (c >= C_TOOLBAR_COUNT && !control_enabled(c)) {
            continue;   /* the round arrows only appear when there's somewhere to go */
        }
        if (gfx_rect_contains(control_rect(c), x, y)) {
            return c;
        }
    }
    return C_NONE;
}

static void draw_icon(struct gfx_surface *s, int c, float cx, float cy, gfx_color ink) {
    const float w = 1.8f;
    switch (c) {
    case C_PREV:
    case C_ARROW_LEFT:
        gfx_draw_line(s, cx + 3, cy - 7, cx - 4, cy, 2.2f, ink);
        gfx_draw_line(s, cx - 4, cy, cx + 3, cy + 7, 2.2f, ink);
        break;
    case C_NEXT:
    case C_ARROW_RIGHT:
        gfx_draw_line(s, cx - 3, cy - 7, cx + 4, cy, 2.2f, ink);
        gfx_draw_line(s, cx + 4, cy, cx - 3, cy + 7, 2.2f, ink);
        break;
    case C_ZOOM_OUT:
    case C_ZOOM_IN:
        gfx_fill_circle(s, cx - 2, cy - 2, 7.5f, ink);
        gfx_fill_circle(s, cx - 2, cy - 2, 5.7f, WHITE);
        gfx_draw_line(s, cx + 3.5f, cy + 3.5f, cx + 8, cy + 8, 2.4f, ink);
        gfx_draw_line(s, cx - 5, cy - 2, cx + 1, cy - 2, w, ink);
        if (c == C_ZOOM_IN) {
            gfx_draw_line(s, cx - 2, cy - 5, cx - 2, cy + 1, w, ink);
        }
        break;
    case C_FIT:
    case C_FULLSCREEN: {
        /* Four corners: pointing in (fit) or out (full screen). */
        float d = 7, l = 4;
        for (int i = 0; i < 4; i++) {
            float sx = i & 1 ? 1 : -1, sy = i & 2 ? 1 : -1;
            float x = cx + sx * d, y = cy + sy * d;
            if (c == C_FIT) {
                gfx_draw_line(s, x - sx * l, y, x, y, w, ink);
                gfx_draw_line(s, x, y - sy * l, x, y, w, ink);
                gfx_draw_line(s, x, y, x - sx * 3.5f, y - sy * 3.5f, w, ink);
            } else {
                gfx_draw_line(s, x - sx * l, y, x, y, w, ink);
                gfx_draw_line(s, x, y - sy * l, x, y, w, ink);
            }
        }
        if (c == C_FIT) {
            gfx_fill_rect(s, gfx_rect((int)cx - 2, (int)cy - 2, 4, 4), ink);
        }
        break;
    }
    case C_INFO:
        gfx_fill_circle(s, cx, cy, 8.5f, ink);
        gfx_fill_circle(s, cx, cy, 6.7f, WHITE);
        gfx_fill_circle(s, cx, cy - 3.5f, 1.2f, ink);
        gfx_draw_line(s, cx, cy - 0.5f, cx, cy + 4, w, ink);
        break;
    case C_DELETE:
        gfx_draw_line(s, cx - 7, cy - 5, cx + 7, cy - 5, w, ink);
        gfx_draw_line(s, cx - 2.5f, cy - 7.5f, cx + 2.5f, cy - 7.5f, w, ink);
        gfx_draw_line(s, cx - 5, cy - 4, cx - 4, cy + 7, w, ink);
        gfx_draw_line(s, cx + 5, cy - 4, cx + 4, cy + 7, w, ink);
        gfx_draw_line(s, cx - 4, cy + 7, cx + 4, cy + 7, w, ink);
        gfx_draw_line(s, cx - 1.5f, cy - 1.5f, cx - 1.5f, cy + 4, 1.3f, ink);
        gfx_draw_line(s, cx + 1.5f, cy - 1.5f, cx + 1.5f, cy + 4, 1.3f, ink);
        break;
    default:
        break;
    }
}

static void draw_toolbar(struct gfx_surface *s) {
    gfx_fill_rect(s, gfx_rect(0, 0, s->width, TOOLBAR_H), WHITE);
    gfx_fill_rect(s, gfx_rect(0, TOOLBAR_H - 1, s->width, 1), LINE_COLOR);
    for (int c = 0; c < C_TOOLBAR_COUNT; c++) {
        struct gfx_rect r = control_rect(c);
        bool enabled = control_enabled(c);
        if (enabled && (p.pressed == c || p.hover == c)) {
            gfx_fill_rounded_rect(s, r, 8, p.pressed == c ? PRESSED : HOVER);
        }
        draw_icon(s, c, r.x + r.w / 2.0f, r.y + r.h / 2.0f, enabled ? INK : INK_FAINT);
    }
    /* Zoom level between the zoom buttons. */
    char zoom[16];
    snprintf(zoom, sizeof(zoom), "%d%%", p.image ? (int)(p.zoom * 100 + 0.5f) : 100);
    struct gfx_rect out = control_rect(C_ZOOM_OUT), in = control_rect(C_ZOOM_IN);
    float zw = ttf_text_width(p.ui, UI_SIZE, zoom);
    float zx = out.x + out.w + (in.x - out.x - out.w - zw) / 2;
    ttf_draw_text(s, p.ui, UI_SIZE, zx, TOOLBAR_H / 2 + 5, p.image ? INK : INK_FAINT, zoom);
    gfx_fill_rect(s, gfx_rect(control_rect(C_FULLSCREEN).x - 10, 14, 1, TOOLBAR_H - 28), LINE_COLOR);

    /* The name, and where it is among its folder's pictures. */
    int left = control_rect(C_NEXT).x + BUTTON + 14, right = out.x - 14;
    if (right > left && p.path[0]) {
        gfx_set_clip(s, gfx_rect(left, 0, right - left, TOOLBAR_H));
        float x = ttf_draw_text(s, p.bold, UI_SIZE, left, TOOLBAR_H / 2 + 5, INK, path_base(p.path));
        if (p.sibling_count > 1 && p.index >= 0) {
            char where[32];
            snprintf(where, sizeof(where), "%d of %d", p.index + 1, p.sibling_count);
            ttf_draw_text(s, p.ui, UI_SIZE, x + 10, TOOLBAR_H / 2 + 5, INK_SOFT, where);
        }
        gfx_reset_clip(s);
    }
}

static void draw_centered_text(struct gfx_surface *s, struct gfx_rect v, float dy, gfx_color ink, const char *t) {
    float w = ttf_text_width(p.ui, 14, t);
    ttf_draw_text(s, p.ui, 14, v.x + (v.w - w) / 2, v.y + v.h / 2.0f + dy, ink, t);
}

static void draw_view(struct gfx_surface *s) {
    struct gfx_rect v = view_rect();
    gfx_fill_rect(s, v, p.fullscreen ? GFX_RGB(0, 0, 0) : BACKDROP);
    gfx_set_clip(s, v);
    if (p.image) {
        float w = p.image->width * p.zoom, h = p.image->height * p.zoom;
        float x = v.x + v.w / 2.0f - p.cx * p.zoom, y = v.y + v.h / 2.0f - p.cy * p.zoom;
        struct gfx_rect to = gfx_rect((int)(x + 0.5f), (int)(y + 0.5f), (int)(w + 0.5f), (int)(h + 0.5f));
        to.w = to.w < 1 ? 1 : to.w;
        to.h = to.h < 1 ? 1 : to.h;
        /* Pictures with transparency show over a light checkerboard. */
        struct gfx_rect shown = gfx_rect_intersect(to, v);
        for (int cy = shown.y; !p.opaque && cy < shown.y + shown.h; cy += 10) {
            for (int cx = shown.x; cx < shown.x + shown.w; cx += 10) {
                bool dark = (((cx - to.x) / 10) + ((cy - to.y) / 10)) & 1;
                gfx_fill_rect(s, gfx_rect_intersect(gfx_rect(cx, cy, 10, 10), shown),
                              dark ? GFX_RGB(0xDD, 0xDD, 0xE2) : GFX_RGB(0xF4, 0xF4, 0xF7));
            }
        }
        gfx_draw_scaled(s, to, p.image, NULL);
    } else if (p.error) {
        draw_centered_text(s, v, 0, GFX_RGB(0xD0, 0xD0, 0xD8), p.error);
    } else {
        draw_centered_text(s, v, -10, GFX_RGB(0xD0, 0xD0, 0xD8), "No photo open");
        draw_centered_text(s, v, 14, GFX_RGB(0x90, 0x90, 0x9A), "Open one from Files, or drop it here.");
    }
    /* Round previous/next buttons over the picture. */
    for (int c = C_ARROW_LEFT; c <= C_ARROW_RIGHT; c++) {
        if (!control_enabled(c)) {
            continue;
        }
        struct gfx_rect r = control_rect(c);
        int alpha = p.pressed == c ? 230 : p.hover == c ? 200 : 120;
        gfx_fill_circle(s, r.x + ARROW_R, r.y + ARROW_R, ARROW_R, GFX_RGBA(0x20, 0x20, 0x26, alpha));
        draw_icon(s, c, r.x + ARROW_R, r.y + ARROW_R, WHITE);
    }
    gfx_reset_clip(s);
}

static void draw(void) {
    struct gfx_surface *s = gui_window_surface(p.w);
    if (!p.fullscreen) {
        draw_toolbar(s);
    }
    draw_view(s);
    if (p.dialog != DIALOG_NONE) {
        draw_dialog(s);
    }
    gui_window_invalidate(p.w, NULL);
}

/* --- acting ------------------------------------------------------------------------------------ */

static void set_fullscreen(bool on) {
    if (on == p.fullscreen) {
        return;
    }
    p.fullscreen = on;
    gui_window_set_fullscreen(p.w, on);
    /* The RESIZE event that follows redraws at the new size. */
}

static void press(int c, int x, int y) {
    if (!control_enabled(c)) {
        return;
    }
    switch (c) {
    case C_PREV:
    case C_ARROW_LEFT:  open_sibling(-1); break;
    case C_NEXT:
    case C_ARROW_RIGHT: open_sibling(1); break;
    case C_ZOOM_OUT:    zoom_center(-1); break;
    case C_ZOOM_IN:     zoom_center(1); break;
    case C_FIT:         set_fit(); break;
    case C_FULLSCREEN:  set_fullscreen(!p.fullscreen); break;
    case C_INFO:        show_properties(); break;
    case C_DELETE:      ask_delete(); break;
    default:            break;
    }
    (void)x;
    (void)y;
}

static void update_cursor(int x, int y) {
    bool over_view = gfx_rect_contains(view_rect(), x, y) && control_at(x, y) == C_NONE;
    gui_window_set_cursor(p.w, over_view && can_pan() ? GUI_CURSOR_HAND : GUI_CURSOR_ARROW);
}

static void mouse_down(const struct gui_event *ev) {
    if (p.dialog != DIALOG_NONE) {
        p.dialog_pressed = ev->button == OS_MOUSE_LEFT ? dialog_button_at(ev->x, ev->y) : -1;
        return;
    }
    if (ev->button != OS_MOUSE_LEFT) {
        return;
    }
    int c = control_at(ev->x, ev->y);
    if (c != C_NONE) {
        p.pressed = c;
        return;
    }
    if (!gfx_rect_contains(view_rect(), ev->x, ev->y) || !p.image) {
        return;
    }
    /* Double-click: fit <-> actual size. */
    uint64_t now = uptime_ms();
    if (now - p.last_click < DOUBLE_CLICK_MS && abs(ev->x - p.last_click_x) < 6 &&
        abs(ev->y - p.last_click_y) < 6) {
        p.last_click = 0;
        if (p.fit && p.zoom < 1) {
            set_actual_size(ev->x, ev->y);
        } else {
            set_fit();
        }
        return;
    }
    p.last_click = now;
    p.last_click_x = ev->x;
    p.last_click_y = ev->y;
    p.panning = true;
    p.pan_x = ev->x;
    p.pan_y = ev->y;
    p.pan_cx = p.cx;
    p.pan_cy = p.cy;
}

static void mouse_up(const struct gui_event *ev) {
    if (p.dialog != DIALOG_NONE) {
        int b = dialog_button_at(ev->x, ev->y);
        bool chosen = b >= 0 && b == p.dialog_pressed;
        p.dialog_pressed = -1;
        if (chosen) {
            dialog_choose(b);
        }
        return;
    }
    p.panning = false;
    int c = p.pressed;
    p.pressed = C_NONE;
    if (c != C_NONE && control_at(ev->x, ev->y) == c) {
        press(c, ev->x, ev->y);
    }
}

static bool mouse_move(const struct gui_event *ev) {
    if (p.dialog != DIALOG_NONE) {
        int b = dialog_button_at(ev->x, ev->y);
        bool changed = b != p.dialog_hover;
        p.dialog_hover = b;
        return changed;
    }
    if (p.panning && (ev->buttons & OS_MOUSE_LEFT)) {
        p.cx = p.pan_cx - (ev->x - p.pan_x) / p.zoom;
        p.cy = p.pan_cy - (ev->y - p.pan_y) / p.zoom;
        if (abs(ev->x - p.pan_x) > 3 || abs(ev->y - p.pan_y) > 3) {
            p.last_click = 0;   /* a drag, not the first half of a double-click */
        }
        clamp_view();
        return true;
    }
    int c = control_at(ev->x, ev->y);
    update_cursor(ev->x, ev->y);
    if (c != p.hover) {
        p.hover = c;
        return true;
    }
    return false;
}

static bool key(const struct gui_event *ev) {
    if (p.dialog != DIALOG_NONE) {
        if (ev->key == OS_KEY_ESCAPE) {
            p.dialog = DIALOG_NONE;
        } else if (ev->key == OS_KEY_ENTER) {
            dialog_choose(0);
        } else {
            return false;
        }
        return true;
    }
    switch (ev->key) {
    case OS_KEY_LEFT:   open_sibling(-1); return true;
    case OS_KEY_RIGHT:  open_sibling(1); return true;
    case OS_KEY_HOME:
        if (p.sibling_count > 1 && p.index > 0) {
            open_sibling(-p.index);
        }
        return true;
    case OS_KEY_END:
        if (p.sibling_count > 1 && p.index >= 0) {
            open_sibling(p.sibling_count - 1 - p.index);
        }
        return true;
    case OS_KEY_DELETE: ask_delete(); return true;
    case KEY_F11:       set_fullscreen(!p.fullscreen); return true;
    case OS_KEY_ESCAPE:
        if (p.fullscreen) {
            set_fullscreen(false);
        }
        return true;
    default:
        break;
    }
    switch (ev->ch) {
    case '+':
    case '=':
        zoom_center(1);
        return true;
    case '-':
        zoom_center(-1);
        return true;
    case '0':
        set_fit();
        return true;
    case '1': {
        struct gfx_rect v = view_rect();
        set_actual_size(v.x + v.w / 2, v.y + v.h / 2);
        return true;
    }
    case 'i':
        show_properties();
        return true;
    case 'f':
        set_fullscreen(!p.fullscreen);
        return true;
    default:
        return false;
    }
}

int main(int argc, char **argv) {
    p.g = gui_open();
    if (!p.g) {
        return 1;
    }
    p.ui = gui_font(p.g, GUI_FONT_REGULAR);
    p.bold = gui_font(p.g, GUI_FONT_BOLD);
    if (!p.ui || !p.bold) {
        return 1;
    }
    p.hover = p.pressed = C_NONE;
    p.zoom = 1;
    p.fit = true;

    int w = gui_screen_width(p.g) * 2 / 3, h = gui_screen_height(p.g) * 2 / 3;
    p.w = gui_window_create(p.g, "Photos", -1, -1, w < 640 ? 640 : w, h < 460 ? 460 : h);
    if (!p.w) {
        return 1;
    }
    if (argc > 1) {
        open_path(argv[1]);
        load_siblings();
    }
    draw();

    struct gui_event ev;
    while (gui_wait_event(p.g, &ev, -1)) {
        bool redraw = false;
        switch (ev.type) {
        case GUI_EVENT_QUIT:
        case GUI_EVENT_CLOSE:
            gui_close(p.g);
            return 0;
        case GUI_EVENT_RESIZE:
            clamp_view();
            redraw = true;
            break;
        case GUI_EVENT_MOUSE_DOWN:
            mouse_down(&ev);
            redraw = true;
            break;
        case GUI_EVENT_MOUSE_UP:
            mouse_up(&ev);
            update_cursor(ev.x, ev.y);
            redraw = true;
            break;
        case GUI_EVENT_MOUSE_MOVE:
            redraw = mouse_move(&ev);
            break;
        case GUI_EVENT_SCROLL:
            if (p.dialog == DIALOG_NONE && p.image && gfx_rect_contains(view_rect(), ev.x, ev.y)) {
                zoom_step(ev.wheel < 0 ? 1 : -1, ev.x, ev.y);   /* wheel up: closer */
                update_cursor(ev.x, ev.y);
                redraw = true;
            }
            break;
        case GUI_EVENT_KEY:
            redraw = key(&ev);
            break;
        case GUI_EVENT_DROP: {
            char paths[1][OS_PATH_MAX];
            if (gui_drop_paths(paths, 1) == 1 && image_supported_name(paths[0])) {
                open_path(paths[0]);
                load_siblings();
                redraw = true;
            }
            break;
        }
        default:
            break;
        }
        if (redraw) {
            draw();
        }
    }
    return 0;
}
