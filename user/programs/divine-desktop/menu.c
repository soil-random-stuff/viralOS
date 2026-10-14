/*
 * Context menus: a rounded, shadowed list that pops up at the mouse. The
 * item under the pointer is highlighted; releasing a button over an item,
 * or pressing Enter, chooses it.
 */

#include <stdlib.h>
#include <string.h>

#include "desktop.h"

#define ITEM_HEIGHT      28
#define SEPARATOR_HEIGHT 9
#define PAD_Y            6
#define PAD_LEFT         14
#define CHECK_SPACE      20
#define PAD_RIGHT        22
#define MENU_RADIUS      9.0f
#define MENU_SHADOW      14


enum { WINDOW_MENU_MINIMIZE = 1, WINDOW_MENU_MAXIMIZE, WINDOW_MENU_CLOSE };

static bool is_separator(const struct gui_menu_item *item) {
    return item->label == NULL;
}

struct gfx_rect menu_bounds(const struct menu *m) {
    struct gfx_rect b = gfx_rect_inset(m->rect, -18);
    b.h += 6;
    return b;
}

/* y of each item's top edge inside the menu; returns the total height. */
static int layout(const struct menu *m, int *tops) {
    int y = PAD_Y;
    for (int i = 0; i < m->count; i++) {
        tops[i] = y;
        y += is_separator(&m->items[i]) ? SEPARATOR_HEIGHT : ITEM_HEIGHT;
    }
    return y + PAD_Y;
}

static void render_menu(struct desktop *d) {
    struct menu *m = &d->menu;
    struct gfx_surface *s = m->surface;
    struct ttf_font *font = d->regular;
    int tops[MENU_MAX_ITEMS];
    layout(m, tops);

    bool any_checks = false;
    for (int i = 0; i < m->count; i++) {
        any_checks |= m->items[i].checked;
    }
    int text_x = PAD_LEFT + (any_checks ? CHECK_SPACE : 0);

    gfx_clear(s, GFX_RGBA(0, 0, 0, 0));
    struct gfx_rect all = gfx_rect(0, 0, s->width, s->height);
    gfx_fill_rounded_rect(s, all, MENU_RADIUS, GFX_RGBA(0xFC, 0xFC, 0xFD, 250));
    gfx_stroke_rounded_rect(s, all, MENU_RADIUS, 1, GFX_RGBA(0, 0, 0, 40));

    for (int i = 0; i < m->count; i++) {
        const struct gui_menu_item *item = &m->items[i];
        if (is_separator(item)) {
            gfx_fill_rect(s, gfx_rect(10, tops[i] + SEPARATOR_HEIGHT / 2, s->width - 20, 1),
                          GFX_RGB(0xE1, 0xE1, 0xE6));
            continue;
        }
        bool hot = i == m->hover && !item->disabled;
        if (hot) {
            gfx_fill_rounded_rect(s, gfx_rect(5, tops[i] + 1, s->width - 10, ITEM_HEIGHT - 2), 6, ACCENT);
        }
        gfx_color ink = item->disabled ? GFX_RGB(0xA6, 0xA6, 0xAE)
                                       : hot ? GFX_RGB(255, 255, 255) : GFX_RGB(0x24, 0x24, 0x2A);
        float baseline = (float)tops[i] + ITEM_HEIGHT / 2.0f + 5;
        if (font) {
            if (item->checked) {
                ttf_draw_text(s, font, MENU_FONT_SIZE, PAD_LEFT, baseline, ink, "\xE2\x9C\x93");  /* ✓ */
            }
            /* "Label\tCtrl+S": the part after the tab is a shortcut, right-aligned and softer. */
            char label[64];
            strlcpy(label, item->label, sizeof(label));
            char *tab = strchr(label, '\t');
            if (tab) {
                *tab++ = '\0';
                float sw = ttf_text_width(font, MENU_FONT_SIZE, tab);
                gfx_color soft = item->disabled ? ink : hot ? GFX_RGBA(255, 255, 255, 200)
                                                             : GFX_RGB(0x8A, 0x8A, 0x94);
                ttf_draw_text(s, font, MENU_FONT_SIZE, s->width - PAD_LEFT - sw, baseline, soft, tab);
            }
            ttf_draw_text(s, font, MENU_FONT_SIZE, (float)text_x, baseline, ink, label);
        }
    }
    damage(d, m->rect);
}

