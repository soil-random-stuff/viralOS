/*
 * PNG decoding.
 *
 * A PNG file is a signature followed by chunks: IHDR (size and pixel
 * format), PLTE (palette), tRNS (transparency), IDAT (the compressed
 * pixels, possibly split over several chunks) and IEND. Decompressed, the
 * pixels are rows, each prefixed by a filter type saying how the row was
 * predicted from its neighbours; undoing the filters gives the samples.
 * Interlaced images store seven smaller sub-images (Adam7) one after another.
 */

#include <image.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_DIMENSION 16384
#define MAX_PIXELS    (64u * 1024 * 1024)

enum color_type { GRAY = 0, RGB = 2, PALETTE = 3, GRAY_ALPHA = 4, RGBA = 6 };

struct png {
    uint32_t width, height;
    int depth, color, interlace;
    int channels;
    uint32_t palette[256];       /* 0xAARRGGBB */
    int palette_size;
    bool has_key;                /* tRNS for grey/RGB: this colour is transparent */
    uint16_t key[3];
};

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int channels_of(int color) {
    switch (color) {
    case GRAY:       return 1;
    case RGB:        return 3;
    case PALETTE:    return 1;
    case GRAY_ALPHA: return 2;
    case RGBA:       return 4;
    default:         return 0;
    }
}

static bool valid_depth(int color, int depth) {
    switch (color) {
    case GRAY:    return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case PALETTE: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    default:      return depth == 8 || depth == 16;
    }
}

/* Bytes in one filtered row of a (sub)image `w` pixels wide, without the filter byte. */
static size_t row_bytes(const struct png *p, uint32_t w) {
    return ((size_t)w * (size_t)p->channels * (size_t)p->depth + 7) / 8;
}

/* --- filters -------------------------------------------------------------------------- */

static uint8_t paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

/* Undo one row's filter in place. `prev` is the row above (already unfiltered) or NULL. */
static bool unfilter(uint8_t *row, const uint8_t *prev, size_t len, size_t bpp, int type) {
    for (size_t i = 0; i < len; i++) {
        int a = i >= bpp ? row[i - bpp] : 0;          /* left */
        int b = prev ? prev[i] : 0;                   /* above */
        int c = prev && i >= bpp ? prev[i - bpp] : 0; /* above-left */
        switch (type) {
        case 0: break;
        case 1: row[i] = (uint8_t)(row[i] + a); break;
        case 2: row[i] = (uint8_t)(row[i] + b); break;
        case 3: row[i] = (uint8_t)(row[i] + (a + b) / 2); break;
        case 4: row[i] = (uint8_t)(row[i] + paeth(a, b, c)); break;
        default: return false;
        }
    }
    return true;
}

/* --- samples to pixels ------------------------------------------------------------------- */

/* Sample number `index` of a row, at the image's bit depth (16-bit samples whole). */
static uint32_t sample(const struct png *p, const uint8_t *row, size_t index) {
    switch (p->depth) {
    case 16: return (uint32_t)row[2 * index] << 8 | row[2 * index + 1];
    case 8:  return row[index];
    default: {
        size_t bit = index * (size_t)p->depth;
        int shift = 8 - p->depth - (int)(bit % 8);
        return (row[bit / 8] >> shift) & ((1u << p->depth) - 1);
    }
    }
}

/* Scale a sample to 0..255. */
static uint32_t to8(const struct png *p, uint32_t v) {
    if (p->depth == 16) {
        return v >> 8;
    }
    return v * 255 / ((1u << p->depth) - 1);
}

