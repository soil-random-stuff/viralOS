/*
 * The volume control in the panel's system tray: a speaker icon left of
 * the clock (its look follows the level: VolumeMute/Low/Medium/High.png in
 * /usr/share/dde-res/Icons/Status). Clicking it opens a card above the panel:
 *
 *   Speakers (HD Audio)                v      <- the output; click to choose another
 *   ------------------------------------
 *   [speaker]  [=========---------]  50%      <- click the speaker to mute
 *
 * The wheel over the icon or the card changes the level. Everything goes
 * through audio_mixer() (SYS_AUDIO_MIXER): the kernel holds the state, so
 * it applies to every program's sound.
 */

#include <image.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "desktop.h"
#include <paths.h>

#define TRAY_ICON     22
#define CARD_ICON     34
#define CARD_W        340
#define HEAD_H        52
#define ROW_H         36      /* an output in the list */
#define BODY_H        78
#define GAP           10      /* between the card and the panel */
#define SLIDER_X      66
#define SLIDER_RIGHT  70      /* room for the percentage */
#define SLIDER_H      18

#define INK           GFX_RGB(0x1C, 0x1C, 0x22)
#define INK_SOFT      GFX_RGB(0x6A, 0x6A, 0x74)
#define LINE          GFX_RGB(0xD4, 0xD4, 0xDA)
#define HOVER_FILL    GFX_RGBA(0, 0, 0, 14)
#define TRACK         GFX_RGB(0xE6, 0xE6, 0xEA)

enum { V_NONE = -1, V_HEAD = -2, V_MUTE = -3, V_SLIDER = -4 };   /* >= 0: an output in the list */

static const char *const level_names[] = { "Mute", "Low", "Medium", "High" };

static int level(const struct desktop *d) {
    const struct os_audio_mixer *m = &d->mixer;
    if (!d->mixer_ok || m->muted || m->volume == 0) {
        return 0;
    }
    return m->volume < 34 ? 1 : m->volume < 67 ? 2 : 3;
}

static struct gfx_surface *load_icon(const char *path, int size) {
    struct gfx_surface *img = image_load(path, NULL);
    if (!img) {
        return NULL;
    }
    struct gfx_surface *s = gfx_surface_scaled(img, size, size);
    gfx_surface_destroy(img);
    return s;
}

void volume_init(struct desktop *d) {
    for (int i = 0; i < 4; i++) {
        char path[96];
        snprintf(path, sizeof(path), _PATH_DDE_RES "/Icons/Status/Volume%s.png", level_names[i]);
        d->volume_tray_icons[i] = load_icon(path, TRAY_ICON);
        d->volume_card_icons[i] = load_icon(path, CARD_ICON);
    }
    d->mixer_ok = audio_mixer(OS_MIXER_GET, 0, &d->mixer) == 0;
    d->volume_hover = V_NONE;
}

/* --- in the panel ------------------------------------------------------------------------------- */

/* Draw the tray icon into the panel surface, just left of the clock (whose rect is known by now). */
void volume_draw_tray(struct desktop *d, struct gfx_surface *panel, bool hovered, bool pressed) {
    int size = PANEL_HEIGHT - 12;
    d->volume_slot = gfx_rect(d->clock_rect.x - 4 - size, d->panel_rect.y + 6, size, size);
    struct gfx_rect r = gfx_rect(d->volume_slot.x - d->panel_rect.x, 6, size, size);
    if (pressed || d->volume_open) {
        gfx_fill_rounded_rect(panel, r, 10, GFX_RGBA(0, 0, 0, 30));
    } else if (hovered) {
        gfx_fill_rounded_rect(panel, r, 10, GFX_RGBA(0, 0, 0, 16));
    }
    struct gfx_surface *icon = d->volume_tray_icons[level(d)];
    if (icon) {
        gfx_blit(panel, r.x + (size - TRAY_ICON) / 2, r.y + (size - TRAY_ICON) / 2, icon, NULL);
    }
}

void volume_tooltip(const struct desktop *d, char *text, size_t size) {
    if (!d->mixer_ok) {
        strlcpy(text, "No sound card", size);
    } else if (d->mixer.muted) {
        strlcpy(text, "Volume: muted", size);
    } else {
        snprintf(text, size, "Volume: %u%%", d->mixer.volume);
    }
}

