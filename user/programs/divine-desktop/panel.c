/*
 * The panel: a floating white bar along the bottom of the screen. On the
 * left the apps menu button, then the apps: pinned ones always, others
 * while they run, with a mark under the running ones (a longer, blue one
 * for the app in front). On the right the clock, with the system tray
 * (empty for now) to its left.
 *
 * Clicking an app starts it, brings its window to the front, or, if it's
 * in front already, minimizes it. Middle click starts another copy, right
 * click opens a small menu. Hovering shows a tooltip.
 *
 * The apps menu pops up above the panel: every app as a tile, and at the
 * bottom the user and a button that leaves the desktop.
 */

#include <image.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sound.h>

#include <users.h>

#include "desktop.h"
#include <paths.h>

#define SLOT            46      /* hover square around each icon */
#define SLOT_STEP       54
#define SLOT_FIRST      8       /* from the panel's left edge */
#define SLOT_RADIUS     10
#define PANEL_ICON      36
#define CLOCK_FONT      19.0f
#define CLOCK_RIGHT     20      /* clock's right edge, from the panel's */
#define TOOLTIP_DELAY   550     /* ms of hovering before a tooltip shows */
#define TOOLTIP_FONT    12.5f

#define LAUNCHER_COLS   4
#define LAUNCHER_PAD    18
#define TILE_W          92
#define TILE_H          102
#define TILE_ICON       44
#define LAUNCHER_HEAD   60      /* title and description */
#define LAUNCHER_FOOT   58      /* user + exit button */
#define LAUNCHER_GAP    10      /* between the apps menu and the panel */
#define LAUNCHER_TOP    12      /* the least room left above the apps menu */
#define BAR_W           5       /* the scroll bar (when the apps don't all fit) */
#define BAR_HIT         16      /* how far from the right edge it can be grabbed */
#define WHEEL_STEP      (TILE_H / 2)

#define INK             GFX_RGB(0x1C, 0x1C, 0x22)
#define INK_SOFT        GFX_RGB(0x6A, 0x6A, 0x74)
#define HOVER_FILL      GFX_RGBA(0, 0, 0, 16)
#define PRESS_FILL      GFX_RGBA(0, 0, 0, 30)

enum { PANEL_MENU_NEW = 1, PANEL_MENU_CLOSE };

static const char *const weekdays[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                        "Friday", "Saturday" };
static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July",
                                      "August", "September", "October", "November", "December" };

/* --- what's on the panel --------------------------------------------------------------- */

static bool window_in_entry(const struct window *w, const struct panel_entry *e) {
    return e->app >= 0 ? w->client->app == e->app : w->client == e->client;
}

/* Is any window of this entry open (or the focused one)? */
static void entry_state(struct desktop *d, const struct panel_entry *e, bool *running, bool *front) {
    *running = *front = false;
    for (struct window *w = d->windows; w; w = w->next) {
        if (window_in_entry(w, e)) {
            *running = true;
            *front |= w == d->focused && w->visible;
        }
    }
}

static const char *entry_name(struct desktop *d, const struct panel_entry *e) {
    return e->app >= 0 ? d->apps[e->app].name : e->client->name;
}

static void rebuild_entries(struct desktop *d) {
    d->entry_count = 0;
    for (int i = 0; i < d->app_count && d->entry_count < PANEL_MAX; i++) {
        if (d->apps[i].pinned) {
            d->entries[d->entry_count++] = (struct panel_entry){ .app = i };
        }
    }
    /* Running programs that aren't pinned, in the order their windows were opened. */
    for (struct window *w = d->windows; w; w = w->next) {
        bool present = false;
        for (int i = 0; i < d->entry_count; i++) {
            present |= window_in_entry(w, &d->entries[i]);
        }
        if (!present && d->entry_count < PANEL_MAX) {
            d->entries[d->entry_count++] = (struct panel_entry){
                .app = w->client->app,
                .client = w->client->app >= 0 ? NULL : w->client,
            };
        }
    }
    int x = d->panel_rect.x + SLOT_FIRST, y = d->panel_rect.y + (PANEL_HEIGHT - SLOT) / 2;
    d->menu_slot = gfx_rect(x, y, SLOT, SLOT);
    for (int i = 0; i < d->entry_count; i++) {
        d->entries[i].slot = gfx_rect(x + (i + 1) * SLOT_STEP, y, SLOT, SLOT);
    }
}

/* --- drawing the panel --------------------------------------------------------------------- */

static struct gfx_rect local(const struct desktop *d, struct gfx_rect r) {
    return gfx_rect(r.x - d->panel_rect.x, r.y - d->panel_rect.y, r.w, r.h);
}

