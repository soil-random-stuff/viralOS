/*
 * Compositing: building the screen picture from wallpaper, window shadows,
 * windows, the panel, open menus and the cursor, one damaged rectangle at
 * a time, then copying those rectangles to the real framebuffer.
 */

#include <math.h>
#include <string.h>

#include "desktop.h"

/* --- damage ---------------------------------------------------------------------------- */

void damage(struct desktop *d, struct gfx_rect r) {
    r = gfx_rect_intersect(r, (struct gfx_rect){ 0, 0, d->screen.width, d->screen.height });
    if (gfx_rect_empty(r)) {
        return;
    }
    /* Merge with an overlapping rectangle, so nothing gets drawn twice. */
    for (int i = 0; i < d->damage_count; i++) {
        if (!gfx_rect_empty(gfx_rect_intersect(d->damage[i], r))) {
            d->damage[i] = gfx_rect_union(d->damage[i], r);
            return;
        }
    }
    if (d->damage_count == MAX_DAMAGE) {
        /* Too many pieces: one big rectangle around everything. */
        for (int i = 1; i < d->damage_count; i++) {
            d->damage[0] = gfx_rect_union(d->damage[0], d->damage[i]);
        }
        d->damage[0] = gfx_rect_union(d->damage[0], r);
        d->damage_count = 1;
        return;
    }
    d->damage[d->damage_count++] = r;
}

struct gfx_rect window_bounds(const struct window *w) {
    struct gfx_rect b = gfx_rect_inset(w->frame, -SHADOW_BLUR);
    b.h += SHADOW_OFFSET;
    return b;
}

/* --- rounded corners --------------------------------------------------------------------- */

void init_corners(struct desktop *d) {
    const float r = (float)CORNER_RADIUS;
    for (int y = 0; y < CORNER_RADIUS; y++) {
        for (int x = 0; x < CORNER_RADIUS; x++) {
            float dx = r - (x + 0.5f), dy = r - (y + 0.5f);
            float c = 0.5f - (sqrtf(dx * dx + dy * dy) - r);
            d->corner[y][x] = c <= 0 ? 0 : c >= 1 ? 255 : (uint8_t)(c * 255 + 0.5f);
        }
    }
}

/* Coverage of a pixel near one of the window's corners (255 elsewhere). */
static uint32_t corner_coverage(const struct desktop *d, const struct window *w, int fx, int fy) {
    int R = CORNER_RADIUS;
    int cy = fy < R ? fy : fy >= w->frame.h - R ? w->frame.h - 1 - fy : -1;
    int cx = fx < R ? fx : fx >= w->frame.w - R ? w->frame.w - 1 - fx : -1;
    if (w->maximized || w->fullscreen || cx < 0 || cy < 0) {
        return 255;
    }
    return d->corner[cy][cx];
}

/* --- drawing one rectangle of the screen ------------------------------------------------------- */

static void draw_shadow(struct desktop *d, const struct window *w, struct gfx_rect area) {
    if (!w->shadow || w->maximized || w->fullscreen) {
        return;
    }
    int sx = w->frame.x - SHADOW_BLUR, sy = w->frame.y - SHADOW_BLUR + SHADOW_OFFSET;
    struct gfx_rect s = { sx, sy, w->shadow->width, w->shadow->height };
    /* Under the window's body the shadow would be painted over anyway, so
       only draw the ring around it (keeping the corners, which are see-through). */
    struct gfx_rect inner = gfx_rect_inset(w->frame, CORNER_RADIUS);
    struct gfx_rect bands[4] = {
        { s.x, s.y, s.w, inner.y - s.y },                                     /* above */
        { s.x, inner.y + inner.h, s.w, s.y + s.h - (inner.y + inner.h) },     /* below */
        { s.x, inner.y, inner.x - s.x, inner.h },                             /* left */
        { inner.x + inner.w, inner.y, s.x + s.w - (inner.x + inner.w), inner.h }, /* right */
    };
    gfx_color color = w == d->focused ? GFX_RGBA(0, 0, 0, 120) : GFX_RGBA(0, 0, 0, 70);
    for (int i = 0; i < 4; i++) {
        struct gfx_rect clip = gfx_rect_intersect(bands[i], area);
        if (!gfx_rect_empty(clip)) {
            gfx_set_clip(&d->screen, clip);
            gfx_fill_mask(&d->screen, sx, sy, w->shadow, color);
        }
    }
    gfx_set_clip(&d->screen, area);
}

