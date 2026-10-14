/* Windows: creating them, their title bars, stacking, focus, and moving them around. */

#include <stdlib.h>
#include <string.h>

#include "desktop.h"

static const gfx_color button_colors[BUTTON_COUNT] = {
    GFX_RGB(0xFE, 0xBC, 0x2E),   /* minimize */
    GFX_RGB(0x28, 0xC8, 0x40),   /* maximize */
    GFX_RGB(0xFF, 0x5F, 0x57),   /* close */
};

/* On the right, like Windows: minimize, maximize, close. */
static int button_center_x(const struct window *w, int i) {
    return w->frame.w - BUTTON_RIGHT - (BUTTON_COUNT - 1 - i) * BUTTON_SPACING;
}

int content_width(const struct window *w) {
    return w->frame.w;
}

int content_height(const struct window *w) {
    return w->frame.h - title_height(w);
}

/* --- decorations ------------------------------------------------------------------------ */

static void draw_button_symbol(struct gfx_surface *s, int i, float cx, float cy) {
    gfx_color ink = GFX_RGBA(0, 0, 0, 140);
    float k = 2.8f;
    if (i == BUTTON_CLOSE) {
        gfx_draw_line(s, cx - k, cy - k, cx + k, cy + k, 1.3f, ink);
        gfx_draw_line(s, cx - k, cy + k, cx + k, cy - k, 1.3f, ink);
    } else if (i == BUTTON_MINIMIZE) {
        gfx_draw_line(s, cx - k - 0.5f, cy, cx + k + 0.5f, cy, 1.4f, ink);
    } else {
        gfx_draw_line(s, cx - k - 0.5f, cy, cx + k + 0.5f, cy, 1.4f, ink);
        gfx_draw_line(s, cx, cy - k - 0.5f, cx, cy + k + 0.5f, 1.4f, ink);
    }
}

void draw_decorations(struct desktop *d, struct window *w) {
    struct gfx_surface *s = w->title_bar;
    bool focused = w == d->focused;

    gfx_clear(s, GFX_RGB(255, 255, 255));
    gfx_fill_rect(s, gfx_rect(0, TITLE_HEIGHT - 1, w->frame.w, 1), GFX_RGB(0xE6, 0xE6, 0xEA));

    /* The three buttons; symbols appear while the mouse is over any of them. */
    float cy = TITLE_HEIGHT / 2.0f;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (w->dialog && i != BUTTON_CLOSE) {
            continue;
        }
        gfx_color c = focused || w->hover_button != BUTTON_NONE ? button_colors[i]
                                                                : GFX_RGB(0xD5, 0xD5, 0xDA);
        if (w->pressed_button == i) {
            c = GFX_RGB(((c >> 16) & 0xFF) * 3 / 4, ((c >> 8) & 0xFF) * 3 / 4, (c & 0xFF) * 3 / 4);
        }
        float cx = (float)button_center_x(w, i);
        gfx_fill_circle(s, cx, cy, BUTTON_RADIUS + 0.7f, GFX_RGBA(0, 0, 0, 34));   /* thin rim */
        gfx_fill_circle(s, cx, cy, BUTTON_RADIUS, c);
        if (w->hover_button != BUTTON_NONE) {
            draw_button_symbol(s, i, cx, cy);
        }
    }

    /* Title, centred, but kept clear of the buttons. */
    if (d->bold) {
        float width = ttf_text_width(d->bold, TITLE_FONT_SIZE, w->title);
        float right = (float)(button_center_x(w, 0) - 18);
        float left = 16;
        float x = (w->frame.w - width) / 2;
        if (x + width > right) {
            x = right - width;
        }
        if (x < left) {
            x = left;
        }
        gfx_set_clip(s, gfx_rect((int)left, 0, (int)(right - left), TITLE_HEIGHT));
        ttf_draw_text(s, d->bold, TITLE_FONT_SIZE, x, cy + 5,
                      focused ? GFX_RGB(0x2A, 0x2A, 0x30) : GFX_RGB(0x9A, 0x9A, 0xA2), w->title);
        gfx_reset_clip(s);
    }
    if (w->visible) {
        damage(d, gfx_rect(w->frame.x, w->frame.y, w->frame.w, TITLE_HEIGHT));
    }
}