static void draw_slot(struct desktop *d, struct gfx_rect slot, int which, bool active,
                      const struct gfx_surface *icon) {
    struct gfx_surface *s = d->panel;
    struct gfx_rect r = local(d, slot);
    if (d->panel_pressed == which && d->panel_hover == which) {
        gfx_fill_rounded_rect(s, r, SLOT_RADIUS, PRESS_FILL);
    } else if (d->panel_hover == which || active) {
        gfx_fill_rounded_rect(s, r, SLOT_RADIUS, HOVER_FILL);
    }
    if (icon) {
        gfx_blit(s, r.x + (SLOT - icon->width) / 2, r.y + (SLOT - icon->height) / 2, icon, NULL);
    }
}

static bool clock24 = true;

static void format_clock(char *buf, size_t size, struct tm *tm) {
    if (clock24) {
        snprintf(buf, size, "%02d:%02d", tm->tm_hour, tm->tm_min);
    } else {
        int h = tm->tm_hour % 12;
        snprintf(buf, size, "%d:%02d %s", h ? h : 12, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
    }
}

static void render_panel(struct desktop *d) {
    struct gfx_surface *s = d->panel;
    struct gfx_rect all = gfx_rect(0, 0, s->width, s->height);
    gfx_clear(s, GFX_RGBA(0, 0, 0, 0));
    gfx_fill_rounded_rect(s, all, PANEL_RADIUS, GFX_RGB(255, 255, 255));
    gfx_stroke_rounded_rect(s, all, PANEL_RADIUS, 1, GFX_RGBA(0, 0, 0, 16));

    draw_slot(d, d->menu_slot, PANEL_HOVER_MENU, d->launcher_open, d->menu_icon);
    for (int i = 0; i < d->entry_count; i++) {
        struct panel_entry *e = &d->entries[i];
        draw_slot(d, e->slot, i, false, app_icon(d, e->app, e->client ? e->client->name : "", false));
        bool running, front;
        entry_state(d, e, &running, &front);
        if (running) {
            /* The mark: short and grey, or longer and blue for the app in front. */
            struct gfx_rect r = local(d, e->slot);
            int w = front ? 16 : 6;
            gfx_fill_rounded_rect(s, gfx_rect(r.x + (SLOT - w) / 2, r.y + SLOT + 1, w, 3), 1.5f,
                                  front ? ACCENT : GFX_RGB(0x9A, 0x9A, 0xA4));
        }
    }

    /* The clock. */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    d->clock_minute = (int)(now / 60);
    clock24 = d->cfg.clock24;
    char text[16];
    format_clock(text, sizeof(text), &tm);
    if (d->bold) {
        float w = ttf_text_width(d->bold, CLOCK_FONT, text);
        float x = s->width - CLOCK_RIGHT - w;
        struct ttf_metrics m;
        ttf_get_metrics(d->bold, CLOCK_FONT, &m);
        float baseline = (PANEL_HEIGHT + m.ascent - m.descent) / 2;
        d->clock_rect = gfx_rect(d->panel_rect.x + (int)x - 8, d->panel_rect.y + 6, (int)w + 16,
                                 PANEL_HEIGHT - 12);
        if (d->panel_hover == PANEL_HOVER_CLOCK) {
            gfx_fill_rounded_rect(s, local(d, d->clock_rect), SLOT_RADIUS, HOVER_FILL);
        }
        ttf_draw_text(s, d->bold, CLOCK_FONT, x, baseline, GFX_RGB(0, 0, 0), text);
    }
    /* The system tray, left of the clock. */
    volume_draw_tray(d, s, d->panel_hover == PANEL_HOVER_VOLUME, d->panel_pressed == PANEL_HOVER_VOLUME);
    network_draw_tray(d, s, d->panel_hover == PANEL_HOVER_NETWORK, d->panel_pressed == PANEL_HOVER_NETWORK);
    damage(d, d->panel_rect);
}

void panel_redraw(struct desktop *d) {
    render_panel(d);
}

bool panel_init(struct desktop *d) {
    int w = d->screen.width - 2 * PANEL_MARGIN;
    d->panel_rect = gfx_rect(PANEL_MARGIN, d->screen.height - PANEL_MARGIN - PANEL_HEIGHT, w, PANEL_HEIGHT);
    d->panel = gfx_surface_create(w, PANEL_HEIGHT);
    d->panel_shadow = gfx_shadow_create(w, PANEL_HEIGHT, PANEL_RADIUS, PANEL_SHADOW);
    d->panel_hover = d->panel_pressed = PANEL_HOVER_NONE;
    d->tooltip_for = PANEL_HOVER_NONE;
    d->launcher_hover = -1;

    struct gfx_surface *img = image_load_png(_PATH_DDE_RES "/Icons/Apps/AppsMenu.png", NULL);
    if (img) {
        d->menu_icon = gfx_surface_scaled(img, PANEL_ICON, PANEL_ICON);
        gfx_surface_destroy(img);
    }
    if (!d->panel || !d->panel_shadow) {
        return false;
    }
    volume_init(d);
    network_init(d);
    panel_update(d);
    return true;
}

void panel_update(struct desktop *d) {
    if (!d->panel) {
        return;
    }
    rebuild_entries(d);
    if (d->panel_hover >= d->entry_count) {
        d->panel_hover = PANEL_HOVER_NONE;
    }
    render_panel(d);
}

void panel_compose(struct desktop *d, struct gfx_rect area) {
    struct gfx_rect bounds = gfx_rect_inset(d->panel_rect, -PANEL_SHADOW);
    bounds.h += 4;
    if (d->panel_hidden || !d->panel || !d->panel_shadow || gfx_rect_empty(gfx_rect_intersect(bounds, area))) {
        return;
    }
    gfx_fill_mask(&d->screen, d->panel_rect.x - PANEL_SHADOW, d->panel_rect.y - PANEL_SHADOW + 4,
                  d->panel_shadow, GFX_RGBA(0, 0, 0, 70));
    gfx_blit(&d->screen, d->panel_rect.x, d->panel_rect.y, d->panel, NULL);
}

bool panel_contains(const struct desktop *d, int x, int y) {
    return !d->panel_hidden && gfx_rect_contains(d->panel_rect, x, y);
}

/* --- tooltips ------------------------------------------------------------------------------- */

static void hide_tooltip(struct desktop *d) {
    if (d->tooltip_for != PANEL_HOVER_NONE) {
        damage(d, d->tooltip_rect);
        d->tooltip_for = PANEL_HOVER_NONE;
    }
}

static void show_tooltip(struct desktop *d) {
    char text[96];
    struct gfx_rect anchor;
    int which = d->panel_hover;
    if (which == PANEL_HOVER_MENU) {
        strlcpy(text, "Apps", sizeof(text));
        anchor = d->menu_slot;
    } else if (which == PANEL_HOVER_CLOCK) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(text, sizeof(text), "%s, %d %s %d", weekdays[tm.tm_wday], tm.tm_mday,
                 months[tm.tm_mon], tm.tm_year + 1900);
        anchor = d->clock_rect;
    } else if (which == PANEL_HOVER_VOLUME) {
        volume_tooltip(d, text, sizeof(text));
        anchor = d->volume_slot;
    } else if (which == PANEL_HOVER_NETWORK) {
        network_tooltip(d, text, sizeof(text));
        anchor = d->net_slot;
    } else if (which >= 0 && which < d->entry_count) {
        strlcpy(text, entry_name(d, &d->entries[which]), sizeof(text));
        anchor = d->entries[which].slot;
    } else {
        return;
    }
    if (!d->regular) {
        return;
    }
    int w = (int)ttf_text_width(d->regular, TOOLTIP_FONT, text) + 20, h = 26;
    gfx_surface_destroy(d->tooltip);
    d->tooltip = gfx_surface_create(w, h);
    if (!d->tooltip) {
        return;
    }
    struct gfx_rect all = gfx_rect(0, 0, w, h);
    gfx_fill_rounded_rect(d->tooltip, all, 7, GFX_RGBA(0x24, 0x24, 0x2A, 240));
    ttf_draw_text(d->tooltip, d->regular, TOOLTIP_FONT, 10, 17.5f, GFX_RGB(255, 255, 255), text);

    int x = anchor.x + anchor.w / 2 - w / 2;
    int max_x = d->screen.width - w - 6;
    x = x > max_x ? max_x : x < 6 ? 6 : x;
    d->tooltip_rect = gfx_rect(x, d->panel_rect.y - h - 8, w, h);
    d->tooltip_for = which;
    damage(d, d->tooltip_rect);
}

