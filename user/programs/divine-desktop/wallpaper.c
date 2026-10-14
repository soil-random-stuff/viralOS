/*
 * The wallpaper: the picture in ~/.config/dde/settings.conf (Settings > Wallpaper), by default
 * /usr/share/dde-res/Backgrounds/default.png. It fills the whole screen without
 * being stretched: scaled just big enough to cover both directions, with
 * the overflow cropped evenly from the sides (or top and bottom).
 */

#include <image.h>
#include <string.h>

#include "desktop.h"
#include <paths.h>

#define DEFAULT_WALLPAPER _PATH_DDE_RES "/Backgrounds/default.png"
#define FALLBACK_COLOR    GFX_RGB(0x2B, 0x2F, 0x3A)

struct gfx_surface *make_wallpaper(struct desktop *d) {
    int w = d->screen.width, h = d->screen.height;
    struct gfx_surface *s = gfx_surface_create(w, h);
    if (!s) {
        return NULL;
    }
    const char *error = NULL;
    struct gfx_surface *img = image_load(d->cfg.wallpaper, &error);
    if (!img && strcmp(d->cfg.wallpaper, DEFAULT_WALLPAPER) != 0) {
        dprintf(STDERR_FILENO, "desktop: wallpaper %s: %s\n", d->cfg.wallpaper, error ? error : "?");
        img = image_load(DEFAULT_WALLPAPER, &error);
    }
    if (!img) {
        dprintf(STDERR_FILENO, "desktop: wallpaper %s: %s\n", DEFAULT_WALLPAPER, error ? error : "?");
        gfx_clear(s, FALLBACK_COLOR);   /* no picture: a plain, calm colour */
        return s;
    }

    /* Cover: scale so the picture fills both directions, crop the overflow evenly. */
    float sx = (float)w / img->width, sy = (float)h / img->height;
    float scale = sx > sy ? sx : sy;
    int crop_w = (int)(w / scale + 0.5f), crop_h = (int)(h / scale + 0.5f);
    crop_w = crop_w > img->width ? img->width : crop_w;
    crop_h = crop_h > img->height ? img->height : crop_h;
    struct gfx_rect from = gfx_rect((img->width - crop_w) / 2, (img->height - crop_h) / 2, crop_w, crop_h);
    gfx_clear(s, GFX_RGB(0, 0, 0));
    gfx_draw_scaled(s, gfx_rect(0, 0, w, h), img, &from);
    gfx_surface_destroy(img);
    return s;
}