/* --- creating ------------------------------------------------------------------------------ */

/* (Re)build the title bar and shadow for the current frame size. */
static bool alloc_frame(struct window *w) {
    struct gfx_surface *bar = gfx_surface_create(w->frame.w, TITLE_HEIGHT);
    struct gfx_alpha *shadow = gfx_shadow_create(w->frame.w, w->frame.h, CORNER_RADIUS, SHADOW_BLUR);
    if (!bar || !shadow) {
        gfx_surface_destroy(bar);
        gfx_alpha_destroy(shadow);
        return false;
    }
    gfx_surface_destroy(w->title_bar);
    gfx_alpha_destroy(w->shadow);
    w->title_bar = bar;
    w->shadow = shadow;
    return true;
}

struct gfx_rect work_area(const struct desktop *d) {
    return gfx_rect(0, 0, d->screen.width, d->screen.height - PANEL_HEIGHT - 2 * PANEL_MARGIN);
}

struct window *window_create(struct desktop *d, struct client *c, uint32_t id, const char *title,
                             int x, int y, int width, int height, struct window *owner, bool dialog) {
    struct gfx_rect area = work_area(d);
    if (width < 1 || height < 1 || width > d->screen.width * 2 || height > d->screen.height * 2) {
        return NULL;
    }
    struct window *w = calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    w->client = c;
    w->id = id;
    strlcpy(w->title, title, sizeof(w->title));
    w->hover_button = w->pressed_button = BUTTON_NONE;
    w->visible = true;
    w->dialog = dialog;
    w->owner = owner;

    int count = 0;
    for (struct window *o = d->windows; o; o = o->next) {
        count++;
    }
    int fw = width, fh = height + TITLE_HEIGHT;
    if ((x < 0 || y < 0) && owner) {
        /* Over the middle of its window, a little towards the top. */
        x = owner->frame.x + (owner->frame.w - fw) / 2;
        y = owner->frame.y + (owner->frame.h - fh) / 3;
        x = x < 10 ? 10 : x + fw > area.w - 10 ? area.w - 10 - fw : x;
        y = y < 10 ? 10 : y + fh > area.h - 10 ? area.h - 10 - fh : y;
        if (!owner->visible) {
            window_show(d, owner);
        }
    } else if (x < 0 || y < 0) {
        /* Centred, each new window a step down and to the right of the last. */
        x = (area.w - fw) / 2 + (count % 6) * 32 - 80;
        y = (area.h - fh) / 2 + (count % 6) * 28 - 60;
        x = x < 10 ? 10 : x;
        y = y < 10 ? 10 : y;
    }
    w->frame = gfx_rect(x, y, fw, fh);
    if (!alloc_frame(w)) {
        free(w);
        return NULL;
    }

    /* On top. */
    struct window **link = &d->windows;
    while (*link) {
        link = &(*link)->next;
    }
    *link = w;
    draw_decorations(d, w);
    focus(d, w);
    damage(d, window_bounds(w));
    panel_update(d);
    return w;
}