void panel_tick(struct desktop *d) {
    if ((int)(time(NULL) / 60) != d->clock_minute) {
        render_panel(d);
    }
    network_tick(d);
    bool due = d->panel_hover != PANEL_HOVER_NONE && d->panel_pressed == PANEL_HOVER_NONE &&
               !d->menu.open && !d->launcher_open && !d->volume_open && !d->net_open &&
               uptime_ms() - d->hover_since >= TOOLTIP_DELAY;
    if (due && d->tooltip_for != d->panel_hover) {
        show_tooltip(d);
    }
}

int panel_next_timeout(struct desktop *d) {
    int ms = 1000;   /* the clock: checked every second */
    if (d->panel_hover != PANEL_HOVER_NONE && d->tooltip_for != d->panel_hover &&
        d->panel_pressed == PANEL_HOVER_NONE) {
        uint64_t since = uptime_ms() - d->hover_since;
        int left = since >= TOOLTIP_DELAY ? 0 : (int)(TOOLTIP_DELAY - since);
        ms = left < ms ? left : ms;
    }
    return ms;
}

/* --- the mouse on the panel ------------------------------------------------------------------- */

static int hover_at(struct desktop *d, int x, int y) {
    if (!panel_contains(d, x, y)) {
        return PANEL_HOVER_NONE;
    }
    if (gfx_rect_contains(d->menu_slot, x, y)) {
        return PANEL_HOVER_MENU;
    }
    for (int i = 0; i < d->entry_count; i++) {
        if (gfx_rect_contains(d->entries[i].slot, x, y)) {
            return i;
        }
    }
    if (gfx_rect_contains(d->clock_rect, x, y)) {
        return PANEL_HOVER_CLOCK;
    }
    if (gfx_rect_contains(d->volume_slot, x, y)) {
        return PANEL_HOVER_VOLUME;
    }
    if (gfx_rect_contains(d->net_slot, x, y)) {
        return PANEL_HOVER_NETWORK;
    }
    return PANEL_HOVER_NONE;
}

