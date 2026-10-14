/*
 * Saving pictures: PNG through stb_image_write (vendor/stb_image_write.h, v1.16, public domain /
 * MIT, by Sean Barrett: unchanged), built without stdio, writing through a callback to our file.
 */

#include <image.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STBI_WRITE_NO_STDIO
#define STBIW_ASSERT(x) ((void)0)
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma GCC diagnostic ignored "-Wunused-function"   /* (what we don't use of it; reported at the end) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "vendor/stb_image_write.h"
#pragma GCC diagnostic pop

struct sink {
    int fd;
    bool failed;
};

static void write_out(void *context, void *data, int size) {
    struct sink *s = context;
    if (!s->failed && write(s->fd, data, (size_t)size) != size) {
        s->failed = true;
    }
}

int image_save_png(const char *path, const struct gfx_surface *s) {
    if (!s || s->width <= 0 || s->height <= 0) {
        return -EINVAL;
    }
    /* Premultiplied ARGB -> straight RGBA. */
    uint8_t *rgba = malloc((size_t)s->width * (size_t)s->height * 4);
    if (!rgba) {
        return -ENOMEM;
    }
    bool opaque = true;
    for (int y = 0; y < s->height; y++) {
        const uint32_t *row = gfx_row((struct gfx_surface *)s, y);
        uint8_t *out = rgba + (size_t)y * (size_t)s->width * 4;
        for (int x = 0; x < s->width; x++) {
            uint32_t p = row[x];
            uint32_t a = p >> 24, r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            if (a && a < 255) {
                r = r * 255 / a;
                g = g * 255 / a;
                b = b * 255 / a;
            }
            opaque &= a == 255;
            out[x * 4] = (uint8_t)r;
            out[x * 4 + 1] = (uint8_t)g;
            out[x * 4 + 2] = (uint8_t)b;
            out[x * 4 + 3] = (uint8_t)a;
        }
    }
    if (opaque) {
        /* No transparency: RGB, a quarter smaller. */
        for (size_t i = 0, n = (size_t)s->width * (size_t)s->height; i < n; i++) {
            memmove(rgba + i * 3, rgba + i * 4, 3);
        }
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        free(rgba);
        return fd;
    }
    struct sink sink = { fd, false };
    int channels = opaque ? 3 : 4;
    stbi_write_png_compression_level = 6;
    int ok = stbi_write_png_to_func(write_out, &sink, s->width, s->height, channels, rgba, s->width * channels);
    close(fd);
    free(rgba);
    if (!ok || sink.failed) {
        unlink(path);
        return -EIO;
    }
    return 0;
}
