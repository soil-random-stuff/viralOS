#pragma once

/*
 * libgfx: 2D drawing into memory.
 *
 * Surfaces hold premultiplied ARGB pixels (0xAARRGGBB with the color
 * channels already multiplied by alpha), which makes blending one
 * multiply-add per channel. Colors passed to drawing functions are plain,
 * not premultiplied: GFX_RGBA(255, 0, 0, 128) is half-transparent red.
 *
 * Everything is clipped to the surface's clip rectangle and antialiased
 * where edges aren't axis-aligned.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t gfx_color;

#define GFX_RGBA(r, g, b, a) \
    ((gfx_color)(((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define GFX_RGB(r, g, b) GFX_RGBA(r, g, b, 255)
#define GFX_ALPHA(color, a) (((color) & 0x00FFFFFFu) | ((uint32_t)(a) << 24))

struct gfx_rect {
    int x, y, w, h;
};

struct gfx_surface {
    uint32_t *pixels;        /* premultiplied ARGB */
    int width, height;
    int stride;              /* pixels from one row to the next */
    struct gfx_rect clip;    /* drawing only happens inside this */
    bool owned;              /* pixels belong to this surface (freed with it) */
};

/* An 8-bit coverage/alpha map: masks for text and shadows. */
struct gfx_alpha {
    uint8_t *data;
    int width, height;
};

/* --- rectangles ------------------------------------------------------------------ */

static inline struct gfx_rect gfx_rect(int x, int y, int w, int h) {
    return (struct gfx_rect){ x, y, w, h };
}
bool gfx_rect_empty(struct gfx_rect r);
bool gfx_rect_contains(struct gfx_rect r, int x, int y);
struct gfx_rect gfx_rect_intersect(struct gfx_rect a, struct gfx_rect b);
struct gfx_rect gfx_rect_union(struct gfx_rect a, struct gfx_rect b);
struct gfx_rect gfx_rect_inset(struct gfx_rect r, int by);   /* negative grows it */

/* --- surfaces --------------------------------------------------------------------- */

struct gfx_surface *gfx_surface_create(int width, int height);   /* transparent */
void gfx_surface_destroy(struct gfx_surface *s);
/* A surface sharing (part of) another's pixels. */
struct gfx_surface gfx_surface_view(struct gfx_surface *parent, struct gfx_rect area);
void gfx_set_clip(struct gfx_surface *s, struct gfx_rect clip);   /* kept inside the surface */
void gfx_reset_clip(struct gfx_surface *s);

/* --- drawing ----------------------------------------------------------------------- */

void gfx_clear(struct gfx_surface *s, gfx_color color);   /* replace, don't blend */
void gfx_fill_rect(struct gfx_surface *s, struct gfx_rect r, gfx_color color);
void gfx_fill_rounded_rect(struct gfx_surface *s, struct gfx_rect r, float radius, gfx_color color);
void gfx_stroke_rounded_rect(struct gfx_surface *s, struct gfx_rect r, float radius,
                             float width, gfx_color color);
void gfx_fill_gradient(struct gfx_surface *s, struct gfx_rect r, gfx_color top, gfx_color bottom);
void gfx_fill_circle(struct gfx_surface *s, float cx, float cy, float radius, gfx_color color);
void gfx_draw_line(struct gfx_surface *s, float x0, float y0, float x1, float y1,
                   float width, gfx_color color);
/* points: x0, y0, x1, y1, ... (closed automatically). Non-zero winding. */
void gfx_fill_polygon(struct gfx_surface *s, const float *points, int count, gfx_color color);

/* Paint `color` through a coverage mask whose top-left lands at (x, y). */
void gfx_fill_mask(struct gfx_surface *s, int x, int y, const struct gfx_alpha *mask,
                   gfx_color color);
/* Draw `src` (area `from`, or all of it if NULL) with its top-left at (x, y). */
void gfx_blit(struct gfx_surface *dst, int x, int y, const struct gfx_surface *src,
              const struct gfx_rect *from);           /* alpha-blended */