void panel_mouse_move(struct desktop *d) {
    int hover = hover_at(d, d->mouse_x, d->mouse_y);
    if (hover == d->panel_hover) {
        return;
    }
    d->panel_hover = hover;
    d->hover_since = uptime_ms();
    hide_tooltip(d);
    render_panel(d);
}

void panel_mouse_down(struct desktop *d, uint32_t button) {
    hide_tooltip(d);
    int which = d->panel_hover;
    if (which == PANEL_HOVER_NONE || which == PANEL_HOVER_CLOCK) {
        return;
    }
    if (button == OS_MOUSE_RIGHT && which >= 0) {
        launcher_close(d);
        volume_close(d);
        network_close(d);
        struct panel_entry *e = &d->entries[which];
        bool running, front;
        entry_state(d, e, &running, &front);
        struct gui_menu_item items[4];
        int n = 0;
        items[n++] = (struct gui_menu_item){ entry_name(d, e), 0, true, false };
        items[n++] = (struct gui_menu_item){ NULL, 0, false, false };
        if (e->app >= 0) {
            items[n++] = (struct gui_menu_item){ running ? "New window" : "Open", PANEL_MENU_NEW, false, false };
        }
        if (running) {
            items[n++] = (struct gui_menu_item){ "Close", PANEL_MENU_CLOSE, false, false };
        }
        menu_show(d, d->mouse_x, d->mouse_y, items, n, MENU_PANEL);
        d->menu.data = which;
        return;
    }
    d->panel_pressed = which;
    render_panel(d);
}

/* Left click on an app: start it, bring it forward, or minimize it if it's in front. */
static void activate(struct desktop *d, int index) {
    struct panel_entry *e = &d->entries[index];
    struct window *top_visible = NULL, *top_any = NULL;
    for (struct window *w = d->windows; w; w = w->next) {
        if (window_in_entry(w, e)) {
            top_any = w;
            if (w->visible) {
                top_visible = w;
            }
        }
    }
    if (!top_any) {
        if (e->app >= 0) {
            app_launch(d, e->app);
        }
        return;
    }
    if (d->focused && d->focused->visible && window_in_entry(d->focused, e)) {
        window_minimize(d, d->focused);
        return;
    }
    window_show(d, top_visible ? top_visible : top_any);
}

void panel_mouse_up(struct desktop *d, uint32_t button) {
    int pressed = d->panel_pressed;
    d->panel_pressed = PANEL_HOVER_NONE;
    render_panel(d);
    if (pressed != d->panel_hover) {
        return;   /* moved off before releasing */
    }
    if (pressed == PANEL_HOVER_MENU) {
        if (button == OS_MOUSE_LEFT) {
            volume_close(d);
            network_close(d);
            launcher_toggle(d);
        }
        return;
    }
    if (pressed == PANEL_HOVER_VOLUME) {
        if (button == OS_MOUSE_LEFT) {
            network_close(d);
            volume_toggle(d);
        }
        return;
    }
    if (pressed == PANEL_HOVER_NETWORK) {
        if (button == OS_MOUSE_LEFT) {
            network_toggle(d);
        }
        return;
    }
    launcher_close(d);
    volume_close(d);
    network_close(d);
    if (pressed < 0 || pressed >= d->entry_count) {
        return;
    }
    if (button == OS_MOUSE_LEFT) {
        activate(d, pressed);
    } else if (button == OS_MOUSE_MIDDLE && d->entries[pressed].app >= 0) {
        app_launch(d, d->entries[pressed].app);
    }
}

