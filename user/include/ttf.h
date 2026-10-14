#pragma once

/*
 * libttf: TrueType fonts.
 *
 * Reads .ttf files (glyf outlines: simple and composite glyphs, cmap
 * formats 4 and 12, horizontal metrics, and kerning from the 'kern'
 * table), and draws antialiased text with libgfx's rasterizer. Rendered
 * glyphs are cached per size. No hinting: glyphs are drawn as designed,
 * which looks right from about 11 px up.
 */

#include <stdbool.h>
#include <stdint.h>

#include <gfx.h>

struct ttf_font;

struct ttf_font *ttf_load(const char *path);   /* NULL if missing or not a TrueType font */
void ttf_free(struct ttf_font *font);

struct ttf_metrics {
    float ascent;       /* baseline to top of tall letters (positive) */
    float descent;      /* baseline to bottom of descenders (positive) */
    float line_height;  /* baseline to baseline */
};
void ttf_get_metrics(struct ttf_font *font, float size, struct ttf_metrics *out);

/* Draw UTF-8 text with its baseline at y; returns the x where the next character would go. */
float ttf_draw_text(struct gfx_surface *s, struct ttf_font *font, float size, float x, float y,
                    gfx_color color, const char *text);
float ttf_text_width(struct ttf_font *font, float size, const char *text);

/* Kerning (adjusting pairs like "AV") is on by default. */
void ttf_set_kerning(struct ttf_font *font, bool enabled);

/* For the curious: font-level information. */
const char *ttf_family_name(struct ttf_font *font);
uint32_t ttf_glyph_count(struct ttf_font *font);
uint32_t ttf_kerning_pairs(struct ttf_font *font);