/* --- the card ----------------------------------------------------------------------------------------- */

static int list_rows(const struct desktop *d) {
    return d->volume_list_open && d->mixer_ok ? d->mixer.output_count : 0;
}

static struct gfx_rect slider_rect(const struct desktop *d) {
    int y = HEAD_H + 1 + list_rows(d) * ROW_H + (list_rows(d) ? 1 : 0);
    return gfx_rect(SLIDER_X, y + (BODY_H - SLIDER_H) / 2, CARD_W - SLIDER_X - SLIDER_RIGHT, SLIDER_H);
}

static struct gfx_rect mute_rect(const struct desktop *d) {
    struct gfx_rect s = slider_rect(d);
    return gfx_rect(14, s.y + SLIDER_H / 2 - 21, 42, 42);
}

static int item_at(const struct desktop *d, int x, int y) {
    if (!d->volume_open || !gfx_rect_contains(d->volume_rect, x, y)) {
        return V_NONE;
    }
    int lx = x - d->volume_rect.x, ly = y - d->volume_rect.y;
    if (ly < HEAD_H) {
        return V_HEAD;
    }
    if (ly < HEAD_H + 1 + list_rows(d) * ROW_H) {
        int i = (ly - HEAD_H - 1) / ROW_H;
        return i >= 0 && i < list_rows(d) ? i : V_NONE;
    }
    if (gfx_rect_contains(mute_rect(d), lx, ly)) {
        return V_MUTE;
    }
    struct gfx_rect s = gfx_rect_inset(slider_rect(d), -8);
    return gfx_rect_contains(s, lx, ly) ? V_SLIDER : V_NONE;
}

static void draw_chevron(struct gfx_surface *s, float cx, float cy, bool up, gfx_color ink) {
    float d = up ? -1 : 1;
    gfx_draw_line(s, cx - 8, cy - 4 * d, cx, cy + 4 * d, 1.8f, ink);
    gfx_draw_line(s, cx, cy + 4 * d, cx + 8, cy - 4 * d, 1.8f, ink);
}