void panel_menu_chosen(struct desktop *d, int entry, int id) {
    if (entry < 0 || entry >= d->entry_count) {
        return;
    }
    struct panel_entry *e = &d->entries[entry];
    if (id == PANEL_MENU_NEW && e->app >= 0) {
        app_launch(d, e->app);
    } else if (id == PANEL_MENU_CLOSE) {
        for (struct window *w = d->windows; w; w = w->next) {
            if (window_in_entry(w, e)) {
                window_close_request(w);
            }
        }
    }
}

/* --- the apps menu ------------------------------------------------------------------------------- */

static char shown_name[64] = "User";
static struct gfx_surface *shown_picture;   /* the user's, round */

/* Where tile `i` is, inside the apps menu (scrolled). */
static struct gfx_rect tile_rect(const struct desktop *d, int i) {
    return gfx_rect(LAUNCHER_PAD + (i % LAUNCHER_COLS) * TILE_W,
                    LAUNCHER_HEAD + (i / LAUNCHER_COLS) * TILE_H - d->launcher_scroll, TILE_W, TILE_H);
}

/* The part of the apps menu the apps scroll in. */
static struct gfx_rect list_rect(const struct desktop *d) {
    return gfx_rect(0, LAUNCHER_HEAD, d->launcher_rect.w, d->launcher_view_h);
}

static bool scrollable(const struct desktop *d) {
    return d->launcher_content_h > d->launcher_view_h;
}

static int max_scroll(const struct desktop *d) {
    return scrollable(d) ? d->launcher_content_h - d->launcher_view_h : 0;
}

/* The scroll bar's track and thumb, inside the apps menu. */
static struct gfx_rect bar_track(const struct desktop *d) {
    return gfx_rect(d->launcher_rect.w - BAR_W - 5, LAUNCHER_HEAD + 4, BAR_W, d->launcher_view_h - 8);
}

static struct gfx_rect bar_thumb(const struct desktop *d) {
    struct gfx_rect t = bar_track(d);
    int h = (int)((long)t.h * d->launcher_view_h / d->launcher_content_h);
    h = h < 30 ? 30 : h > t.h ? t.h : h;
    int y = t.y + (int)((long)(t.h - h) * d->launcher_scroll / (max_scroll(d) ? max_scroll(d) : 1));
    return gfx_rect(t.x, y, t.w, h);
}

/* Is (lx, ly), inside the apps menu, where the scroll bar can be grabbed? */
static bool on_bar(const struct desktop *d, int lx, int ly) {
    return scrollable(d) && lx >= d->launcher_rect.w - BAR_HIT && gfx_rect_contains(list_rect(d), lx, ly);
}

static struct gfx_rect exit_rect(const struct desktop *d) {
    int h = d->launcher_rect.h;
    return gfx_rect(d->launcher_rect.w - LAUNCHER_PAD - 40, h - LAUNCHER_FOOT + (LAUNCHER_FOOT - 40) / 2, 40, 40);
}

/* A power symbol: a ring open at the top, and a line through the gap. */
static void draw_power(struct gfx_surface *s, float cx, float cy, gfx_color ink) {
    const float r = 8, pi = 3.14159265f;
    float start = -pi / 2 + 0.75f, end = -pi / 2 - 0.75f + 2 * pi;
    int steps = 24;
    for (int i = 0; i < steps; i++) {
        float a0 = start + (end - start) * i / steps, a1 = start + (end - start) * (i + 1) / steps;
        gfx_draw_line(s, cx + r * (float)cos(a0), cy + r * (float)sin(a0), cx + r * (float)cos(a1), cy + r * (float)sin(a1), 1.8f, ink);
    }
    gfx_draw_line(s, cx, cy - r - 2, cx, cy - 1, 1.8f, ink);
}