/* One row of the window, frame columns [x0, x0 + n): title bar or content. */
static void window_row(const struct window *w, int fy, int x0, int n, uint32_t *out) {
    if (fy < title_height(w)) {
        memcpy(out, gfx_row(w->title_bar, fy) + x0, (size_t)n * sizeof(uint32_t));
        return;
    }
    int cy = fy - title_height(w);
    int have = 0;   /* columns the program's buffer covers */
    if (w->buffer && cy < w->buffer_h && x0 < w->buffer_w) {
        have = w->buffer_w - x0 < n ? w->buffer_w - x0 : n;
        memcpy(out, w->buffer + (size_t)cy * w->buffer_w + x0, (size_t)have * sizeof(uint32_t));
    }
    for (int i = have; i < n; i++) {
        out[i] = CONTENT_BLANK;
    }
}

static void draw_window(struct desktop *d, const struct window *w, struct gfx_rect area) {
    struct gfx_rect v = gfx_rect_intersect(w->frame, area);
    if (gfx_rect_empty(v)) {
        return;
    }
    int R = CORNER_RADIUS;
    uint32_t row[v.w];
    for (int y = v.y; y < v.y + v.h; y++) {
        int fy = y - w->frame.y;
        uint32_t *dst = gfx_row(&d->screen, y) + v.x;
        bool corner_row = !w->maximized && !w->fullscreen && (fy < R || fy >= w->frame.h - R);
        if (!corner_row) {
            window_row(w, fy, v.x - w->frame.x, v.w, dst);   /* windows are opaque */
            continue;
        }
        window_row(w, fy, v.x - w->frame.x, v.w, row);
        for (int i = 0; i < v.w; i++) {
            uint32_t c = corner_coverage(d, w, v.x + i - w->frame.x, fy);
            /* Content pixels may carry any alpha; at the corners use them as opaque. */
            uint32_t src = row[i] | 0xFF000000u;
            dst[i] = c == 255 ? src : c ? gfx_blend(dst[i], gfx_scale(src, c)) : dst[i];
        }
    }
}

static void draw_menu(struct desktop *d, struct gfx_rect area) {
    struct menu *m = &d->menu;
    if (!m->open || gfx_rect_empty(gfx_rect_intersect(menu_bounds(m), area))) {
        return;
    }
    if (m->shadow) {
        gfx_fill_mask(&d->screen, m->rect.x - 14, m->rect.y - 14 + 5, m->shadow, GFX_RGBA(0, 0, 0, 95));
    }
    gfx_blit(&d->screen, m->rect.x, m->rect.y, m->surface, NULL);
}

static void draw_tooltip(struct desktop *d, struct gfx_rect area) {
    if (d->panel_hidden || d->tooltip_for == PANEL_HOVER_NONE || !d->tooltip ||
        gfx_rect_empty(gfx_rect_intersect(d->tooltip_rect, area))) {
        return;
    }
    gfx_blit(&d->screen, d->tooltip_rect.x, d->tooltip_rect.y, d->tooltip, NULL);
}

static void compose(struct desktop *d, struct gfx_rect area) {
    gfx_set_clip(&d->screen, area);
    if (d->locked) {
        lock_compose(d, area);
        gfx_blit(&d->screen, d->mouse_x - d->cursor_hot_x, d->mouse_y - d->cursor_hot_y, d->cursor, NULL);
        gfx_reset_clip(&d->screen);
        return;
    }
    if (d->wallpaper) {
        gfx_copy(&d->screen, area.x, area.y, d->wallpaper, &area);
    } else {
        gfx_fill_rect(&d->screen, area, GFX_RGB(0x1E, 0x22, 0x2C));   /* still loading */
    }
    icons_compose(d, area);
    for (struct window *w = d->windows; w; w = w->next) {
        if (w->visible) {
            draw_shadow(d, w, area);
            draw_window(d, w, area);
        }
    }
    panel_compose(d, area);
    launcher_compose(d, area);
    volume_compose(d, area);
    network_compose(d, area);
    drag_compose(d, area);
    draw_menu(d, area);
    desk_dialog_compose(d, area);
    if (!d->shooting) {
        draw_tooltip(d, area);
        gfx_blit(&d->screen, d->mouse_x - d->cursor_hot_x, d->mouse_y - d->cursor_hot_y, d->cursor, NULL);
    }
    gfx_reset_clip(&d->screen);
}

