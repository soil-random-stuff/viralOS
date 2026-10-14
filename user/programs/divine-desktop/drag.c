/*
 * Dragging files with the mouse.
 *
 * A drag starts from the desktop's own icons (pressing on a selected icon
 * and moving), or from a program's window (it sends GUI_MSG_DRAG with the
 * paths in GUI_DRAG_FILE while the button is held). Faded copies of the
 * icons follow the pointer. Where the button is released decides:
 *
 *   on a folder icon on the desktop    moved into that folder
 *   on the wallpaper                   desktop icons: moved to that spot on the grid;
 *                                      files from elsewhere: moved to the desktop, there
 *   on a window                        its program gets a GUI_EVENT_DROP and decides
 *                                      (Files moves them into the folder it shows)
 *   on the panel, or Escape            nothing
 *
 * Moving something to the folder it's already in does nothing, so dragging
 * desktop icons onto a Files window showing the Desktop folder (or files
 * from a Files window showing the Desktop folder onto the desktop) leaves
 * them where they are.
 */

#include <stdlib.h>
#include <string.h>

#include "desktop.h"

#define GHOST_ALPHA 150
#define MAX_DRAG    256

int icon_at(struct desktop *d, int x, int y);
void icons_move_selected(struct desktop *d, int grabbed, int x, int y);
void icons_place_at(struct desktop *d, char (*names)[OS_NAME_MAX], int count, int x, int y);

/* Draw `src` faded to `alpha`, inside the destination's clip. */
static void blit_alpha(struct gfx_surface *dst, int x, int y, const struct gfx_surface *src, uint32_t alpha) {
    struct gfx_rect r = gfx_rect_intersect(gfx_rect(x, y, src->width, src->height), dst->clip);
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        uint32_t *out = gfx_row(dst, yy) + r.x;
        const uint32_t *in = gfx_row(src, yy - y) + (r.x - x);
        for (int i = 0; i < r.w; i++) {
            out[i] = gfx_blend(out[i], gfx_scale(in[i], alpha));
        }
    }
}

/* A blue bubble with how many things are being dragged. */
static void draw_badge(struct desktop *d, struct gfx_surface *s, float cx, float cy, int count) {
    char text[12];
    snprintf(text, sizeof(text), "%d", count);
    float w = d->bold ? ttf_text_width(d->bold, 11, text) : 8;
    float r = w / 2 + 6 > 10 ? w / 2 + 6 : 10;
    gfx_fill_rounded_rect(s, gfx_rect((int)(cx - r), (int)(cy - 10), (int)(2 * r), 20), 10, ACCENT);
    if (d->bold) {
        ttf_draw_text(s, d->bold, 11, cx - w / 2, cy + 4, GFX_RGB(255, 255, 255), text);
    }
}

/* --- where things are drawn --------------------------------------------------------------------- */

static int offset_x(struct desktop *d) {
    return d->mouse_x - d->drag.grab_x;
}

static int offset_y(struct desktop *d) {
    return d->mouse_y - d->drag.grab_y;
}

static struct gfx_rect ghost_bounds(struct desktop *d) {
    struct gfx_rect b = { 0, 0, 0, 0 };
    if (d->drag.from_icons) {
        for (int i = 0; i < d->icon_count; i++) {
            if (d->icons[i].selected) {
                struct gfx_rect r = d->icons[i].rect;
                r.x += offset_x(d);
                r.y += offset_y(d);
                b = gfx_rect_empty(b) ? r : gfx_rect_union(b, r);
            }
        }
        /* and the badge by the pointer */
        b = gfx_rect_union(b, gfx_rect(d->mouse_x, d->mouse_y, 48, 34));
    } else if (d->drag.ghost) {
        b = gfx_rect(d->mouse_x + 4, d->mouse_y + 8, d->drag.ghost->width, d->drag.ghost->height);
    }
    return b;
}

void drag_compose(struct desktop *d, struct gfx_rect area) {
    struct drag *g = &d->drag;
    if (!g->active || gfx_rect_empty(gfx_rect_intersect(g->drawn, area))) {
        return;
    }
    if (g->from_icons) {
        for (int i = 0; i < d->icon_count; i++) {
            const struct desk_icon *ic = &d->icons[i];
            if (ic->selected && ic->image) {
                int x = ic->rect.x + offset_x(d) + (ic->rect.w - ic->image->width) / 2;
                int y = ic->rect.y + offset_y(d) + 8;
                blit_alpha(&d->screen, x, y, ic->image, GHOST_ALPHA);
            }
        }
        if (g->count > 1) {
            draw_badge(d, &d->screen, d->mouse_x + 26, d->mouse_y + 22, g->count);
        }
    } else if (g->ghost) {
        blit_alpha(&d->screen, d->mouse_x + 4, d->mouse_y + 8, g->ghost, 220);
    }
}