static void render_launcher(struct desktop *d) {
    struct gfx_surface *s = d->launcher;
    struct gfx_rect all = gfx_rect(0, 0, s->width, s->height);
    gfx_clear(s, GFX_RGBA(0, 0, 0, 0));
    gfx_fill_rounded_rect(s, all, PANEL_RADIUS, GFX_RGB(255, 255, 255));
    gfx_stroke_rounded_rect(s, all, PANEL_RADIUS, 1, GFX_RGBA(0, 0, 0, 16));

    if (d->bold) {
        ttf_draw_text(s, d->bold, 15, LAUNCHER_PAD + 8, 30, INK, "Apps");
    }
    /* Under the title: what the app under the mouse does. */
    if (d->regular && d->launcher_hover >= 0 && d->apps[d->launcher_hover].desc[0]) {
        gfx_set_clip(s, gfx_rect(LAUNCHER_PAD + 8, 0, s->width - 2 * LAUNCHER_PAD - 16, LAUNCHER_HEAD));
        ttf_draw_text(s, d->regular, 12, LAUNCHER_PAD + 8, 48, INK_SOFT, d->apps[d->launcher_hover].desc);
        gfx_reset_clip(s);
    }
    struct gfx_rect list = list_rect(d);
    gfx_set_clip(s, list);
    for (int i = 0; i < d->app_count; i++) {
        struct gfx_rect t = tile_rect(d, i);
        if (t.y + t.h <= list.y || t.y >= list.y + list.h) {
            continue;   /* scrolled out of sight */
        }
        if (d->launcher_hover == i) {
            gfx_fill_rounded_rect(s, gfx_rect_inset(t, 3), SLOT_RADIUS, HOVER_FILL);
        }
        const struct gfx_surface *icon = d->apps[i].icon_large;
        if (icon) {
            gfx_blit(s, t.x + (TILE_W - icon->width) / 2, t.y + 10, icon, NULL);
        }
        if (!d->regular) {
            continue;
        }
        /* The name: on two lines if it needs them (split at a space), cut short after that. */
        const float size = 12, max = TILE_W - 10;
        float cx = t.x + TILE_W / 2.0f, y = t.y + 10 + TILE_ICON + 19;
        char first[40], rest[40] = "";
        strlcpy(first, d->apps[i].name, sizeof(first));
        if (ttf_text_width(d->regular, size, first) > max) {
            for (char *sp = strrchr(first, ' '); sp; sp = strrchr(first, ' ')) {
                char tail[40];
                snprintf(tail, sizeof(tail), "%s%s%s", sp + 1, rest[0] ? " " : "", rest);
                strlcpy(rest, tail, sizeof(rest));
                *sp = '\0';
                if (ttf_text_width(d->regular, size, first) <= max) {
                    break;
                }
            }
        }
        const char *lines[2] = { first, rest };
        for (int l = 0; l < 2 && lines[l][0]; l++) {
            char base[40], text[44];
            strlcpy(base, lines[l], sizeof(base));
            strlcpy(text, base, sizeof(text));
            while (strlen(base) > 1 && ttf_text_width(d->regular, size, text) > max) {
                base[strlen(base) - 1] = '\0';
                snprintf(text, sizeof(text), "%s...", base);
            }
            float w = ttf_text_width(d->regular, size, text);
            ttf_draw_text(s, d->regular, size, cx - w / 2, y + l * 15, INK, text);
        }
    }
    gfx_reset_clip(s);
    if (scrollable(d)) {
        /* A line under the title once some apps are scrolled up behind it, and the scroll bar. */
        if (d->launcher_scroll > 0) {
            gfx_fill_rect(s, gfx_rect(0, list.y, s->width, 1), GFX_RGB(0xEC, 0xEC, 0xF0));
        }
        bool hot = d->launcher_bar_drag || on_bar(d, d->mouse_x - d->launcher_rect.x, d->mouse_y - d->launcher_rect.y);
        gfx_fill_rounded_rect(s, bar_track(d), BAR_W / 2.0f, GFX_RGBA(0, 0, 0, hot ? 14 : 0));
        gfx_fill_rounded_rect(s, bar_thumb(d), BAR_W / 2.0f, GFX_RGBA(0, 0, 0, hot ? 110 : 60));
    }

    /* Footer: who's here, and the way out. */
    int fy = s->height - LAUNCHER_FOOT;
    gfx_fill_rect(s, gfx_rect(0, fy, s->width, 1), GFX_RGB(0xEC, 0xEC, 0xF0));
    float cy = fy + LAUNCHER_FOOT / 2.0f;
    if (shown_picture) {
        gfx_blit(s, LAUNCHER_PAD + 24 - 15, (int)cy - 15, shown_picture, NULL);
    } else {
        gfx_fill_circle(s, LAUNCHER_PAD + 24, cy, 15, ACCENT);
        if (d->bold) {
            char initial[2] = { (char)(shown_name[0] >= 'a' && shown_name[0] <= 'z' ? shown_name[0] - 32 : shown_name[0]), 0 };
            float w = ttf_text_width(d->bold, 14, initial);
            ttf_draw_text(s, d->bold, 14, LAUNCHER_PAD + 24 - w / 2, cy + 5, GFX_RGB(255, 255, 255), initial);
        }
    }
    if (d->regular) {
        ttf_draw_text(s, d->regular, 13.5f, LAUNCHER_PAD + 48, cy + 5, INK, shown_name);
    }
    struct gfx_rect e = exit_rect(d);
    if (d->launcher_hover == LAUNCHER_EXIT) {
        gfx_fill_rounded_rect(s, e, SLOT_RADIUS, HOVER_FILL);
    }
    draw_power(s, e.x + e.w / 2.0f, e.y + e.h / 2.0f + 1, d->launcher_hover == LAUNCHER_EXIT ? INK : INK_SOFT);

    damage(d, d->launcher_rect);
}