bool render_screenshot(struct desktop *d, uint32_t *out, const struct client *exclude,
                       struct gui_shot_windows *list) {
    if (d->locked || d->wallpaper_hidden) {
        return false;
    }
    /* Hide the asking program's windows for a moment (marked by the client: shown again below). */
    struct window *hidden[64];
    int hidden_count = 0;
    list->count = 0;
    struct window *order[GUI_SHOT_WINDOWS_MAX];
    int n = 0;
    for (struct window *w = d->windows; w; w = w->next) {
        if (!w->visible) {
            continue;
        }
        if (w->client == exclude) {
            if (hidden_count < 64) {
                hidden[hidden_count++] = w;
                w->visible = false;
            }
        } else if (n < GUI_SHOT_WINDOWS_MAX) {
            order[n++] = w;
        } else {
            memmove(order, order + 1, sizeof(order[0]) * (GUI_SHOT_WINDOWS_MAX - 1));   /* keep the top ones */
            order[GUI_SHOT_WINDOWS_MAX - 1] = w;
        }
    }
    struct gfx_rect all = gfx_rect(0, 0, d->screen.width, d->screen.height);
    d->shooting = true;
    compose(d, all);
    d->shooting = false;
    for (int y = 0; y < all.h; y++) {
        memcpy(out + (size_t)y * (size_t)all.w, gfx_row(&d->screen, y), (size_t)all.w * sizeof(uint32_t));
    }
    for (int i = 0; i < hidden_count; i++) {
        hidden[i]->visible = true;
    }
    damage(d, all);   /* the real screen again, with them and the pointer */
    for (int i = n - 1; i >= 0; i--) {
        struct gui_shot_window *sw = &list->windows[list->count++];
        sw->x = order[i]->frame.x;
        sw->y = order[i]->frame.y;
        sw->width = order[i]->frame.w;
        sw->height = order[i]->frame.h;
        strlcpy(sw->title, order[i]->title, sizeof(sw->title));
    }
    return true;
}

/* Copy a finished rectangle to the real screen, converting the pixel format if needed. */
static void present(struct desktop *d, struct gfx_rect area) {
    const struct os_display_info *di = &d->display;
    bool native = di->red_shift == 16 && di->green_shift == 8 && di->blue_shift == 0;
    for (int y = area.y; y < area.y + area.h; y++) {
        const uint32_t *src = gfx_row(&d->screen, y) + area.x;
        uint32_t *dst = (uint32_t *)(di->address + (uint64_t)y * di->pitch) + area.x;
        if (native) {
            memcpy(dst, src, (size_t)area.w * sizeof(uint32_t));
            continue;
        }
        for (int x = 0; x < area.w; x++) {
            uint32_t p = src[x];
            dst[x] = ((p >> 16) & 0xFF) << di->red_shift | ((p >> 8) & 0xFF) << di->green_shift |
                     (p & 0xFF) << di->blue_shift;
        }
    }
}

void render(struct desktop *d) {
    /* The panel hides behind a full-screen window and comes back when it goes. */
    bool hidden = fullscreen_on_top(d);
    if (hidden != d->panel_hidden) {
        d->panel_hidden = hidden;
        struct gfx_rect bounds = gfx_rect_inset(d->panel_rect, -PANEL_SHADOW);
        bounds.h += 4;
        damage(d, bounds);
    }
    for (int i = 0; i < d->damage_count; i++) {
        if (!d->wallpaper_hidden) {
            compose(d, d->damage[i]);
        }
        present(d, d->damage[i]);
    }
    d->damage_count = 0;
}
