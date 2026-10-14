/* Loading a picture of any supported format: recognising it and handing it to its decoder. */

#include <unistd.h>

#include "internal.h"

uint8_t *image_read_file(const char *path, size_t *size, const char **error) {
    struct os_file_info info;
    if (stat(path, &info) < 0 || info.type != OS_FILE) {
        image_fail(error, "file not found");
        return NULL;
    }
    int fd = open(path, O_RDONLY);
    uint8_t *data = malloc(info.size ? info.size : 1);
    if (fd < 0 || !data) {
        if (fd >= 0) {
            close(fd);
        }
        free(data);
        image_fail(error, data ? "cannot read the file" : "not enough memory");
        return NULL;
    }
    uint64_t got = 0;
    while (got < info.size) {
        long n = read(fd, data + got, info.size - got);
        if (n <= 0) {
            break;
        }
        got += (uint64_t)n;
    }
    close(fd);
    if (got != info.size) {
        free(data);
        image_fail(error, "cannot read the file");
        return NULL;
    }
    *size = info.size;
    return data;
}

struct gfx_surface *image_orient(struct gfx_surface *s, int orientation) {
    if (orientation < 2 || orientation > 8) {
        return s;
    }
    int w = s->width, h = s->height;
    bool swap = orientation >= 5;   /* 5-8 turn it on its side */
    struct gfx_surface *out = gfx_surface_create(swap ? h : w, swap ? w : h);
    if (!out) {
        return s;   /* not enough memory: sideways is better than nothing */
    }
    for (int y = 0; y < out->height; y++) {
        uint32_t *row = gfx_row(out, y);
        for (int x = 0; x < out->width; x++) {
            int sx, sy;
            switch (orientation) {
            case 2:  sx = w - 1 - x; sy = y; break;              /* mirrored */
            case 3:  sx = w - 1 - x; sy = h - 1 - y; break;      /* upside down */
            case 4:  sx = x; sy = h - 1 - y; break;              /* flipped */
            case 5:  sx = y; sy = x; break;                      /* transposed */
            case 6:  sx = y; sy = h - 1 - x; break;              /* turned 90 degrees clockwise */
            case 7:  sx = w - 1 - y; sy = h - 1 - x; break;      /* transverse */
            default: sx = w - 1 - y; sy = x; break;              /* 8: turned 90 counter-clockwise */
            }
            row[x] = gfx_row(s, sy)[sx];
        }
    }
    gfx_surface_destroy(s);
    return out;
}

const char *image_format(const uint8_t *d, size_t n) {
    if (n >= 8 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) {
        return "PNG";
    }
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) {
        return "JPEG";
    }
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) {
        return "WebP";
    }
    if (n >= 6 && (memcmp(d, "GIF87a", 6) == 0 || memcmp(d, "GIF89a", 6) == 0)) {
        return "GIF";
    }
    if (n >= 14 && d[0] == 'B' && d[1] == 'M') {
        return "BMP";
    }
    if (n >= 4 && ((d[0] == 'I' && d[1] == 'I' && d[2] == 42 && d[3] == 0) ||
                   (d[0] == 'M' && d[1] == 'M' && d[2] == 0 && d[3] == 42))) {
        return "TIFF";
    }
    return NULL;
}

struct gfx_surface *image_decode(const uint8_t *data, size_t size, const char **error) {
    const char *format = image_format(data, size);
    if (!format) {
        return image_fail(error, "not a picture format we know");
    }
    switch (format[0]) {
    case 'P': return image_decode_png(data, size, error);
    case 'J': return image_decode_jpeg(data, size, error);
    case 'W': return image_decode_webp(data, size, error);
    case 'G': return image_decode_gif(data, size, error);
    case 'B': return image_decode_bmp(data, size, error);
    default:  return image_decode_tiff(data, size, error);
    }
}

struct gfx_surface *image_load(const char *path, const char **error) {
    size_t size;
    uint8_t *data = image_read_file(path, &size, error);
    if (!data) {
        return NULL;
    }
    struct gfx_surface *s = image_decode(data, size, error);
    free(data);
    return s;
}

static int lower(int c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

bool image_supported_name(const char *name) {
    static const char *const extensions[] = {
        "png", "jpg", "jpeg", "jpe", "jfif", "webp", "gif", "bmp", "dib", "tif", "tiff",
    };
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        const char *a = dot + 1, *b = extensions[i];
        while (*a && lower(*a) == *b) {
            a++;
            b++;
        }
        if (!*a && !*b) {
            return true;
        }
    }
    return false;
}
