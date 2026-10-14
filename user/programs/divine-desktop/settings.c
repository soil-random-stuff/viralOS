/*
 * The desktop's settings (~/.config/dde/settings.conf, see <config.h>), read at start and again when Settings
 * says they changed (GUI_MSG_SETTINGS): the wallpaper, the clock, the wheel, double-click speed,
 * and the icons' size, arrangement and the pinned apps (which live elsewhere, so they come in the
 * message or are re-read from /usr/share/applications).
 */

#include <config.h>
#include <string.h>

#include "desktop.h"
#include <paths.h>

#define DEFAULT_WALLPAPER _PATH_DDE_RES "/Backgrounds/default.png"

void dde_load_settings(struct desktop *d) {
    if (!config_get(DDE_CONFIG, "Wallpaper", d->cfg.wallpaper, sizeof(d->cfg.wallpaper)) || !d->cfg.wallpaper[0]) {
        strlcpy(d->cfg.wallpaper, DEFAULT_WALLPAPER, sizeof(d->cfg.wallpaper));
    }
    d->cfg.clock24 = config_get_int(DDE_CONFIG, "Clock24", 1) != 0;
    d->cfg.natural_scroll = config_get_int(DDE_CONFIG, "NaturalScroll", 0) != 0;
    long dc = config_get_int(DDE_CONFIG, "DoubleClick", 450);
    d->cfg.double_click = dc < 150 ? 150 : dc > 1500 ? 1500 : (int)dc;
}

void dde_settings_changed(struct desktop *d, const struct gui_msg_settings *m) {
    char old_wallpaper[sizeof(d->cfg.wallpaper)];
    strlcpy(old_wallpaper, d->cfg.wallpaper, sizeof(old_wallpaper));
    dde_load_settings(d);
    if (strcmp(old_wallpaper, d->cfg.wallpaper) != 0) {
        struct gfx_surface *w = make_wallpaper(d);
        if (w) {
            gfx_surface_destroy(d->wallpaper);
            d->wallpaper = w;
            damage(d, gfx_rect(0, 0, d->screen.width, d->screen.height));
        }
    }
    if (m->icon_size >= 0 && m->icon_size <= 2) {
        icons_set_size(d, m->icon_size);
    }
    if (m->auto_arrange >= 0) {
        icons_set_auto_arrange(d, m->auto_arrange != 0);
    }
    apps_reload_pinned(d);
    panel_update(d);
}