static void render_card(struct desktop *d) {
    struct gfx_surface *s = d->volume_card;
    int w = s->width, h = s->height;
    gfx_clear(s, GFX_RGBA(0, 0, 0, 0));
    gfx_fill_rounded_rect(s, gfx_rect(0, 0, w, h), PANEL_RADIUS, GFX_RGB(255, 255, 255));
    gfx_stroke_rounded_rect(s, gfx_rect(0, 0, w, h), PANEL_RADIUS, 1, GFX_RGBA(0, 0, 0, 16));
    struct ttf_font *font = d->regular, *bold = d->bold;
    const struct os_audio_mixer *m = &d->mixer;

    /* The output, with a chevron for the list. */
    if (d->volume_hover == V_HEAD && d->mixer_ok && m->output_count > 1) {
        gfx_set_clip(s, gfx_rect(0, 0, w, HEAD_H));
        gfx_fill_rounded_rect(s, gfx_rect(0, 0, w, HEAD_H + PANEL_RADIUS), PANEL_RADIUS, HOVER_FILL);
        gfx_reset_clip(s);
    }
    const char *name = !d->mixer_ok ? "No sound card" : m->output_count ? m->outputs[m->output] : "No outputs";
    if (font) {
        gfx_set_clip(s, gfx_rect(16, 0, w - 60, HEAD_H));
        ttf_draw_text(s, font, 16, 18, HEAD_H / 2 + 6, INK, name);
        gfx_reset_clip(s);
    }
    if (d->mixer_ok && m->output_count > 1) {
        draw_chevron(s, w - 30, HEAD_H / 2.0f, d->volume_list_open, INK);
    }
    gfx_fill_rect(s, gfx_rect(0, HEAD_H, w, 1), LINE);

    /* The outputs to choose from. */
    for (int i = 0; i < list_rows(d); i++) {
        struct gfx_rect row = gfx_rect(0, HEAD_H + 1 + i * ROW_H, w, ROW_H);
        if (d->volume_hover == i) {
            gfx_fill_rect(s, row, HOVER_FILL);
        }
        if (i == m->output) {
            gfx_fill_rounded_rect(s, gfx_rect(8, row.y + 9, 4, ROW_H - 18), 2, ACCENT);
        }
        if (font) {
            ttf_draw_text(s, font, 14, 24, row.y + ROW_H / 2 + 5, i == m->output ? INK : INK_SOFT, m->outputs[i]);
        }
    }
    if (list_rows(d)) {
        gfx_fill_rect(s, gfx_rect(0, HEAD_H + 1 + list_rows(d) * ROW_H, w, 1), LINE);
    }

    /* Mute button, slider, percentage. */
    struct gfx_rect mr = mute_rect(d);
    if (d->volume_hover == V_MUTE && d->mixer_ok) {
        gfx_fill_rounded_rect(s, mr, 10, HOVER_FILL);
    }
    struct gfx_surface *icon = d->volume_card_icons[level(d)];
    if (icon) {
        gfx_blit(s, mr.x + (mr.w - CARD_ICON) / 2, mr.y + (mr.h - CARD_ICON) / 2, icon, NULL);
    }
    struct gfx_rect sr = slider_rect(d);
    gfx_fill_rounded_rect(s, sr, 5, TRACK);
    int value = d->mixer_ok ? (int)m->volume : 0;
    int fill = (sr.w - 6) * value / 100;
    if (fill > 0) {
        /* Deep blue to a lighter one, left to right (dimmed while muted). */
        struct gfx_rect bar = gfx_rect(sr.x + 3, sr.y + 3, fill, sr.h - 6);
        gfx_set_clip(s, bar);
        for (int x = 0; x < bar.w; x++) {
            int t = (sr.w - 6) ? x * 255 / (sr.w - 6) : 0;
            int r = 0x00 + (0x1A - 0x00) * t / 255, g = 0x10 + (0x78 - 0x10) * t / 255;
            int b = 0xFF + (0xF0 - 0xFF) * t / 255;
            gfx_fill_rect(s, gfx_rect(bar.x + x, bar.y, 1, bar.h),
                          m->muted ? GFX_RGB(0xA8, 0xAE, 0xC8) : GFX_RGB(r, g, b));
        }
        gfx_reset_clip(s);
    }
    if (bold) {
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%", value);
        float tw = ttf_text_width(bold, 18, pct);
        ttf_draw_text(s, bold, 18, w - 16 - tw, sr.y + sr.h / 2 + 7, GFX_RGB(0, 0, 0), pct);
    }
    damage(d, d->volume_rect);
}

static struct gfx_rect card_bounds(const struct desktop *d) {
    struct gfx_rect b = gfx_rect_inset(d->volume_rect, -PANEL_SHADOW);
    b.h += 4;
    return b;
}

/* (Re)size the card for its contents, its right edge on the panel's. */
static bool layout_card(struct desktop *d) {
    int h = HEAD_H + 1 + list_rows(d) * ROW_H + (list_rows(d) ? 1 : 0) + BODY_H;
    if (d->volume_card && d->volume_card->height == h) {
        return true;
    }
    if (d->volume_open) {
        damage(d, card_bounds(d));
    }
    gfx_surface_destroy(d->volume_card);
    gfx_alpha_destroy(d->volume_shadow);
    d->volume_card = gfx_surface_create(CARD_W, h);
    d->volume_shadow = gfx_shadow_create(CARD_W, h, PANEL_RADIUS, PANEL_SHADOW);
    d->volume_rect = gfx_rect(d->panel_rect.x + d->panel_rect.w - CARD_W, d->panel_rect.y - GAP - h, CARD_W, h);
    return d->volume_card && d->volume_shadow;
}

void volume_close(struct desktop *d) {
    if (!d->volume_open) {
        return;
    }
    damage(d, card_bounds(d));
    gfx_surface_destroy(d->volume_card);
    gfx_alpha_destroy(d->volume_shadow);
    d->volume_card = NULL;
    d->volume_shadow = NULL;
    d->volume_open = false;
    d->volume_dragging = false;
    panel_redraw(d);
}

void volume_toggle(struct desktop *d) {
    if (d->volume_open) {
        volume_close(d);
        return;
    }
    launcher_close(d);
    network_close(d);
    d->mixer_ok = audio_mixer(OS_MIXER_GET, 0, &d->mixer) == 0;   /* fresh, in case a program changed it */
    d->volume_list_open = false;
    d->volume_hover = V_NONE;
    if (!layout_card(d)) {
        return;
    }
    d->volume_open = true;
    render_card(d);
    damage(d, card_bounds(d));
    panel_redraw(d);
}

