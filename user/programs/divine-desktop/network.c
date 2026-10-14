/*
 * The network in the panel's system tray, left of the volume: Ethernet.png while the computer is
 * online, NoInternet.png otherwise (/usr/share/dde-res/Icons/Status). Clicking it opens a card laid out like
 * Windows 10's network flyout:
 *
 *   [icon]  Ethernet (eth0)                       <- one entry per adapter; click to select it
 *           Connected, Internet access
 *           Properties                [Disconnect] <- the selected one
 *   -------------------------------------------
 *   Network & Internet settings                    <- opens Settings on its Network page
 *   Change settings, such as using a fixed address.
 *
 * The adapters are read with netif_info() every couple of seconds (and when the card opens);
 * connecting and disconnecting go through libnetconf, which also saves the choice.
 */

#include <image.h>
#include <netconf.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "desktop.h"
#include <paths.h>

#define TRAY_ICON     22
#define CARD_ICON     30
#define CARD_W        360
#define ENTRY_H       66
#define ENTRY_OPEN_H  112     /* the selected entry: its link and button too */
#define FOOT_H        66
#define EMPTY_H       70
#define GAP           10
#define CHECK_MS      2000
#define BUTTON_W      112
#define BUTTON_H      32

#define INK           GFX_RGB(0x1C, 0x1C, 0x22)
#define INK_SOFT      GFX_RGB(0x6A, 0x6A, 0x74)
#define LINE          GFX_RGB(0xD4, 0xD4, 0xDA)
#define HOVER_FILL    GFX_RGBA(0, 0, 0, 14)
#define SELECT_FILL   GFX_RGBA(0x2F, 0x6F, 0xEB, 26)
#define BUTTON_FILL   GFX_RGB(0xE8, 0xE8, 0xEC)

enum { N_NONE = -1, N_SETTINGS = -2, N_BUTTON = -3, N_PROPERTIES = -4 };   /* >= 0: an entry */

static struct gfx_surface *load_icon(const char *path, int size) {
    struct gfx_surface *img = image_load(path, NULL);
    if (!img) {
        return NULL;
    }
    struct gfx_surface *s = gfx_surface_scaled(img, size, size);
    gfx_surface_destroy(img);
    return s;
}

/* Re-read the adapters; true if anything shown changed. */
static bool refresh(struct desktop *d) {
    struct os_netif_info ifs[NET_MAX];
    int n = 0;
    for (int i = 0; n < NET_MAX; i++) {
        struct os_netif_info info;
        if (netif_info(i, &info) != 0) {
            break;
        }
        if (!(info.flags & OS_NETIF_LOOPBACK)) {
            ifs[n++] = info;
        }
    }
    bool online = false;
    for (int i = 0; i < n; i++) {
        online |= netconf_online(&ifs[i]);
    }
    bool changed = n != d->net_count || online != d->net_online;
    for (int i = 0; i < n && !changed; i++) {
        changed = ifs[i].state != d->net_ifs[i].state || ifs[i].address != d->net_ifs[i].address ||
                  ifs[i].gateway != d->net_ifs[i].gateway;
    }
    memcpy(d->net_ifs, ifs, sizeof(ifs[0]) * (size_t)n);
    d->net_count = n;
    d->net_online = online;
    d->net_checked_ms = uptime_ms();
    if (d->net_selected >= n) {
        d->net_selected = n ? 0 : -1;
    }
    return changed;
}

void network_init(struct desktop *d) {
    d->net_tray_icons[0] = load_icon(_PATH_DDE_RES "/Icons/Status/NoInternet.png", TRAY_ICON);
    d->net_tray_icons[1] = load_icon(_PATH_DDE_RES "/Icons/Status/Ethernet.png", TRAY_ICON);
    d->net_card_icons[0] = load_icon(_PATH_DDE_RES "/Icons/Status/NoInternet.png", CARD_ICON);
    d->net_card_icons[1] = load_icon(_PATH_DDE_RES "/Icons/Status/Ethernet.png", CARD_ICON);
    d->net_hover = N_NONE;
    d->net_selected = -1;
    refresh(d);
}

