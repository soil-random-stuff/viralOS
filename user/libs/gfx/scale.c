/*
 * Drawing a picture at a different size. Shrinking averages every source
 * pixel that falls into a destination pixel (no shimmering or jagged
 * edges); enlarging interpolates between the four nearest (bilinear).
 * Works on premultiplied pixels, so transparent edges blend correctly.
 */

#include <gfx.h>
#include <math.h>
#include <stdlib.h>

static uint32_t average(const struct gfx_surface *src, int x0, int y0, int x1, int y1) {
    uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
    for (int y = y0; y < y1; y++) {
        const uint32_t *row = gfx_row(src, y);
        for (int x = x0; x < x1; x++) {
            uint32_t p = row[x];
            a += p >> 24;
            r += (p >> 16) & 0xFF;
            g += (p >> 8) & 0xFF;
            b += p & 0xFF;
            n++;
        }
    }
    if (!n) {
        return 0;
    }
    return (a / n) << 24 | (r / n) << 16 | (g / n) << 8 | (b / n);
}

static uint32_t lerp(uint32_t p, uint32_t q, uint32_t t) {   /* t: 0..256 */
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        uint32_t a = (p >> shift) & 0xFF, b = (q >> shift) & 0xFF;
        out |= ((a * (256 - t) + b * t) >> 8) << shift;
    }
    return out;
}

static uint32_t bilinear(const struct gfx_surface *src, float fx, float fy) {
    fx = fmaxf(fx - 0.5f, 0);   /* pixel centres */
    fy = fmaxf(fy - 0.5f, 0);
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1 < src->width ? x0 + 1 : x0, y1 = y0 + 1 < src->height ? y0 + 1 : y0;
    if (x0 >= src->width) {
        x0 = x1 = src->width - 1;
    }
    if (y0 >= src->height) {
        y0 = y1 = src->height - 1;
    }
    uint32_t tx = (uint32_t)((fx - (float)x0) * 256), ty = (uint32_t)((fy - (float)y0) * 256);
    uint32_t top = lerp(gfx_row(src, y0)[x0], gfx_row(src, y0)[x1], tx);
    uint32_t bottom = lerp(gfx_row(src, y1)[x0], gfx_row(src, y1)[x1], tx);
    return lerp(top, bottom, ty);
}

void gfx_draw_scaled(struct gfx_surface *dst, struct gfx_rect to, const struct gfx_surface *src,
                     const struct gfx_rect *from_area) {
    struct gfx_rect from = from_area ? *from_area : gfx_rect(0, 0, src->width, src->height);
    from = gfx_rect_intersect(from, gfx_rect(0, 0, src->width, src->height));
    if (gfx_rect_empty(from) || gfx_rect_empty(to)) {
        return;
    }
    struct gfx_rect v = gfx_rect_intersect(to, dst->clip);
    float sx = (float)from.w / (float)to.w, sy = (float)from.h / (float)to.h;
    bool shrinking = sx >= 1 && sy >= 1;

    for (int y = v.y; y < v.y + v.h; y++) {
        uint32_t *out = gfx_row(dst, y);
        float fy0 = from.y + (y - to.y) * sy;
        int y0 = (int)fy0, y1 = (int)(fy0 + sy);
        if (y1 <= y0) {
            y1 = y0 + 1;
        }
        if (y1 > from.y + from.h) {
            y1 = from.y + from.h;
        }
        for (int x = v.x; x < v.x + v.w; x++) {
            float fx0 = from.x + (x - to.x) * sx;
            uint32_t p;
            if (shrinking) {
                int x0 = (int)fx0, x1 = (int)(fx0 + sx);
                if (x1 <= x0) {
                    x1 = x0 + 1;
                }
                if (x1 > from.x + from.w) {
                    x1 = from.x + from.w;
                }
                p = average(src, x0, y0, x1, y1);
            } else {
                p = bilinear(src, fx0 + sx * 0.5f, fy0 + sy * 0.5f);
            }
            out[x] = (p >> 24) == 255 ? p : gfx_blend(out[x], p);
        }
    }
}

struct gfx_surface *gfx_surface_scaled(const struct gfx_surface *src, int width, int height) {
    struct gfx_surface *s = gfx_surface_create(width, height);
    if (s) {
        gfx_draw_scaled(s, gfx_rect(0, 0, width, height), src, NULL);
    }
    return s;
}