/* --- starting and ending -------------------------------------------------------------------------- */

static void finish(struct desktop *d) {
    struct drag *g = &d->drag;
    damage(d, g->drawn);
    if (g->target >= 0 && g->target < d->icon_count) {
        damage(d, d->icons[g->target].rect);
    }
    free(g->paths);
    gfx_surface_destroy(g->ghost);
    memset(g, 0, sizeof(*g));
    g->target = -1;
}

void drag_cancel(struct desktop *d) {
    if (d->drag.from_icons) {
        icons_move_selected(d, -1, 0, 0);   /* redraw them where they were */
    }
    finish(d);
}

static bool begin(struct desktop *d) {
    struct drag *g = &d->drag;
    finish(d);
    g->paths = malloc(sizeof(*g->paths) * MAX_DRAG);
    return g->paths != NULL;
}

void drag_from_icons(struct desktop *d, int grabbed, int press_x, int press_y);
void drag_from_icons(struct desktop *d, int grabbed, int press_x, int press_y) {
    struct drag *g = &d->drag;
    if (!begin(d)) {
        return;
    }
    for (int i = 0; i < d->icon_count && g->count < MAX_DRAG; i++) {
        if (d->icons[i].selected) {
            path_join(g->paths[g->count++], FILE_PATH_MAX, DESKTOP_DIR, d->icons[i].info.name);
        }
    }
    strlcpy(g->source_dir, DESKTOP_DIR, sizeof(g->source_dir));
    g->from_icons = true;
    g->grabbed = grabbed;
    g->grab_x = press_x;
    g->grab_y = press_y;
    g->active = true;
    g->drawn = ghost_bounds(d);
    damage(d, g->drawn);
}

void drag_from_client(struct desktop *d, struct client *c, struct window *w, int count) {
    struct drag *g = &d->drag;
    (void)count;
    /* Only while the button that started it is still held in that window. */
    if (!(d->buttons & OS_MOUSE_LEFT) || !w || d->captured != w || w->client != c || !begin(d)) {
        return;
    }
    int fd = open(GUI_DRAG_FILE, O_RDONLY);
    static char buf[MAX_DRAG * FILE_PATH_MAX];
    long len = 0, n;
    while (fd >= 0 && len < (long)sizeof(buf) - 1 && (n = read(fd, buf + len, sizeof(buf) - 1 - len)) > 0) {
        len += n;
    }
    if (fd >= 0) {
        close(fd);
    }
    buf[len > 0 ? len : 0] = '\0';
    for (char *line = buf, *next; line && *line && g->count < MAX_DRAG; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        if (line[0] == '/') {
            strlcpy(g->paths[g->count++], line, FILE_PATH_MAX);
        }
    }
    if (!g->count) {
        finish(d);
        return;
    }
    path_parent(g->paths[0], g->source_dir, sizeof(g->source_dir));

    /* The picture: the first item's icon, and how many there are. */
    struct os_file_info info;
    enum file_icon kind = stat(g->paths[0], &info) == 0 ? file_icon_for(&info) : FILE_ICON_FILE;
    struct gfx_surface *icon = d->file_icons[kind];
    int size = icon ? icon->width : 48;
    g->ghost = gfx_surface_create(size + 24, size + 14);
    if (g->ghost) {
        if (icon) {
            gfx_blit(g->ghost, 0, 4, icon, NULL);
        }
        if (g->count > 1) {
            draw_badge(d, g->ghost, size + 10, 12, g->count);
        }
    }
    d->captured = NULL;   /* the window doesn't get the rest of this press */
    g->active = true;
    g->drawn = ghost_bounds(d);
    damage(d, g->drawn);
}

/* --- while dragging ---------------------------------------------------------------------------------- */