static struct gfx_rect launcher_bounds(const struct desktop *d) {
    struct gfx_rect b = gfx_rect_inset(d->launcher_rect, -PANEL_SHADOW);
    b.h += 4;
    return b;
}

static void find_user_name(void) {
    strlcpy(shown_name, session_display_name(), sizeof(shown_name));
    struct user_account a;
    gfx_surface_destroy(shown_picture);
    shown_picture = users_picture(users_find(user_name(), &a) ? &a : NULL, 30);
}

static void launcher_open(struct desktop *d) {
    int rows = (d->app_count + LAUNCHER_COLS - 1) / LAUNCHER_COLS;
    rows = rows < 1 ? 1 : rows;
    int w = 2 * LAUNCHER_PAD + LAUNCHER_COLS * TILE_W;
    int h = LAUNCHER_HEAD + rows * TILE_H + 10 + LAUNCHER_FOOT;
    int room = d->panel_rect.y - LAUNCHER_GAP - LAUNCHER_TOP;
    int least = LAUNCHER_HEAD + TILE_H + 10 + LAUNCHER_FOOT;
    if (h > room) {
        h = room > least ? room : least;   /* the rest scrolls */
    }
    d->launcher_rect = gfx_rect(d->panel_rect.x, d->panel_rect.y - LAUNCHER_GAP - h, w, h);
    d->launcher_content_h = rows * TILE_H + 10;
    d->launcher_view_h = h - LAUNCHER_HEAD - LAUNCHER_FOOT;
    d->launcher_scroll = 0;
    d->launcher_bar_drag = false;
    d->launcher = gfx_surface_create(w, h);
    d->launcher_shadow = gfx_shadow_create(w, h, PANEL_RADIUS, PANEL_SHADOW);
    if (!d->launcher || !d->launcher_shadow) {
        gfx_surface_destroy(d->launcher);
        gfx_alpha_destroy(d->launcher_shadow);
        d->launcher = NULL;
        d->launcher_shadow = NULL;
        return;
    }
    find_user_name();
    hide_tooltip(d);
    d->launcher_open = true;
    d->launcher_hover = -1;
    render_launcher(d);
    damage(d, launcher_bounds(d));
    render_panel(d);   /* the button stays highlighted while it's open */
}

void launcher_close(struct desktop *d) {
    if (!d->launcher_open) {
        return;
    }
    damage(d, launcher_bounds(d));
    gfx_surface_destroy(d->launcher);
    gfx_alpha_destroy(d->launcher_shadow);
    d->launcher = NULL;
    d->launcher_shadow = NULL;
    d->launcher_open = false;
    d->launcher_bar_drag = false;
    render_panel(d);
}

void launcher_toggle(struct desktop *d) {
    if (d->launcher_open) {
        launcher_close(d);
    } else {
        launcher_open(d);
    }
}

bool launcher_contains(const struct desktop *d, int x, int y) {
    return d->launcher_open && gfx_rect_contains(d->launcher_rect, x, y);
}

static int launcher_item_at(struct desktop *d, int x, int y) {
    if (!launcher_contains(d, x, y)) {
        return -1;
    }
    int lx = x - d->launcher_rect.x, ly = y - d->launcher_rect.y;
    for (int i = 0; gfx_rect_contains(list_rect(d), lx, ly) && !on_bar(d, lx, ly) && i < d->app_count; i++) {
        if (gfx_rect_contains(gfx_rect_inset(tile_rect(d, i), 3), lx, ly)) {
            return i;
        }
    }
    if (gfx_rect_contains(exit_rect(d), lx, ly)) {
        return LAUNCHER_EXIT;
    }
    return -1;
}

static void scroll_to(struct desktop *d, int scroll) {
    scroll = scroll < 0 ? 0 : scroll > max_scroll(d) ? max_scroll(d) : scroll;
    if (scroll != d->launcher_scroll) {
        d->launcher_scroll = scroll;
        d->launcher_hover = launcher_item_at(d, d->mouse_x, d->mouse_y);
        render_launcher(d);
    }
}