/* --- in the panel ------------------------------------------------------------------------------- */

void network_draw_tray(struct desktop *d, struct gfx_surface *panel, bool hovered, bool pressed) {
    int size = PANEL_HEIGHT - 12;
    d->net_slot = gfx_rect(d->volume_slot.x - 2 - size, d->panel_rect.y + 6, size, size);
    struct gfx_rect r = gfx_rect(d->net_slot.x - d->panel_rect.x, 6, size, size);
    if (pressed || d->net_open) {
        gfx_fill_rounded_rect(panel, r, 10, GFX_RGBA(0, 0, 0, 30));
    } else if (hovered) {
        gfx_fill_rounded_rect(panel, r, 10, GFX_RGBA(0, 0, 0, 16));
    }
    struct gfx_surface *icon = d->net_tray_icons[d->net_online ? 1 : 0];
    if (icon) {
        gfx_blit(panel, r.x + (size - TRAY_ICON) / 2, r.y + (size - TRAY_ICON) / 2, icon, NULL);
    }
}

void network_tooltip(const struct desktop *d, char *text, size_t size) {
    if (d->net_count == 0) {
        strlcpy(text, "No network adapters", size);
        return;
    }
    for (int i = 0; i < d->net_count; i++) {
        if (netconf_online(&d->net_ifs[i])) {
            snprintf(text, size, "%s: Internet access", d->net_ifs[i].name);
            return;
        }
    }
    snprintf(text, size, "%s: %s", d->net_ifs[0].name, netconf_state_text(&d->net_ifs[0]));
}

/* Every couple of seconds: has the network changed? */
void network_tick(struct desktop *d) {
    if (uptime_ms() - d->net_checked_ms < CHECK_MS) {
        return;
    }
    if (refresh(d)) {
        panel_redraw(d);
        if (d->net_open) {
            network_close(d);
            network_toggle(d);   /* (its size may have changed) */
        }
    }
}

/* --- the card ------------------------------------------------------------------------------------- */

static int entry_height(const struct desktop *d, int i) {
    return i == d->net_selected ? ENTRY_OPEN_H : ENTRY_H;
}

static int entries_height(const struct desktop *d) {
    if (d->net_count == 0) {
        return EMPTY_H;
    }
    int h = 0;
    for (int i = 0; i < d->net_count; i++) {
        h += entry_height(d, i);
    }
    return h;
}

static struct gfx_rect entry_rect(const struct desktop *d, int index) {
    int y = 0;
    for (int i = 0; i < index; i++) {
        y += entry_height(d, i);
    }
    return gfx_rect(0, y, CARD_W, entry_height(d, index));
}

static struct gfx_rect button_rect(const struct desktop *d) {
    struct gfx_rect e = entry_rect(d, d->net_selected);
    return gfx_rect(CARD_W - 16 - BUTTON_W, e.y + e.h - 14 - BUTTON_H, BUTTON_W, BUTTON_H);
}

static struct gfx_rect properties_rect(const struct desktop *d) {
    struct gfx_rect e = entry_rect(d, d->net_selected);
    return gfx_rect(60, e.y + e.h - 14 - BUTTON_H + 6, 84, 20);
}

static int item_at(const struct desktop *d, int x, int y) {
    if (!d->net_open || !gfx_rect_contains(d->net_rect, x, y)) {
        return N_NONE;
    }
    int lx = x - d->net_rect.x, ly = y - d->net_rect.y;
    if (ly >= entries_height(d) + 1) {
        return N_SETTINGS;
    }
    if (d->net_selected >= 0) {
        if (gfx_rect_contains(button_rect(d), lx, ly)) {
            return N_BUTTON;
        }
        if (gfx_rect_contains(properties_rect(d), lx, ly)) {
            return N_PROPERTIES;
        }
    }
    for (int i = 0; i < d->net_count; i++) {
        if (gfx_rect_contains(entry_rect(d, i), lx, ly)) {
            return i;
        }
    }
    return N_NONE;
}