static bool is_dragged(struct desktop *d, int icon) {
    if (d->drag.from_icons) {
        return d->icons[icon].selected;
    }
    char path[FILE_PATH_MAX];
    path_join(path, sizeof(path), DESKTOP_DIR, d->icons[icon].info.name);
    for (int i = 0; i < d->drag.count; i++) {
        if (strcmp(d->drag.paths[i], path) == 0) {
            return true;
        }
    }
    return false;
}

/* The desktop folder icon under the pointer that things could drop into, or -1. */
static int folder_target(struct desktop *d) {
    if (window_at(d, d->mouse_x, d->mouse_y) || panel_contains(d, d->mouse_x, d->mouse_y) ||
        launcher_contains(d, d->mouse_x, d->mouse_y)) {
        return -1;
    }
    int i = icon_at(d, d->mouse_x, d->mouse_y);
    if (i < 0 || d->icons[i].info.type != OS_DIRECTORY || is_dragged(d, i)) {
        return -1;
    }
    return i;
}

void drag_move(struct desktop *d) {
    struct drag *g = &d->drag;
    damage(d, g->drawn);
    int target = folder_target(d);
    if (target != g->target) {
        if (g->target >= 0 && g->target < d->icon_count) {
            damage(d, d->icons[g->target].rect);
        }
        if (target >= 0) {
            damage(d, d->icons[target].rect);
        }
        g->target = target;
    }
    g->drawn = ghost_bounds(d);
    damage(d, g->drawn);
}

static void report(struct desktop *d, int result, const char *failed) {
    if (result < 0) {
        char message[160];
        snprintf(message, sizeof(message), "%s: %s", failed, strerror(result));
        desk_dialog_open(d, DESK_DIALOG_ERROR, "Can't move it", message, NULL, NULL);
    }
}

void drag_drop(struct desktop *d) {
    struct drag *g = &d->drag;
    int x = d->mouse_x, y = d->mouse_y;
    char failed[OS_NAME_MAX] = "";

    /* On the panel or the apps menu: nothing. */
    if (panel_contains(d, x, y) || launcher_contains(d, x, y)) {
        drag_cancel(d);
        return;
    }
    /* On a window: its program decides. */
    struct window *w = window_at(d, x, y);
    if (w) {
        if (g->from_icons) {
            int fd = open(GUI_DRAG_FILE, O_WRONLY | O_CREAT | O_TRUNC);
            for (int i = 0; fd >= 0 && i < g->count; i++) {
                write(fd, g->paths[i], strlen(g->paths[i]));
                write(fd, "\n", 1);
            }
            if (fd >= 0) {
                close(fd);
            }
        }
        struct gui_msg_event ev = {
            .header = { GUI_MSG_EVENT, w->id },
            .type = GUI_EVENT_DROP,
            .x = x - w->frame.x,
            .y = y - w->frame.y - title_height(w),
            .screen_x = x,
            .screen_y = y,
            .count = g->count,
        };
        send_event(w, &ev);
        window_raise(d, w);
        focus(d, w);
        drag_cancel(d);
        return;
    }
    /* On a folder icon: into that folder. */
    int target = folder_target(d);
    if (target >= 0) {
        char dir[FILE_PATH_MAX];
        path_join(dir, sizeof(dir), DESKTOP_DIR, d->icons[target].info.name);
        int n = move_into(g->paths, g->count, dir, NULL, failed, sizeof(failed));
        finish(d);
        icons_reload(d);
        report(d, n, failed);
        return;
    }
    /* On the wallpaper. */
    if (g->from_icons) {
        /* Desktop icons: to where the grabbed one's middle now is. */
        const struct desk_icon *ic = &d->icons[g->grabbed];
        int cx = ic->rect.x + offset_x(d) + ic->rect.w / 2, cy = ic->rect.y + offset_y(d) + ic->rect.h / 2;
        icons_move_selected(d, g->grabbed, cx, cy);
        finish(d);
        return;
    }
    if (strcmp(g->source_dir, DESKTOP_DIR) == 0) {
        finish(d);   /* they're on the desktop already */
        return;
    }
    static char names[MAX_DRAG][OS_NAME_MAX];
    int n = move_into(g->paths, g->count, DESKTOP_DIR, names, failed, sizeof(failed));
    if (n > 0) {
        icons_place_at(d, names, n, x, y);
    }
    finish(d);
    for (int i = 0; i < d->icon_count; i++) {
        d->icons[i].selected = false;
    }
    icons_reload(d);
    report(d, n, failed);
}
