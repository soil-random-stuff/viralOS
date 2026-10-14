/* Alpha masks, blurring, and the soft shadows built from them. */

#include <gfx.h>
#include <stdlib.h>
#include <string.h>

struct gfx_alpha *gfx_alpha_create(int width, int height) {
    if (width <= 0 || height <= 0) {
        return NULL;
    }
    struct gfx_alpha *a = malloc(sizeof(*a));
    uint8_t *data = calloc((size_t)width * (size_t)height, 1);
    if (!a || !data) {
        free(a);
        free(data);
        return NULL;
    }
    *a = (struct gfx_alpha){ data, width, height };
    return a;
}

void gfx_alpha_destroy(struct gfx_alpha *a) {
    if (a) {
        free(a->data);
        free(a);
    }
}

/*
 * One box blur pass over `count` values `step` apart: each output is the
 * average of the 2r+1 inputs around it, kept as a running sum so the cost
 * doesn't depend on r.
 */
static void box_pass(uint8_t *line, int count, int step, int r, uint8_t *tmp) {
    for (int i = 0; i < count; i++) {
        tmp[i] = line[i * step];
    }
    int window = 2 * r + 1;
    int sum = 0;
    for (int i = -r; i <= r; i++) {
        if (i >= 0 && i < count) {
            sum += tmp[i];
        }
    }
    for (int i = 0; i < count; i++) {
        line[i * step] = (uint8_t)(sum / window);
        int out = i - r, in = i + r + 1;
        if (out >= 0) {
            sum -= tmp[out];
        }
        if (in < count) {
            sum += tmp[in];
        }
    }
}

void gfx_alpha_blur(struct gfx_alpha *a, int radius) {
    if (radius < 1) {
        return;
    }
    int longest = a->width > a->height ? a->width : a->height;
    uint8_t *tmp = malloc((size_t)longest);
    if (!tmp) {
        return;
    }
    /* Three box blurs of radius r/3 come out very close to a Gaussian. */
    int r = radius / 3 > 0 ? radius / 3 : 1;
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < a->height; y++) {
            box_pass(a->data + (size_t)y * a->width, a->width, 1, r, tmp);
        }
        for (int x = 0; x < a->width; x++) {
            box_pass(a->data + x, a->height, a->width, r, tmp);
        }
    }
    free(tmp);
}

struct gfx_alpha *gfx_shadow_create(int w, int h, float corner_radius, int blur) {
    struct gfx_alpha *a = gfx_alpha_create(w + 2 * blur, h + 2 * blur);
    if (!a) {
        return NULL;
    }
    /* Draw the shape as a white rounded rect into a temporary surface, keep its alpha. */
    struct gfx_surface tmp = {
        .pixels = calloc((size_t)a->width * a->height, sizeof(uint32_t)),
        .width = a->width,
        .height = a->height,
        .stride = a->width,
        .clip = { 0, 0, a->width, a->height },
    };
    if (!tmp.pixels) {
        gfx_alpha_destroy(a);
        return NULL;
    }
    gfx_fill_rounded_rect(&tmp, (struct gfx_rect){ blur, blur, w, h }, corner_radius,
                          GFX_RGB(255, 255, 255));
    for (size_t i = 0; i < (size_t)a->width * a->height; i++) {
        a->data[i] = (uint8_t)(tmp.pixels[i] >> 24);
    }
    free(tmp.pixels);
    gfx_alpha_blur(a, blur);
    return a;
}