void window_destroy(struct desktop *d, struct window *w) {
    for (struct window **link = &d->windows; *link; link = &(*link)->next) {
        if (*link == w) {
            *link = w->next;
            break;
        }
    }
    for (struct window *o = d->windows; o; o = o->next) {
        if (o->owner == w) {
            o->owner = NULL;
        }
    }
    struct window *owner = w->owner;
    if (w->visible) {
        damage(d, window_bounds(w));
    }
    if (d->dragging == w) {
        d->dragging = NULL;
    }
    if (d->resizing == w) {
        d->resizing = NULL;
    }
    if (d->captured == w) {
        d->captured = NULL;
    }
    if (d->menu.open && ((d->menu.kind == MENU_WINDOW && d->menu.window == w) ||
                         (d->menu.kind == MENU_CLIENT && d->menu.client == w->client &&
                          d->menu.window_id == w->id))) {
        menu_close(d);
    }
    if (d->focused == w) {
        d->focused = NULL;
        if (owner && owner->visible) {
            focus(d, window_modal(d, owner) ? window_modal(d, owner) : owner);   /* back to where it came from */
        } else {
            focus_top(d);
        }
    }
    if (w->buffer) {
        shm_unmap(w->buffer);
    }
    gfx_surface_destroy(w->title_bar);
    gfx_alpha_destroy(w->shadow);
    free(w);
    panel_update(d);
}

void window_set_buffer(struct desktop *d, struct window *w, uint32_t *buffer, int width, int height) {
    if (w->buffer) {
        shm_unmap(w->buffer);
    }
    w->buffer = buffer;
    w->buffer_w = width;
    w->buffer_h = height;
    if (w->visible) {
        damage(d, gfx_rect(w->frame.x, w->frame.y + title_height(w), content_width(w), content_height(w)));
    }
}

void window_set_title(struct desktop *d, struct window *w, const char *title) {
    strlcpy(w->title, title, sizeof(w->title));
    draw_decorations(d, w);
    panel_update(d);   /* tooltips show titles */
}

/* --- stacking and focus --------------------------------------------------------------------- */

void window_raise(struct desktop *d, struct window *w) {
    struct window **link = &d->windows;
    while (*link && *link != w) {
        link = &(*link)->next;
    }
    if (!*link || !w->next) {
        return;   /* not found, or already on top */
    }
    *link = w->next;
    struct window **end = link;
    while (*end) {
        end = &(*end)->next;
    }
    *end = w;
    w->next = NULL;
    damage(d, window_bounds(w));
    /* Its dialogs stay in front of it. */
    for (bool again = true; again;) {
        again = false;
        for (struct window *o = d->windows; o; o = o->next) {
            if (o->owner == w && o->next) {
                window_raise(d, o);
                again = true;
                break;
            }
        }
    }
}

struct window *window_modal(const struct desktop *d, struct window *w) {
    struct window *found = NULL;
    for (struct window *o = d->windows; o; o = o->next) {
        if (o->owner == w && o->dialog && o->visible) {
            found = o;
        }
    }
    if (!found) {
        return NULL;
    }
    struct window *inner = window_modal(d, found);
    return inner ? inner : found;
}

void focus(struct desktop *d, struct window *w) {
    struct window *modal = w ? window_modal(d, w) : NULL;
    if (modal) {
        w = modal;   /* the keyboard goes to what it's waiting for */
    }
    struct window *old = d->focused;
    if (old == w) {
        return;
    }
    d->focused = w;
    /* Title bar colours and shadow strength change with focus. */
    if (old) {
        draw_decorations(d, old);
        if (old->visible) {
            damage(d, window_bounds(old));
        }
        send_state(d, old);
    }
    if (w) {
        draw_decorations(d, w);
        damage(d, window_bounds(w));
        send_state(d, w);
    }
    panel_update(d);   /* the focused app is marked */
}

/* Focus the topmost visible window (or nothing). */
void focus_top(struct desktop *d) {
    struct window *top = NULL;
    for (struct window *o = d->windows; o; o = o->next) {
        if (o->visible) {
            top = o;
        }
    }
    focus(d, top);
}

void window_show(struct desktop *d, struct window *w) {
    if (!w->visible) {
        w->visible = true;
        damage(d, window_bounds(w));
        send_state(d, w);
    }
    for (struct window *o = d->windows; o; o = o->next) {
        if (o->owner == w && o->dialog && !o->visible) {
            o->visible = true;   /* its dialogs come back with it */
            damage(d, window_bounds(o));
            send_state(d, o);
        }
    }
    window_raise(d, w);
    focus(d, w);
}

