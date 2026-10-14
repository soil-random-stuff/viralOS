#pragma once

/* Shared by the decoders in libimage. */

#include <image.h>
#include <stdlib.h>
#include <string.h>

/* Largest picture we'll try: 16384 x 16384 and at most 64 megapixels. */
#define IMAGE_MAX_SIDE   16384
#define IMAGE_MAX_PIXELS (64u * 1024 * 1024)

static inline struct gfx_surface *image_fail(const char **error, const char *why) {
    if (error) {
        *error = why;
    }
    return NULL;
}

static inline bool image_size_ok(uint32_t w, uint32_t h) {
    return w && h && w <= IMAGE_MAX_SIDE && h <= IMAGE_MAX_SIDE && (uint64_t)w * h <= IMAGE_MAX_PIXELS;
}

/* Straight-alpha colour -> the premultiplied pixels surfaces hold. */
static inline uint32_t image_pixel(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (a == 255) {
        return 0xFF000000u | r << 16 | g << 8 | b;
    }
    return gfx_premultiply(a << 24 | r << 16 | g << 8 | b, 255);
}

static inline uint8_t image_clamp255(int v) {
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static inline uint16_t image_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static inline uint32_t image_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint16_t image_be16(const uint8_t *p) {
    return (uint16_t)(p[0] << 8 | p[1]);
}

static inline uint32_t image_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* image.c: turn a picture upright from an EXIF/TIFF orientation (1-8). Frees `s` if it makes a new one. */
struct gfx_surface *image_orient(struct gfx_surface *s, int orientation);