bool volume_contains(const struct desktop *d, int x, int y) {
    return d->volume_open && gfx_rect_contains(d->volume_rect, x, y);
}

/* Something changed: tell the kernel, show it. */
static void apply(struct desktop *d, int op, long value) {
    if (!d->mixer_ok) {
        return;
    }
    audio_mixer(op, value, &d->mixer);
    if (d->volume_open) {
        render_card(d);
    }
    panel_redraw(d);
}

static void set_from_slider(struct desktop *d) {
    struct gfx_rect sr = slider_rect(d);
    int x = d->mouse_x - d->volume_rect.x - sr.x;
    int v = (x * 100 + sr.w / 2) / sr.w;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (d->mixer.muted) {
        apply(d, OS_MIXER_SET_MUTE, 0);   /* touching the slider unmutes, as most systems do */
    }
    if ((uint32_t)v != d->mixer.volume) {
        apply(d, OS_MIXER_SET_VOLUME, v);
    }
}

void volume_mouse_down(struct desktop *d) {
    int item = item_at(d, d->mouse_x, d->mouse_y);
    if (item == V_SLIDER && d->mixer_ok) {
        d->volume_dragging = true;
        set_from_slider(d);
    }
}

void volume_mouse_move(struct desktop *d) {
    if (d->volume_dragging) {
        set_from_slider(d);
        return;
    }
    if (!d->volume_open) {
        return;
    }
    int hover = item_at(d, d->mouse_x, d->mouse_y);
    if (hover != d->volume_hover) {
        d->volume_hover = hover;
        render_card(d);
    }
}

void volume_mouse_up(struct desktop *d) {
    if (d->volume_dragging) {
        d->volume_dragging = false;
        sounds_volume_changed(d);   /* what it sounds like now */
        return;
    }
    int item = item_at(d, d->mouse_x, d->mouse_y);
    if (!d->mixer_ok) {
        return;
    }
    if (item == V_MUTE) {
        apply(d, OS_MIXER_SET_MUTE, !d->mixer.muted);
        if (!d->mixer.muted) {
            sounds_volume_changed(d);
        }
    } else if (item == V_HEAD && d->mixer.output_count > 1) {
        d->volume_list_open = !d->volume_list_open;
        if (layout_card(d)) {
            render_card(d);
            damage(d, card_bounds(d));
        }
    } else if (item >= 0) {
        apply(d, OS_MIXER_SET_OUTPUT, item);
        d->volume_list_open = false;
        if (layout_card(d)) {
            render_card(d);
            damage(d, card_bounds(d));
        }
    }
}

bool volume_dragging(const struct desktop *d) {
    return d->volume_dragging;
}

bool volume_hover_clickable(const struct desktop *d) {
    return d->volume_open && d->volume_hover != V_NONE &&
           (d->volume_hover != V_HEAD || (d->mixer_ok && d->mixer.output_count > 1));
}

/* The wheel over the tray icon or the card: 5% a notch. */
bool volume_wheel(struct desktop *d, int wheel) {
    bool over = gfx_rect_contains(d->volume_slot, d->mouse_x, d->mouse_y) || volume_contains(d, d->mouse_x, d->mouse_y);
    if (!over || !d->mixer_ok || d->panel_hidden) {
        return false;
    }
    int v = (int)d->mixer.volume + (wheel < 0 ? 5 : -5);
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (d->mixer.muted) {
        apply(d, OS_MIXER_SET_MUTE, 0);
    }
    apply(d, OS_MIXER_SET_VOLUME, v);
    sounds_volume_changed(d);
    return true;
}

void volume_compose(struct desktop *d, struct gfx_rect area) {
    if (!d->volume_open || gfx_rect_empty(gfx_rect_intersect(card_bounds(d), area))) {
        return;
    }
    gfx_fill_mask(&d->screen, d->volume_rect.x - PANEL_SHADOW, d->volume_rect.y - PANEL_SHADOW + 4, d->volume_shadow,
                  GFX_RGBA(0, 0, 0, 80));
    gfx_blit(&d->screen, d->volume_rect.x, d->volume_rect.y, d->volume_card, NULL);
}
