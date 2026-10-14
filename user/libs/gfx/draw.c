/* Filling, blending and copying pixels. */

#include <gfx.h>
#include <math.h>
#include <string.h>

/* The part of r that may be drawn: inside the clip rectangle. */
static struct gfx_rect visible(const struct gfx_surface *s, struct gfx_rect r) {
    return gfx_rect_intersect(r, s->clip);
}

void gfx_clear(struct gfx_surface *s, gfx_color color) {
    uint32_t p = gfx_premultiply(color, 255);
    for (int y = s->clip.y; y < s->clip.y + s->clip.h; y++) {
        uint32_t *row = gfx_row(s, y);
        for (int x = s->clip.x; x < s->clip.x + s->clip.w; x++) {
            row[x] = p;
        }
    }
}

static void fill_span(uint32_t *row, int x0, int x1, uint32_t p) {
    if ((p >> 24) == 255) {
        for (int x = x0; x < x1; x++) {
            row[x] = p;
        }
    } else if (p) {
        for (int x = x0; x < x1; x++) {
            row[x] = gfx_blend(row[x], p);
        }
    }
}

void gfx_fill_rect(struct gfx_surface *s, struct gfx_rect r, gfx_color color) {
    struct gfx_rect v = visible(s, r);
    uint32_t p = gfx_premultiply(color, 255);
    for (int y = v.y; y < v.y + v.h; y++) {
        fill_span(gfx_row(s, y), v.x, v.x + v.w, p);
    }
}

void gfx_fill_gradient(struct gfx_surface *s, struct gfx_rect r, gfx_color top, gfx_color bottom) {
    struct gfx_rect v = visible(s, r);
    for (int y = v.y; y < v.y + v.h; y++) {
        uint32_t t = r.h > 1 ? (uint32_t)((y - r.y) * 255 / (r.h - 1)) : 0;
        gfx_color c = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            uint32_t a = (top >> shift) & 0xFF, b = (bottom >> shift) & 0xFF;
            c |= ((a * (255 - t) + b * t + 127) / 255) << shift;
        }
        fill_span(gfx_row(s, y), v.x, v.x + v.w, gfx_premultiply(c, 255));
    }
}

/* --- rounded rectangles ----------------------------------------------------------------
 *
 * Coverage comes from the signed distance between the pixel's centre and
 * the rounded outline: fully inside -> 1, fully outside -> 0, and a smooth
 * ramp across the one-pixel band in between. Only pixels near a corner need
 * it; the rest of each row is filled outright.
 */

static float rounded_coverage(struct gfx_rect r, float radius, float px, float py) {
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    float qx = fabsf(px - cx) - (r.w * 0.5f - radius);
    float qy = fabsf(py - cy) - (r.h * 0.5f - radius);
    float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);
    float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - radius;
    float c = 0.5f - d;
    return c < 0 ? 0 : c > 1 ? 1 : c;
}

static float clamp_radius(struct gfx_rect r, float radius) {
    float max = (r.w < r.h ? r.w : r.h) * 0.5f;
    return radius < 0 ? 0 : radius > max ? max : radius;
}

/* Coverage of one rounded rect, minus an inner one if `inner` is set (for outlines). */
static void rounded_shape(struct gfx_surface *s, struct gfx_rect r, float radius,
                          const struct gfx_rect *inner, float inner_radius, gfx_color color) {
    struct gfx_rect v = visible(s, r);
    if (gfx_rect_empty(v)) {
        return;
    }
    radius = clamp_radius(r, radius);
    if (inner) {
        inner_radius = clamp_radius(*inner, inner_radius);
    }
    uint32_t full = gfx_premultiply(color, 255);
    int band = (int)ceilf(radius) + 1;
    int inner_band = inner ? (int)ceilf(inner_radius) + 1 : 0;

    for (int y = v.y; y < v.y + v.h; y++) {
        uint32_t *row = gfx_row(s, y);
        bool corner_row = y < r.y + band || y >= r.y + r.h - band;
        bool inner_row = inner && y >= inner->y && y < inner->y + inner->h;
        bool hollow_row = inner_row && y >= inner->y + inner_band &&
                          y < inner->y + inner->h - inner_band;
        for (int x = v.x; x < v.x + v.w; x++) {
            if (hollow_row && x > inner->x && x < inner->x + inner->w - 1) {
                x = inner->x + inner->w - 2;   /* nothing to draw inside an outline */
                continue;
            }
            bool corner_col = x < r.x + band || x >= r.x + r.w - band;
            if (!inner && !(corner_row && corner_col)) {
                /* Interior run: fill up to the right-hand corner band at once. */
                int end = v.x + v.w;
                if (corner_row && end > r.x + r.w - band) {
                    end = r.x + r.w - band;
                }
                fill_span(row, x, end, full);
                x = end - 1;
                continue;
            }
            float c = (corner_row && corner_col) || inner
                          ? rounded_coverage(r, radius, x + 0.5f, y + 0.5f) : 1.0f;
            if (inner_row && x >= inner->x && x < inner->x + inner->w) {
                c -= rounded_coverage(*inner, inner_radius, x + 0.5f, y + 0.5f);
            }
            if (c > 0.002f) {
                row[x] = gfx_blend(row[x], gfx_scale(full, (uint32_t)(c * 255 + 0.5f)));
            }
        }
    }
}