void window_minimize(struct desktop *d, struct window *w) {
    if (!w->visible) {
        return;
    }
    if (w->dialog && w->owner) {
        window_minimize(d, w->owner);   /* a dialog goes away with its window */
        return;
    }
    for (struct window *o = d->windows; o; o = o->next) {
        if (o->owner == w && o->dialog && o->visible) {
            o->visible = false;
            damage(d, window_bounds(o));
            send_state(d, o);
        }
    }
    w->visible = false;
    w->hover_button = BUTTON_NONE;
    w->pressed_button = BUTTON_NONE;
    draw_decorations(d, w);
    damage(d, window_bounds(w));
    if (d->captured == w) {
        d->captured = NULL;
    }
    if (d->dragging == w) {
        d->dragging = NULL;
    }
    send_state(d, w);
    if (d->focused == w) {
        focus_top(d);
    }
    panel_update(d);
}

void window_close_request(struct window *w) {
    struct gui_msg_event ev = { .header = { GUI_MSG_EVENT, w->id }, .type = GUI_EVENT_CLOSE };
    send_event(w, &ev);
}

/* --- hit testing and moving ------------------------------------------------------------------ */

struct window *window_at(struct desktop *d, int x, int y) {
    struct window *hit = NULL;
    for (struct window *w = d->windows; w; w = w->next) {
        if (w->visible && gfx_rect_contains(w->frame, x, y)) {
            hit = w;   /* later ones are on top */
        }
    }
    return hit;
}

enum title_button title_button_at(const struct window *w, int x, int y) {
    int lx = x - w->frame.x, ly = y - w->frame.y;
    if (ly < 0 || ly >= title_height(w)) {
        return BUTTON_NONE;
    }
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (w->dialog && i != BUTTON_CLOSE) {
            continue;
        }
        int dx = lx - button_center_x(w, i), dy = ly - TITLE_HEIGHT / 2;
        if (dx * dx + dy * dy <= 9 * 9) {   /* a little bigger than drawn: easier to hit */
            return (enum title_button)i;
        }
    }
    return BUTTON_NONE;
}

void window_move(struct desktop *d, struct window *w, int x, int y) {
    struct gfx_rect area = work_area(d);
    /* Keep at least the title bar reachable (above the panel). */
    if (y < 0) {
        y = 0;
    }
    if (y > area.h - TITLE_HEIGHT) {
        y = area.h - TITLE_HEIGHT;
    }
    if (x > area.w - 60) {
        x = area.w - 60;
    }
    if (x < 60 - w->frame.w) {
        x = 60 - w->frame.w;
    }
    if (x == w->frame.x && y == w->frame.y) {
        return;
    }
    damage(d, window_bounds(w));
    w->frame.x = x;
    w->frame.y = y;
    damage(d, window_bounds(w));
}

void window_toggle_maximize(struct desktop *d, struct window *w) {
    damage(d, window_bounds(w));
    struct gfx_rect old = w->frame;
    w->frame = w->maximized ? w->saved_frame : work_area(d);
    if (!w->maximized) {
        w->saved_frame = old;
    }
    if (!alloc_frame(w)) {
        w->frame = old;   /* out of memory: stay as we were */
        return;
    }
    w->maximized = !w->maximized;
    draw_decorations(d, w);
    damage(d, window_bounds(w));
    send_state(d, w);
    struct gui_msg_event ev = {
        .header = { GUI_MSG_EVENT, w->id },
        .type = GUI_EVENT_RESIZE,
        .width = content_width(w),
        .height = content_height(w),
    };
    w->sent_w = ev.width;
    w->sent_h = ev.height;
    send_event(w, &ev);
}

bool fullscreen_on_top(const struct desktop *d) {
    const struct window *top = NULL;
    for (const struct window *w = d->windows; w; w = w->next) {
        if (w->visible) {
            top = w;
        }
    }
    return top && top->fullscreen;
}

