/* Surfaces and rectangle arithmetic. */

#include <gfx.h>
#include <stdlib.h>

bool gfx_rect_empty(struct gfx_rect r) {
    return r.w <= 0 || r.h <= 0;
}

bool gfx_rect_contains(struct gfx_rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

struct gfx_rect gfx_rect_intersect(struct gfx_rect a, struct gfx_rect b) {
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (x1 <= x0 || y1 <= y0) {
        return (struct gfx_rect){ 0, 0, 0, 0 };
    }
    return (struct gfx_rect){ x0, y0, x1 - x0, y1 - y0 };
}

struct gfx_rect gfx_rect_union(struct gfx_rect a, struct gfx_rect b) {
    if (gfx_rect_empty(a)) {
        return b;
    }
    if (gfx_rect_empty(b)) {
        return a;
    }
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (struct gfx_rect){ x0, y0, x1 - x0, y1 - y0 };
}

struct gfx_rect gfx_rect_inset(struct gfx_rect r, int by) {
    return (struct gfx_rect){ r.x + by, r.y + by, r.w - 2 * by, r.h - 2 * by };
}

struct gfx_surface *gfx_surface_create(int width, int height) {
    if (width <= 0 || height <= 0) {
        return NULL;
    }
    struct gfx_surface *s = malloc(sizeof(*s));
    uint32_t *pixels = calloc((size_t)width * (size_t)height, sizeof(uint32_t));
    if (!s || !pixels) {
        free(s);
        free(pixels);
        return NULL;
    }
    *s = (struct gfx_surface){
        .pixels = pixels,
        .width = width,
        .height = height,
        .stride = width,
        .clip = { 0, 0, width, height },
        .owned = true,
    };
    return s;
}

void gfx_surface_destroy(struct gfx_surface *s) {
    if (!s) {
        return;
    }
    if (s->owned) {
        free(s->pixels);
    }
    free(s);
}

struct gfx_surface gfx_surface_view(struct gfx_surface *parent, struct gfx_rect area) {
    area = gfx_rect_intersect(area, (struct gfx_rect){ 0, 0, parent->width, parent->height });
    return (struct gfx_surface){
        .pixels = gfx_row(parent, area.y) + area.x,
        .width = area.w,
        .height = area.h,
        .stride = parent->stride,
        .clip = { 0, 0, area.w, area.h },
        .owned = false,
    };
}

void gfx_set_clip(struct gfx_surface *s, struct gfx_rect clip) {
    s->clip = gfx_rect_intersect(clip, (struct gfx_rect){ 0, 0, s->width, s->height });
}

void gfx_reset_clip(struct gfx_surface *s) {
    s->clip = (struct gfx_rect){ 0, 0, s->width, s->height };
}