void gfx_fill_rounded_rect(struct gfx_surface *s, struct gfx_rect r, float radius, gfx_color color) {
    if (radius <= 0) {
        gfx_fill_rect(s, r, color);
        return;
    }
    rounded_shape(s, r, radius, NULL, 0, color);
}

void gfx_stroke_rounded_rect(struct gfx_surface *s, struct gfx_rect r, float radius,
                             float width, gfx_color color) {
    int w = (int)(width + 0.5f);
    struct gfx_rect inner = gfx_rect_inset(r, w > 0 ? w : 1);
    rounded_shape(s, r, radius, &inner, radius - width, color);
}

/* --- copying -------------------------------------------------------------------------------- */

/* Work out which part of src lands where on dst, after clipping both. */
static bool place(struct gfx_surface *dst, int *x, int *y, const struct gfx_surface *src,
                  const struct gfx_rect *from, struct gfx_rect *area) {
    struct gfx_rect a = from ? *from : (struct gfx_rect){ 0, 0, src->width, src->height };
    a = gfx_rect_intersect(a, (struct gfx_rect){ 0, 0, src->width, src->height });
    struct gfx_rect target = { *x, *y, a.w, a.h };
    struct gfx_rect v = visible(dst, target);
    if (gfx_rect_empty(v)) {
        return false;
    }
    area->x = a.x + (v.x - target.x);
    area->y = a.y + (v.y - target.y);
    area->w = v.w;
    area->h = v.h;
    *x = v.x;
    *y = v.y;
    return true;
}

void gfx_blit(struct gfx_surface *dst, int x, int y, const struct gfx_surface *src,
              const struct gfx_rect *from) {
    struct gfx_rect a;
    if (!place(dst, &x, &y, src, from, &a)) {
        return;
    }
    for (int row = 0; row < a.h; row++) {
        uint32_t *d = gfx_row(dst, y + row) + x;
        const uint32_t *sp = gfx_row(src, a.y + row) + a.x;
        for (int i = 0; i < a.w; i++) {
            uint32_t p = sp[i];
            if ((p >> 24) == 255) {
                d[i] = p;
            } else if (p) {
                d[i] = gfx_blend(d[i], p);
            }
        }
    }
}

void gfx_copy(struct gfx_surface *dst, int x, int y, const struct gfx_surface *src,
              const struct gfx_rect *from) {
    struct gfx_rect a;
    if (!place(dst, &x, &y, src, from, &a)) {
        return;
    }
    for (int row = 0; row < a.h; row++) {
        memcpy(gfx_row(dst, y + row) + x, gfx_row(src, a.y + row) + a.x,
               (size_t)a.w * sizeof(uint32_t));
    }
}

void gfx_fill_mask(struct gfx_surface *s, int x, int y, const struct gfx_alpha *mask,
                   gfx_color color) {
    struct gfx_rect v = visible(s, (struct gfx_rect){ x, y, mask->width, mask->height });
    uint32_t full = gfx_premultiply(color, 255);
    for (int row = v.y; row < v.y + v.h; row++) {
        uint32_t *d = gfx_row(s, row);
        const uint8_t *m = mask->data + (size_t)(row - y) * (size_t)mask->width - x;
        for (int col = v.x; col < v.x + v.w; col++) {
            uint32_t c = m[col];
            if (c == 255) {
                d[col] = gfx_blend(d[col], full);
            } else if (c) {
                d[col] = gfx_blend(d[col], gfx_scale(full, c));
            }
        }
    }
}