void window_set_fullscreen(struct desktop *d, struct window *w, bool on) {
    if (on == w->fullscreen) {
        return;
    }
    damage(d, window_bounds(w));
    struct gfx_rect old = w->frame;
    if (on) {
        w->full_saved = w->frame;
        w->frame = gfx_rect(0, 0, d->screen.width, d->screen.height);
    } else {
        w->frame = w->full_saved;
    }
    w->fullscreen = on;
    if (!alloc_frame(w)) {
        w->frame = old;   /* out of memory: stay as we were */
        w->fullscreen = !on;
        return;
    }
    draw_decorations(d, w);
    damage(d, window_bounds(w));
    struct gui_msg_event ev = {
        .header = { GUI_MSG_EVENT, w->id },
        .type = GUI_EVENT_RESIZE,
        .width = content_width(w),
        .height = content_height(w),
    };
    w->sent_w = ev.width;
    w->sent_h = ev.height;
    send_event(w, &ev);
}

/* --- resizing by the edges -------------------------------------------------------------------- */

#define EDGE_OUTSIDE   6     /* the grab zone reaches this far outside the frame... */
#define EDGE_INSIDE    4     /* ...and this far into it */
#define CORNER_GRAB    18    /* this close to a corner grabs both edges */
#define MIN_WIDTH      240
#define MIN_HEIGHT     (TITLE_HEIGHT + 100)
#define RESIZE_TELL_MS 80    /* how often the program hears about the new size while dragging */

int resize_edges_at(struct desktop *d, int x, int y, struct window **out) {
    *out = NULL;
    if (panel_contains(d, x, y) || launcher_contains(d, x, y)) {
        return 0;
    }
    /* From the top window down: the first one whose grab zone holds the point decides. */
    struct window *stack[64];
    int n = 0;
    for (struct window *w = d->windows; w && n < 64; w = w->next) {
        stack[n++] = w;
    }
    while (n-- > 0) {
        struct window *w = stack[n];
        if (!w->visible || !gfx_rect_contains(gfx_rect_inset(w->frame, -EDGE_OUTSIDE), x, y)) {
            continue;
        }
        if (w->maximized || w->fullscreen || w->dialog || window_modal(d, w)) {
            return 0;
        }
        struct gfx_rect f = w->frame;
        int edges = 0;
        edges |= x < f.x + EDGE_INSIDE ? EDGE_LEFT : 0;
        edges |= x >= f.x + f.w - EDGE_INSIDE ? EDGE_RIGHT : 0;
        edges |= y < f.y + EDGE_INSIDE ? EDGE_TOP : 0;
        edges |= y >= f.y + f.h - EDGE_INSIDE ? EDGE_BOTTOM : 0;
        if (!edges) {
            return 0;   /* inside the window: its own business */
        }
        /* Near a corner: both of its edges. */
        if (edges & (EDGE_LEFT | EDGE_RIGHT)) {
            edges |= y < f.y + CORNER_GRAB ? EDGE_TOP : y >= f.y + f.h - CORNER_GRAB ? EDGE_BOTTOM : 0;
        }
        if (edges & (EDGE_TOP | EDGE_BOTTOM)) {
            edges |= x < f.x + CORNER_GRAB ? EDGE_LEFT : x >= f.x + f.w - CORNER_GRAB ? EDGE_RIGHT : 0;
        }
        /* The title bar buttons sit in the top-right corner: leave them clickable. */
        if (edges == (EDGE_TOP | EDGE_RIGHT) && y >= f.y && x < f.x + f.w) {
            edges = y < f.y + EDGE_INSIDE ? EDGE_TOP : 0;
            if (!edges) {
                return 0;
            }
        }
        *out = w;
        return edges;
    }
    return 0;
}