static void render_card(struct desktop *d) {
    struct gfx_surface *s = d->net_card;
    int w = s->width, h = s->height;
    gfx_clear(s, GFX_RGBA(0, 0, 0, 0));
    gfx_fill_rounded_rect(s, gfx_rect(0, 0, w, h), PANEL_RADIUS, GFX_RGB(255, 255, 255));
    gfx_stroke_rounded_rect(s, gfx_rect(0, 0, w, h), PANEL_RADIUS, 1, GFX_RGBA(0, 0, 0, 16));
    struct ttf_font *font = d->regular, *bold = d->bold;
    int top = entries_height(d);

    gfx_set_clip(s, gfx_rect(0, 0, w, top));   /* (keeps the rounded top corners clean) */
    if (d->net_count == 0 && font) {
        ttf_draw_text(s, font, 14, 20, EMPTY_H / 2 + 5, INK_SOFT, "No network adapters found");
    }
    for (int i = 0; i < d->net_count; i++) {
        const struct os_netif_info *n = &d->net_ifs[i];
        struct gfx_rect e = entry_rect(d, i);
        if (i == d->net_selected) {
            gfx_fill_rounded_rect(s, gfx_rect(0, e.y, w, e.h + (i == 0 ? PANEL_RADIUS : 0)),
                                  i == 0 ? PANEL_RADIUS : 0, SELECT_FILL);
        } else if (d->net_hover == i) {
            gfx_fill_rounded_rect(s, gfx_rect(0, e.y, w, e.h + (i == 0 ? PANEL_RADIUS : 0)),
                                  i == 0 ? PANEL_RADIUS : 0, HOVER_FILL);
        }
        struct gfx_surface *icon = d->net_card_icons[netconf_online(n) ? 1 : 0];
        if (icon) {
            gfx_blit(s, 18, e.y + 17, icon, NULL);
        }
        char title[64];
        snprintf(title, sizeof(title), "Ethernet (%s)", n->name);
        if (bold) {
            ttf_draw_text(s, bold, 14, 60, e.y + 29, INK, title);
        }
        if (font) {
            ttf_draw_text(s, font, 13, 60, e.y + 48, INK_SOFT, netconf_state_text(n));
        }
        if (i == d->net_selected) {
            struct gfx_rect pr = properties_rect(d);
            if (font) {
                ttf_draw_text(s, font, 13, pr.x, pr.y + 15, d->net_hover == N_PROPERTIES ? ACCENT : INK, "Properties");
                if (d->net_hover == N_PROPERTIES) {
                    gfx_fill_rect(s, gfx_rect(pr.x, pr.y + 18, (int)ttf_text_width(font, 13, "Properties"), 1), ACCENT);
                }
            }
            struct gfx_rect br = button_rect(d);
            gfx_fill_rounded_rect(s, br, 8, d->net_hover == N_BUTTON ? GFX_RGB(0xD8, 0xD8, 0xDE) : BUTTON_FILL);
            const char *label = n->state == OS_NETIF_DISCONNECTED ? "Connect" : "Disconnect";
            if (font) {
                float tw = ttf_text_width(font, 13.5f, label);
                ttf_draw_text(s, font, 13.5f, br.x + (br.w - tw) / 2, br.y + 21, INK, label);
            }
        }
    }
    gfx_reset_clip(s);
    gfx_fill_rect(s, gfx_rect(0, top, w, 1), LINE);

    /* The way to Settings. */
    struct gfx_rect foot = gfx_rect(0, top + 1, w, FOOT_H);
    if (d->net_hover == N_SETTINGS) {
        gfx_set_clip(s, foot);
        gfx_fill_rounded_rect(s, gfx_rect(0, foot.y - PANEL_RADIUS, w, foot.h + PANEL_RADIUS), PANEL_RADIUS, HOVER_FILL);
        gfx_reset_clip(s);
    }
    if (bold) {
        ttf_draw_text(s, bold, 14, 20, foot.y + 28, ACCENT, "Network & Internet settings");
    }
    if (font) {
        ttf_draw_text(s, font, 12.5f, 20, foot.y + 48, INK_SOFT, "Change settings, such as using a fixed address.");
    }
    damage(d, d->net_rect);
}