static uint32_t pixel(const struct png *p, const uint8_t *row, uint32_t x) {
    size_t s = (size_t)x * (size_t)p->channels;
    uint32_t r, g, b, a = 255;
    switch (p->color) {
    case GRAY: {
        uint32_t v = sample(p, row, s);
        if (p->has_key && v == p->key[0]) {
            a = 0;
        }
        r = g = b = to8(p, v);
        break;
    }
    case GRAY_ALPHA:
        r = g = b = to8(p, sample(p, row, s));
        a = to8(p, sample(p, row, s + 1));
        break;
    case RGB: {
        uint32_t vr = sample(p, row, s), vg = sample(p, row, s + 1), vb = sample(p, row, s + 2);
        if (p->has_key && vr == p->key[0] && vg == p->key[1] && vb == p->key[2]) {
            a = 0;
        }
        r = to8(p, vr);
        g = to8(p, vg);
        b = to8(p, vb);
        break;
    }
    case RGBA:
        r = to8(p, sample(p, row, s));
        g = to8(p, sample(p, row, s + 1));
        b = to8(p, sample(p, row, s + 2));
        a = to8(p, sample(p, row, s + 3));
        break;
    default: {   /* palette */
        uint32_t i = sample(p, row, s);
        uint32_t c = i < (uint32_t)p->palette_size ? p->palette[i] : 0xFF000000u;
        return gfx_premultiply(c, 255);
    }
    }
    return gfx_premultiply(a << 24 | r << 16 | g << 8 | b, 255);
}

/* --- the image ------------------------------------------------------------------------------ */

static const uint8_t adam7_x0[7] = { 0, 4, 0, 2, 0, 1, 0 };
static const uint8_t adam7_y0[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const uint8_t adam7_dx[7] = { 8, 8, 4, 4, 2, 2, 1 };
static const uint8_t adam7_dy[7] = { 8, 8, 8, 4, 4, 2, 2 };

/* Size of one interlace pass (or the whole image, when not interlaced). */
static void pass_size(const struct png *p, int pass, uint32_t *w, uint32_t *h) {
    if (!p->interlace) {
        *w = p->width;
        *h = p->height;
        return;
    }
    *w = p->width > adam7_x0[pass] ? (p->width - adam7_x0[pass] + adam7_dx[pass] - 1) / adam7_dx[pass] : 0;
    *h = p->height > adam7_y0[pass] ? (p->height - adam7_y0[pass] + adam7_dy[pass] - 1) / adam7_dy[pass] : 0;
}

static size_t raw_size(const struct png *p) {
    size_t total = 0;
    for (int pass = 0; pass < (p->interlace ? 7 : 1); pass++) {
        uint32_t w, h;
        pass_size(p, pass, &w, &h);
        if (w && h) {
            total += (size_t)h * (1 + row_bytes(p, w));
        }
    }
    return total;
}

static bool unpack(const struct png *p, uint8_t *raw, struct gfx_surface *s) {
    size_t bpp = ((size_t)p->channels * (size_t)p->depth + 7) / 8;   /* filter unit, at least 1 */
    for (int pass = 0; pass < (p->interlace ? 7 : 1); pass++) {
        uint32_t w, h;
        pass_size(p, pass, &w, &h);
        if (!w || !h) {
            continue;
        }
        size_t len = row_bytes(p, w);
        const uint8_t *prev = NULL;
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *row = raw + 1;
            if (!unfilter(row, prev, len, bpp, raw[0])) {
                return false;
            }
            uint32_t dy = p->interlace ? adam7_y0[pass] + y * adam7_dy[pass] : y;
            uint32_t *out = gfx_row(s, (int)dy);
            for (uint32_t x = 0; x < w; x++) {
                uint32_t dx = p->interlace ? adam7_x0[pass] + x * adam7_dx[pass] : x;
                out[dx] = pixel(p, row, x);
            }
            prev = row;
            raw += 1 + len;
        }
    }
    return true;
}

static struct gfx_surface *fail(const char **error, const char *why) {
    if (error) {
        *error = why;
    }
    return NULL;
}

struct gfx_surface *image_decode_png(const uint8_t *data, size_t size, const char **error) {
    static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (size < 8 || memcmp(data, signature, 8) != 0) {
        return fail(error, "not a PNG file");
    }
    struct png p = { 0 };
    uint8_t *idat = NULL;
    size_t idat_len = 0, idat_cap = 0;
    bool have_header = false, done = false;