int resize_cursor(int edges) {
    bool h = edges & (EDGE_LEFT | EDGE_RIGHT), v = edges & (EDGE_TOP | EDGE_BOTTOM);
    if (h && v) {
        bool nwse = (edges & EDGE_LEFT) == ((edges & EDGE_TOP) ? EDGE_LEFT : 0);
        return nwse ? GUI_CURSOR_RESIZE_NWSE : GUI_CURSOR_RESIZE_NESW;
    }
    return h ? GUI_CURSOR_RESIZE_EW : v ? GUI_CURSOR_RESIZE_NS : GUI_CURSOR_ARROW;
}

/* Tell the program its content's size (it answers with a new buffer and redraws). */
static void tell_size(struct window *w) {
    if (content_width(w) == w->sent_w && content_height(w) == w->sent_h) {
        return;
    }
    struct gui_msg_event ev = {
        .header = { GUI_MSG_EVENT, w->id },
        .type = GUI_EVENT_RESIZE,
        .width = content_width(w),
        .height = content_height(w),
    };
    w->sent_w = ev.width;
    w->sent_h = ev.height;
    send_event(w, &ev);
}

void window_resize_begin(struct desktop *d, struct window *w, int edges) {
    d->resizing = w;
    d->resize_edges = edges;
    d->resize_from = w->frame;
    d->resize_x = d->mouse_x;
    d->resize_y = d->mouse_y;
    d->resize_sent_ms = 0;
    if (!w->sent_w) {
        w->sent_w = content_width(w);
        w->sent_h = content_height(w);
    }
}

void window_resize_update(struct desktop *d) {
    struct window *w = d->resizing;
    struct gfx_rect f = d->resize_from, area = work_area(d);
    int dx = d->mouse_x - d->resize_x, dy = d->mouse_y - d->resize_y;
    int left = f.x, top = f.y, right = f.x + f.w, bottom = f.y + f.h;
    if (d->resize_edges & EDGE_LEFT) {
        left = left + dx > right - MIN_WIDTH ? right - MIN_WIDTH : left + dx;
    }
    if (d->resize_edges & EDGE_RIGHT) {
        right = right + dx < left + MIN_WIDTH ? left + MIN_WIDTH : right + dx;
    }
    if (d->resize_edges & EDGE_TOP) {
        top = top + dy > bottom - MIN_HEIGHT ? bottom - MIN_HEIGHT : top + dy;
        top = top < area.y ? area.y : top;   /* the title bar stays on the screen */
    }
    if (d->resize_edges & EDGE_BOTTOM) {
        bottom = bottom + dy < top + MIN_HEIGHT ? top + MIN_HEIGHT : bottom + dy;
    }
    struct gfx_rect frame = gfx_rect(left, top, right - left, bottom - top);
    if (frame.x == w->frame.x && frame.y == w->frame.y && frame.w == w->frame.w && frame.h == w->frame.h) {
        return;
    }
    struct gfx_surface *bar = frame.w != w->frame.w ? gfx_surface_create(frame.w, TITLE_HEIGHT) : NULL;
    if (frame.w != w->frame.w && !bar) {
        return;   /* out of memory: stay as we are */
    }
    damage(d, window_bounds(w));
    w->frame = frame;
    if (bar) {
        gfx_surface_destroy(w->title_bar);
        w->title_bar = bar;
    }
    /* No shadow while the size keeps changing (making one is slow); it comes back on release. */
    gfx_alpha_destroy(w->shadow);
    w->shadow = NULL;
    draw_decorations(d, w);
    damage(d, window_bounds(w));
    uint64_t now = uptime_ms();
    if (now - d->resize_sent_ms >= RESIZE_TELL_MS) {
        d->resize_sent_ms = now;
        tell_size(w);
    }
}

void window_resize_end(struct desktop *d) {
    struct window *w = d->resizing;
    d->resizing = NULL;
    if (!w) {
        return;
    }
    w->shadow = gfx_shadow_create(w->frame.w, w->frame.h, CORNER_RADIUS, SHADOW_BLUR);
    damage(d, window_bounds(w));
    tell_size(w);
}