bool launcher_wheel(struct desktop *d, int wheel) {
    if (!d->launcher_open || !launcher_contains(d, d->mouse_x, d->mouse_y) || !scrollable(d)) {
        return false;
    }
    scroll_to(d, d->launcher_scroll + wheel * WHEEL_STEP);
    return true;
}

void launcher_mouse_down(struct desktop *d) {
    int lx = d->mouse_x - d->launcher_rect.x, ly = d->mouse_y - d->launcher_rect.y;
    if (!d->launcher_open || !on_bar(d, lx, ly)) {
        return;
    }
    struct gfx_rect thumb = bar_thumb(d);
    if (ly >= thumb.y && ly < thumb.y + thumb.h) {
        d->launcher_bar_grab = ly - thumb.y;
    } else {
        /* On the track: the thumb jumps there (centred on the pointer), then follows it. */
        d->launcher_bar_grab = thumb.h / 2;
    }
    d->launcher_bar_drag = true;
    launcher_mouse_move(d);
}

bool launcher_mouse_up(struct desktop *d) {
    if (!d->launcher_bar_drag) {
        return false;
    }
    d->launcher_bar_drag = false;
    if (d->launcher_open) {
        render_launcher(d);
    }
    return true;
}

void launcher_mouse_move(struct desktop *d) {
    if (!d->launcher_open) {
        return;
    }
    if (d->launcher_bar_drag) {
        struct gfx_rect track = bar_track(d), thumb = bar_thumb(d);
        int top = d->mouse_y - d->launcher_rect.y - d->launcher_bar_grab - track.y;
        int span = track.h - thumb.h;
        scroll_to(d, span > 0 ? (int)((long)top * max_scroll(d) / span) : 0);
        return;
    }
    int hover = launcher_item_at(d, d->mouse_x, d->mouse_y);
    bool was_on_bar = d->launcher_hover == -2;
    bool now_on_bar = on_bar(d, d->mouse_x - d->launcher_rect.x, d->mouse_y - d->launcher_rect.y);
    if (hover != d->launcher_hover || was_on_bar != now_on_bar) {
        d->launcher_hover = hover == -1 && now_on_bar ? -2 : hover;   /* -2: over the scroll bar */
        render_launcher(d);
    }
}

void launcher_click(struct desktop *d) {
    int item = launcher_item_at(d, d->mouse_x, d->mouse_y);
    if (item == LAUNCHER_EXIT) {
        const struct gui_menu_item items[] = {
            { "Lock", POWER_MENU_LOCK, false, false },
            { "Log out", POWER_MENU_LOGOUT, false, false },
            { NULL, 0, false, false },
            { "Shut down", POWER_MENU_OFF, false, false },
            { "Restart", POWER_MENU_RESTART, false, false },
            { NULL, 0, false, false },
            { "Exit to the text shell", POWER_MENU_EXIT, false, false },
        };
        struct gfx_rect e = exit_rect(d);
        menu_show(d, d->launcher_rect.x + e.x, d->launcher_rect.y + e.y, items, 7, MENU_POWER);
        if (d->menu.open) {
            /* Just above the button, right edges lined up. */
            damage(d, menu_bounds(&d->menu));
            d->menu.rect.x = d->launcher_rect.x + e.x + e.w - d->menu.rect.w;
            d->menu.rect.y = d->launcher_rect.y + e.y - d->menu.rect.h - 6;
            damage(d, menu_bounds(&d->menu));
        }
    } else if (item >= 0) {
        launcher_close(d);
        app_launch(d, item);
    }
}

void launcher_compose(struct desktop *d, struct gfx_rect area) {
    if (!d->launcher_open || gfx_rect_empty(gfx_rect_intersect(launcher_bounds(d), area))) {
        return;
    }
    gfx_fill_mask(&d->screen, d->launcher_rect.x - PANEL_SHADOW, d->launcher_rect.y - PANEL_SHADOW + 4,
                  d->launcher_shadow, GFX_RGBA(0, 0, 0, 80));
    gfx_blit(&d->screen, d->launcher_rect.x, d->launcher_rect.y, d->launcher, NULL);
}

void power_menu_chosen(struct desktop *d, int id) {
    if (id == POWER_MENU_EXIT) {
        d->exit_code = EXIT_TO_SHELL;
        d->quit = true;
        return;
    }
    if (id == POWER_MENU_LOCK) {
        launcher_close(d);
        lock_screen(d);
        return;
    }
    if (id == POWER_MENU_LOGOUT) {
        log_out(d);
        return;
    }
    if (id != POWER_MENU_OFF && id != POWER_MENU_RESTART) {
        return;
    }
    /* The apps closed and the drives unmounted first (shutdown.c). */
    shutdown_begin(d, id == POWER_MENU_RESTART);
}