void gfx_copy(struct gfx_surface *dst, int x, int y, const struct gfx_surface *src,
              const struct gfx_rect *from);           /* plain copy, no blending */
/* Draw `src` (area `from`, or all of it) stretched or shrunk to fill `to`. */
void gfx_draw_scaled(struct gfx_surface *dst, struct gfx_rect to, const struct gfx_surface *src,
                     const struct gfx_rect *from);
struct gfx_surface *gfx_surface_scaled(const struct gfx_surface *src, int width, int height);

/* --- masks and shadows ----------------------------------------------------------- */

struct gfx_alpha *gfx_alpha_create(int width, int height);
void gfx_alpha_destroy(struct gfx_alpha *a);
/* Blur in place: three box blurs, which look almost exactly like a Gaussian. */
void gfx_alpha_blur(struct gfx_alpha *a, int radius);
/*
 * A soft shadow for a w x h rounded rectangle: a mask of
 * (w + 2*blur) x (h + 2*blur), to be drawn `blur` pixels up and left of
 * the shape.
 */
struct gfx_alpha *gfx_shadow_create(int w, int h, float corner_radius, int blur);

/* --- the rasterizer (shared with the font renderer) --------------------------------- */

/*
 * Accumulates the exact area covered by a shape in each pixel, for
 * smooth antialiased edges. Feed it lines and quadratic curves in pixel
 * coordinates (0..width, 0..height), then read out coverage 0..255.
 */
struct gfx_raster {
    int width, height;
    int stride;     /* width + 2: edges at x == width spill into spare cells */
    float *cells;
};

bool gfx_raster_init(struct gfx_raster *r, int width, int height);
void gfx_raster_free(struct gfx_raster *r);
void gfx_raster_line(struct gfx_raster *r, float x0, float y0, float x1, float y1);
void gfx_raster_quad(struct gfx_raster *r, float x0, float y0, float cx, float cy,
                     float x1, float y1);
void gfx_raster_coverage(const struct gfx_raster *r, uint8_t *out);   /* width * height bytes */

/* --- pixel helpers ------------------------------------------------------------------- */

static inline uint32_t gfx_div255(uint32_t v) {
    return (v + 1 + (v >> 8)) >> 8;   /* exact for v <= 255 * 255 */
}

/* A plain color at `coverage` (0..255), premultiplied. */
static inline uint32_t gfx_premultiply(gfx_color c, uint32_t coverage) {
    uint32_t a = gfx_div255((c >> 24) * coverage);
    uint32_t r = gfx_div255(((c >> 16) & 0xFF) * a);
    uint32_t g = gfx_div255(((c >> 8) & 0xFF) * a);
    uint32_t b = gfx_div255((c & 0xFF) * a);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* Premultiplied `src` over `dst`. */
static inline uint32_t gfx_blend(uint32_t dst, uint32_t src) {
    uint32_t inv = 255 - (src >> 24);
    if (inv == 0) {
        return src;
    }
    uint32_t rb = (dst & 0x00FF00FF) * inv;
    uint32_t ag = ((dst >> 8) & 0x00FF00FF) * inv;
    rb = ((rb + 0x00800080 + ((rb >> 8) & 0x00FF00FF)) >> 8) & 0x00FF00FF;
    ag = (ag + 0x00800080 + ((ag >> 8) & 0x00FF00FF)) & 0xFF00FF00;
    return src + (rb | ag);
}

/* Scale a premultiplied pixel by coverage 0..255. */
static inline uint32_t gfx_scale(uint32_t p, uint32_t coverage) {
    uint32_t rb = (p & 0x00FF00FF) * coverage;
    uint32_t ag = ((p >> 8) & 0x00FF00FF) * coverage;
    rb = ((rb + 0x00800080 + ((rb >> 8) & 0x00FF00FF)) >> 8) & 0x00FF00FF;
    ag = (ag + 0x00800080 + ((ag >> 8) & 0x00FF00FF)) & 0xFF00FF00;
    return rb | ag;
}

static inline uint32_t *gfx_row(const struct gfx_surface *s, int y) {
    return s->pixels + (size_t)y * (size_t)s->stride;
}
