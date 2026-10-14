/*
 * The scanline rasterizer behind polygons, circles, lines and text.
 *
 * Each line segment adds, to every cell it crosses, how much of that cell
 * lies to the right of the line (signed by direction). Summing the cells of
 * a row from left to right then gives the exact covered area of each pixel,
 * which is the antialiased coverage. Curves are flattened into short lines.
 * (The same approach as the font-rs and stb_truetype renderers.)
 */

#include <gfx.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

bool gfx_raster_init(struct gfx_raster *r, int width, int height) {
    r->width = width;
    r->height = height;
    /* Two spare cells per row: a line touching the right edge writes just
       past it, and those cells only matter for pixels further right. */
    r->stride = width + 2;
    r->cells = calloc((size_t)r->stride * (size_t)height, sizeof(float));
    return r->cells != NULL;
}

void gfx_raster_free(struct gfx_raster *r) {
    free(r->cells);
    r->cells = NULL;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void gfx_raster_line(struct gfx_raster *r, float x0, float y0, float x1, float y1) {
    if (y0 == y1) {
        return;   /* horizontal lines cover no area */
    }
    float dir = 1;
    if (y0 > y1) {
        dir = -1;
        float t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    x0 = clampf(x0, 0, (float)r->width);
    x1 = clampf(x1, 0, (float)r->width);
    float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    if (y0 < 0) {
        x -= y0 * dxdy;
    }
    int y_start = y0 < 0 ? 0 : (int)y0;
    int y_end = (int)ceilf(y1);
    if (y_end > r->height) {
        y_end = r->height;
    }

    for (int y = y_start; y < y_end; y++) {
        float *row = r->cells + (size_t)y * (size_t)r->stride;
        float dy = fminf((float)(y + 1), y1) - fmaxf((float)y, y0);
        float x_next = x + dxdy * dy;
        float d = dy * dir;
        float xa = x < x_next ? x : x_next;
        float xb = x < x_next ? x_next : x;
        float xa_floor = floorf(xa);
        int xa_i = (int)xa_floor;
        float xb_ceil = ceilf(xb);
        int xb_i = (int)xb_ceil;

        if (xb_i <= xa_i + 1) {
            /* Stays within one pixel column on this row. */
            float xm = 0.5f * (x + x_next) - xa_floor;
            row[xa_i] += d - d * xm;
            row[xa_i + 1] += d * xm;
        } else {
            float s = 1.0f / (xb - xa);
            float xa_f = xa - xa_floor;
            float a0 = 0.5f * s * (1 - xa_f) * (1 - xa_f);
            float xb_f = xb - xb_ceil + 1;
            float am = 0.5f * s * xb_f * xb_f;
            row[xa_i] += d * a0;
            if (xb_i == xa_i + 2) {
                row[xa_i + 1] += d * (1 - a0 - am);
            } else {
                float a1 = s * (1.5f - xa_f);
                row[xa_i + 1] += d * (a1 - a0);
                for (int xi = xa_i + 2; xi < xb_i - 1; xi++) {
                    row[xi] += d * s;
                }
                float a2 = a1 + (float)(xb_i - xa_i - 3) * s;
                row[xb_i - 1] += d * (1 - a2 - am);
            }
            row[xb_i] += d * am;
        }
        x = x_next;
    }
}

void gfx_raster_quad(struct gfx_raster *r, float x0, float y0, float cx, float cy,
                     float x1, float y1) {
    /* Enough segments that the flattened curve strays well under a pixel. */
    float devx = x0 - 2 * cx + x1, devy = y0 - 2 * cy + y1;
    float devsq = devx * devx + devy * devy;
    if (devsq < 0.333f) {
        gfx_raster_line(r, x0, y0, x1, y1);
        return;
    }
    int n = 1 + (int)floorf(sqrtf(sqrtf(3.0f * devsq)));
    float px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n;
        float mt = 1 - t;
        float qx = mt * mt * x0 + 2 * mt * t * cx + t * t * x1;
        float qy = mt * mt * y0 + 2 * mt * t * cy + t * t * y1;
        gfx_raster_line(r, px, py, qx, qy);
        px = qx;
        py = qy;
    }
}

void gfx_raster_coverage(const struct gfx_raster *r, uint8_t *out) {
    for (int y = 0; y < r->height; y++) {
        const float *row = r->cells + (size_t)y * (size_t)r->stride;
        uint8_t *dst = out + (size_t)y * (size_t)r->width;
        float acc = 0;   /* each row starts outside the shape */
        for (int x = 0; x < r->width; x++) {
            acc += row[x];
            float c = fabsf(acc);
            dst[x] = c >= 1 ? 255 : (uint8_t)(c * 255 + 0.5f);
        }
    }
}

/* --- shapes built on the rasterizer ------------------------------------------------- */

/* Rasterize a closed polygon (already in surface coordinates) and paint it. */
void gfx_fill_polygon(struct gfx_surface *s, const float *pts, int count, gfx_color color) {
    if (count < 3) {
        return;
    }
    float minx = pts[0], maxx = pts[0], miny = pts[1], maxy = pts[1];
    for (int i = 1; i < count; i++) {
        minx = fminf(minx, pts[2 * i]);
        maxx = fmaxf(maxx, pts[2 * i]);
        miny = fminf(miny, pts[2 * i + 1]);
        maxy = fmaxf(maxy, pts[2 * i + 1]);
    }
    /* Only rasterize the part that can be seen. */
    int x0 = (int)floorf(minx), y0 = (int)floorf(miny);
    int x1 = (int)ceilf(maxx) + 1, y1 = (int)ceilf(maxy) + 1;
    struct gfx_rect box = gfx_rect_intersect((struct gfx_rect){ x0, y0, x1 - x0, y1 - y0 }, s->clip);
    if (gfx_rect_empty(box)) {
        return;
    }
    x0 = box.x;
    y0 = box.y;

    struct gfx_raster r;
    struct gfx_alpha mask = { NULL, box.w, box.h };
    if (!gfx_raster_init(&r, box.w, box.h) || !(mask.data = malloc((size_t)box.w * box.h))) {
        gfx_raster_free(&r);
        return;
    }
    for (int i = 0; i < count; i++) {
        int j = (i + 1) % count;
        gfx_raster_line(&r, pts[2 * i] - x0, pts[2 * i + 1] - y0, pts[2 * j] - x0, pts[2 * j + 1] - y0);
    }
    gfx_raster_coverage(&r, mask.data);
    gfx_fill_mask(s, x0, y0, &mask, color);
    gfx_raster_free(&r);
    free(mask.data);
}

void gfx_fill_circle(struct gfx_surface *s, float cx, float cy, float radius, gfx_color color) {
    enum { MAX_POINTS = 128 };
    float pts[2 * MAX_POINTS];
    int n = (int)(radius * 2.0f) + 12;   /* segments shorter than ~half a pixel */
    if (n > MAX_POINTS) {
        n = MAX_POINTS;
    }
    for (int i = 0; i < n; i++) {
        double a = 2 * M_PI * i / n;
        pts[2 * i] = cx + radius * (float)cos(a);
        pts[2 * i + 1] = cy + radius * (float)sin(a);
    }
    gfx_fill_polygon(s, pts, n, color);
}

void gfx_draw_line(struct gfx_surface *s, float x0, float y0, float x1, float y1,
                   float width, gfx_color color) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) {
        return;
    }
    /* A thin rectangle around the line. */
    float nx = -dy / len * width * 0.5f, ny = dx / len * width * 0.5f;
    float pts[8] = { x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny };
    gfx_fill_polygon(s, pts, 4, color);
}