    for (size_t pos = 8; pos + 12 <= size && !done;) {
        uint32_t len = be32(data + pos);
        const uint8_t *type = data + pos + 4, *body = data + pos + 8;
        if (len > size - pos - 12) {
            free(idat);
            return fail(error, "truncated chunk");
        }
        if (memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            p.width = be32(body);
            p.height = be32(body + 4);
            p.depth = body[8];
            p.color = body[9];
            p.interlace = body[12];
            p.channels = channels_of(p.color);
            if (!p.channels || !valid_depth(p.color, p.depth) || body[10] || body[11] || p.interlace > 1) {
                return fail(error, "unsupported pixel format");
            }
            if (!p.width || !p.height || p.width > MAX_DIMENSION || p.height > MAX_DIMENSION ||
                (uint64_t)p.width * p.height > MAX_PIXELS) {
                return fail(error, "image too large");
            }
            have_header = true;
        } else if (memcmp(type, "PLTE", 4) == 0) {
            p.palette_size = (int)(len / 3 > 256 ? 256 : len / 3);
            for (int i = 0; i < p.palette_size; i++) {
                p.palette[i] = 0xFF000000u | (uint32_t)body[3 * i] << 16 |
                               (uint32_t)body[3 * i + 1] << 8 | body[3 * i + 2];
            }
        } else if (memcmp(type, "tRNS", 4) == 0) {
            if (p.color == PALETTE) {
                for (uint32_t i = 0; i < len && i < 256; i++) {
                    p.palette[i] = (p.palette[i] & 0x00FFFFFFu) | (uint32_t)body[i] << 24;
                }
            } else if (p.color == GRAY && len >= 2) {
                p.key[0] = (uint16_t)(body[0] << 8 | body[1]);
                p.has_key = true;
            } else if (p.color == RGB && len >= 6) {
                for (int i = 0; i < 3; i++) {
                    p.key[i] = (uint16_t)(body[2 * i] << 8 | body[2 * i + 1]);
                }
                p.has_key = true;
            }
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (idat_len + len > idat_cap) {
                size_t cap = idat_cap ? idat_cap * 2 : 65536;
                while (cap < idat_len + len) {
                    cap *= 2;
                }
                uint8_t *grown = realloc(idat, cap);
                if (!grown) {
                    free(idat);
                    return fail(error, "out of memory");
                }
                idat = grown;
                idat_cap = cap;
            }
            memcpy(idat + idat_len, body, len);
            idat_len += len;
        } else if (memcmp(type, "IEND", 4) == 0) {
            done = true;
        }
        /* Other chunks (text, colour profiles, ...) don't affect the pixels: skipped. */
        pos += 12 + (size_t)len;
    }

    if (!have_header || !idat) {
        free(idat);
        return fail(error, have_header ? "no image data" : "no header");
    }
    if (p.color == PALETTE && p.palette_size == 0) {
        free(idat);
        return fail(error, "missing palette");
    }
    size_t expected = raw_size(&p);
    uint8_t *raw = malloc(expected);
    struct gfx_surface *s = gfx_surface_create((int)p.width, (int)p.height);
    if (!raw || !s) {
        free(idat);
        free(raw);
        gfx_surface_destroy(s);
        return fail(error, "out of memory");
    }
    long got = zlib_decompress(idat, idat_len, raw, expected);
    free(idat);
    bool ok = got == (long)expected && unpack(&p, raw, s);
    free(raw);
    if (!ok) {
        gfx_surface_destroy(s);
        return fail(error, got != (long)expected ? "corrupt compressed data" : "bad row filter");
    }
    return s;
}

struct gfx_surface *image_load_png(const char *path, const char **error) {
    struct os_file_info info;
    if (stat(path, &info) < 0 || info.type != OS_FILE) {
        return fail(error, "file not found");
    }
    int fd = open(path, O_RDONLY);
    uint8_t *data = malloc(info.size ? info.size : 1);
    if (fd < 0 || !data) {
        if (fd >= 0) {
            close(fd);
        }
        free(data);
        return fail(error, "cannot read file");
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
    struct gfx_surface *s = got == info.size ? image_decode_png(data, info.size, error)
                                             : fail(error, "cannot read file");
    free(data);
    return s;
}