static struct gfx_rect card_bounds(const struct desktop *d) {
    struct gfx_rect b = gfx_rect_inset(d->net_rect, -PANEL_SHADOW);
    b.h += 4;
    return b;
}

static bool layout_card(struct desktop *d) {
    int h = entries_height(d) + 1 + FOOT_H;
    if (d->net_card && d->net_card->height == h) {
        return true;
    }
    if (d->net_open) {
        damage(d, card_bounds(d));
    }
    gfx_surface_destroy(d->net_card);
    gfx_alpha_destroy(d->net_shadow);
    d->net_card = gfx_surface_create(CARD_W, h);
    d->net_shadow = gfx_shadow_create(CARD_W, h, PANEL_RADIUS, PANEL_SHADOW);
    d->net_rect = gfx_rect(d->panel_rect.x + d->panel_rect.w - CARD_W, d->panel_rect.y - GAP - h, CARD_W, h);
    return d->net_card && d->net_shadow;
}

void network_close(struct desktop *d) {
    if (!d->net_open) {
        return;
    }
    damage(d, card_bounds(d));
    gfx_surface_destroy(d->net_card);
    gfx_alpha_destroy(d->net_shadow);
    d->net_card = NULL;
    d->net_shadow = NULL;
    d->net_open = false;
    panel_redraw(d);
}

void network_toggle(struct desktop *d) {
    if (d->net_open) {
        network_close(d);
        return;
    }
    launcher_close(d);
    volume_close(d);
    refresh(d);
    if (d->net_selected < 0 && d->net_count) {
        d->net_selected = 0;
    }
    for (int i = 0; i < d->net_count; i++) {   /* the one carrying the internet, if any */
        if (d->net_ifs[i].flags & OS_NETIF_DEFAULT_ROUTE) {
            d->net_selected = i;
        }
    }
    d->net_hover = N_NONE;
    if (!layout_card(d)) {
        return;
    }
    d->net_open = true;
    render_card(d);
    damage(d, card_bounds(d));
    panel_redraw(d);
}

bool network_contains(const struct desktop *d, int x, int y) {
    return d->net_open && gfx_rect_contains(d->net_rect, x, y);
}

void network_mouse_move(struct desktop *d) {
    if (!d->net_open) {
        return;
    }
    int hover = item_at(d, d->mouse_x, d->mouse_y);
    if (hover != d->net_hover) {
        d->net_hover = hover;
        render_card(d);
    }
}

static void open_settings(struct desktop *d) {
    network_close(d);
    app_launch_path(d, _PATH_BIN "/divine-settings", "network");
}

void network_mouse_up(struct desktop *d) {
    int item = item_at(d, d->mouse_x, d->mouse_y);
    if (item == N_SETTINGS || item == N_PROPERTIES) {
        open_settings(d);
    } else if (item == N_BUTTON && d->net_selected >= 0) {
        struct os_netif_info *n = &d->net_ifs[d->net_selected];
        netconf_set_power(n, n->state == OS_NETIF_DISCONNECTED);
        refresh(d);
        panel_redraw(d);
        if (layout_card(d)) {
            render_card(d);
            damage(d, card_bounds(d));
        }
    } else if (item >= 0 && item != d->net_selected) {
        d->net_selected = item;
        if (layout_card(d)) {
            render_card(d);
            damage(d, card_bounds(d));
        }
    }
}

bool network_hover_clickable(const struct desktop *d) {
    return d->net_open && d->net_hover != N_NONE;
}

void network_compose(struct desktop *d, struct gfx_rect area) {
    if (!d->net_open || gfx_rect_empty(gfx_rect_intersect(card_bounds(d), area))) {
        return;
    }
    gfx_fill_mask(&d->screen, d->net_rect.x - PANEL_SHADOW, d->net_rect.y - PANEL_SHADOW + 4, d->net_shadow,
                  GFX_RGBA(0, 0, 0, 80));
    gfx_blit(&d->screen, d->net_rect.x, d->net_rect.y, d->net_card, NULL);
}