void menu_show(struct desktop *d, int x, int y, const struct gui_menu_item *items, int count,
               enum menu_kind kind) {
    menu_close(d);
    struct menu *m = &d->menu;
    if (count > MENU_MAX_ITEMS) {
        count = MENU_MAX_ITEMS;
    }
    m->count = count;
    m->kind = kind;
    m->hover = -1;

    /* Keep our own copies: the caller's strings may not outlive the menu. */
    struct ttf_font *font = d->regular;
    float widest = 0;
    bool any_checks = false;
    for (int i = 0; i < count; i++) {
        m->items[i] = items[i];
        if (items[i].label) {
            strlcpy(m->labels[i], items[i].label, sizeof(m->labels[i]));
            m->items[i].label = m->labels[i];
            float wdt = font ? ttf_text_width(font, MENU_FONT_SIZE, m->labels[i]) : 100;
            if (font && strchr(m->labels[i], '\t')) {
                wdt += 36;   /* room between the label and its shortcut */
            }
            widest = wdt > widest ? wdt : widest;
        }
        any_checks |= items[i].checked;
    }
    int tops[MENU_MAX_ITEMS];
    int h = layout(m, tops);
    int w = PAD_LEFT + (any_checks ? CHECK_SPACE : 0) + (int)widest + PAD_RIGHT;
    w = w < 170 ? 170 : w;

    /* Open below-right of the pointer, flipped if it would run off the screen. */
    x += 2;
    y += 2;
    if (x + w > d->screen.width) {
        x = x - w - 4 > 0 ? x - w - 4 : d->screen.width - w;
    }
    if (y + h > d->screen.height) {
        y = y - h - 4 > 0 ? y - h - 4 : d->screen.height - h;
    }
    m->rect = gfx_rect(x, y, w, h);
    m->surface = gfx_surface_create(w, h);
    m->shadow = gfx_shadow_create(w, h, MENU_RADIUS, MENU_SHADOW);
    if (!m->surface) {
        gfx_alpha_destroy(m->shadow);
        m->shadow = NULL;
        return;
    }
    m->open = true;
    render_menu(d);
    damage(d, menu_bounds(m));
}

void menu_close(struct desktop *d) {
    struct menu *m = &d->menu;
    if (!m->open) {
        return;
    }
    damage(d, menu_bounds(m));
    gfx_surface_destroy(m->surface);
    gfx_alpha_destroy(m->shadow);
    m->surface = NULL;
    m->shadow = NULL;
    m->open = false;
}

bool menu_contains(const struct desktop *d, int x, int y) {
    return d->menu.open && gfx_rect_contains(d->menu.rect, x, y);
}

void menu_hover(struct desktop *d, int x, int y) {
    struct menu *m = &d->menu;
    int hover = -1;
    if (menu_contains(d, x, y)) {
        int tops[MENU_MAX_ITEMS];
        layout(m, tops);
        int ly = y - m->rect.y;
        for (int i = 0; i < m->count; i++) {
            int h = is_separator(&m->items[i]) ? SEPARATOR_HEIGHT : ITEM_HEIGHT;
            if (ly >= tops[i] && ly < tops[i] + h && !is_separator(&m->items[i]) &&
                !m->items[i].disabled) {
                hover = i;
            }
        }
    }
    if (hover != m->hover) {
        m->hover = hover;
        render_menu(d);
    }
}

void menu_choose(struct desktop *d) {
    struct menu *m = &d->menu;
    if (!m->open || m->hover < 0) {
        return;
    }
    struct menu saved = *m;   /* closing frees the pictures, not these fields */
    int id = m->items[m->hover].id;
    menu_close(d);

    switch (saved.kind) {
    case MENU_WINDOW:
        if (id == WINDOW_MENU_MINIMIZE) {
            window_minimize(d, saved.window);
        } else if (id == WINDOW_MENU_MAXIMIZE) {
            window_toggle_maximize(d, saved.window);
        } else if (id == WINDOW_MENU_CLOSE) {
            window_close_request(saved.window);
        }
        break;
    case MENU_CLIENT: {
        struct gui_msg_event ev = {
            .header = { GUI_MSG_EVENT, saved.window_id },
            .type = GUI_EVENT_MENU,
            .menu_id = id,
        };
        client_send(saved.client, &ev, sizeof(ev));
        break;
    }
    case MENU_DESKTOP:
    case MENU_ICON:
        icons_menu_chosen(d, id);
        break;
    case MENU_PANEL:
        panel_menu_chosen(d, saved.data, id);
        break;
    case MENU_POWER:
        power_menu_chosen(d, id);
        break;
    }
}

/* Up/Down move between enabled items, Enter chooses, Escape closes. */
void menu_key(struct desktop *d, uint32_t key) {
    struct menu *m = &d->menu;
    if (key == OS_KEY_ESCAPE) {
        menu_close(d);
        return;
    }
    if (key == OS_KEY_ENTER) {
        menu_choose(d);
        return;
    }
    int step = key == OS_KEY_DOWN ? 1 : key == OS_KEY_UP ? -1 : 0;
    if (!step) {
        return;
    }
    int i = m->hover;
    for (int tries = 0; tries < m->count; tries++) {
        i = i < 0 ? (step > 0 ? 0 : m->count - 1) : (i + step + m->count) % m->count;
        if (!is_separator(&m->items[i]) && !m->items[i].disabled) {
            m->hover = i;
            render_menu(d);
            return;
        }
    }
}

void show_window_menu(struct desktop *d, struct window *w, int x, int y) {
    const struct gui_menu_item items[] = {
        { "Minimize", WINDOW_MENU_MINIMIZE, false, false },
        { w->maximized ? "Restore" : "Maximize", WINDOW_MENU_MAXIMIZE, false, false },
        { NULL, 0, false, false },
        { "Close", WINDOW_MENU_CLOSE, false, false },
    };
    menu_show(d, x, y, items, 4, MENU_WINDOW);
    d->menu.window = w;
}
