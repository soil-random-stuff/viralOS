/*
 * BMP (Windows bitmap): BITMAPINFOHEADER and its later versions (and the
 * old OS/2 BITMAPCOREHEADER).
 *
 *   1, 4, 8 bits with a palette, uncompressed or RLE4/RLE8
 *   16, 24, 32 bits, with BI_BITFIELDS masks (alpha too, if there is one)
 *   bottom-up (the usual) or top-down rows
 */

#include "internal.h"

enum { BI_RGB = 0, BI_RLE8 = 1, BI_RLE4 = 2, BI_BITFIELDS = 3, BI_ALPHABITFIELDS = 6 };

struct mask {
    uint32_t mask;
    int shift, bits;
};

static struct mask make_mask(uint32_t m) {
    struct mask r = { m, 0, 0 };
    if (!m) {
        return r;
    }
    while (!(m & 1)) {
        m >>= 1;
        r.shift++;
    }
    while (m & 1) {
        m >>= 1;
        r.bits++;
    }
    return r;
}

/* A masked field scaled to 0-255. */
static uint32_t extract(uint32_t v, struct mask m) {
    if (!m.mask) {
        return 0;
    }
    uint32_t x = (v & m.mask) >> m.shift;
    if (m.bits >= 8) {
        return x >> (m.bits - 8);
    }
    return (x * 255 + ((1u << m.bits) - 1) / 2) / ((1u << m.bits) - 1);
}

/* RLE8/RLE4 into one palette index per pixel (rows bottom-up, as stored). */
static bool unpack_rle(const uint8_t *p, const uint8_t *end, uint8_t *out, int w, int h, bool rle4) {
    int x = 0, y = 0;
    while (p + 1 < end && y < h) {
        int count = p[0], value = p[1];
        p += 2;
        if (count) {
            for (int i = 0; i < count && x < w; i++, x++) {
                out[(size_t)y * w + x] = (uint8_t)(rle4 ? (i & 1 ? value & 15 : value >> 4) : value);
            }
            continue;
        }
        if (value == 0) {          /* end of line */
            x = 0;
            y++;
        } else if (value == 1) {   /* end of bitmap */
            break;
        } else if (value == 2) {   /* delta */
            if (p + 1 >= end) {
                return false;
            }
            x += p[0];
            y += p[1];
            p += 2;
        } else {                   /* a run of literal pixels */
            int bytes = rle4 ? (value + 1) / 2 : value;
            if (p + bytes > end) {
                return false;
            }
            for (int i = 0; i < value; i++, x++) {
                int v = rle4 ? (i & 1 ? p[i / 2] & 15 : p[i / 2] >> 4) : p[i];
                if (x < w) {
                    out[(size_t)y * w + x] = (uint8_t)v;
                }
            }
            p += (bytes + 1) & ~1;   /* padded to a 16-bit boundary */
        }
    }
    return true;
}

struct gfx_surface *image_decode_bmp(const uint8_t *d, size_t n, const char **error) {
    if (n < 26) {
        return image_fail(error, "truncated BMP");
    }
    uint32_t data_offset = image_le32(d + 10), header = image_le32(d + 14);
    int32_t w, h;
    int bpp, compression = BI_RGB;
    uint32_t colors = 0;
    struct mask masks[4] = { { 0, 0, 0 } };
    bool core = header == 12;
    if (core) {   /* OS/2 1.x */
        w = image_le16(d + 18);
        h = (int16_t)image_le16(d + 20);
        bpp = image_le16(d + 24);
    } else {
        if (header < 40 || 14 + header > n) {
            return image_fail(error, "unsupported BMP header");
        }
        w = (int32_t)image_le32(d + 18);
        h = (int32_t)image_le32(d + 22);
        bpp = image_le16(d + 28);
        compression = (int)image_le32(d + 30);
        colors = image_le32(d + 46);
        if (compression == BI_BITFIELDS || compression == BI_ALPHABITFIELDS) {
            /* The masks follow the 40-byte header (or are part of a bigger one). */
            if (14 + 40 + 12 > n) {
                return image_fail(error, "truncated BMP");
            }
            for (int i = 0; i < 3; i++) {
                masks[i] = make_mask(image_le32(d + 54 + 4 * i));
            }
            if ((compression == BI_ALPHABITFIELDS || header >= 56) && 14 + 40 + 16 <= n) {
                masks[3] = make_mask(image_le32(d + 66));
            }
        } else if (header >= 56 && bpp == 32 && compression == BI_RGB && 14 + 56 <= n) {
            /* V3+ headers may give an alpha mask even without BITFIELDS. */
            masks[3] = make_mask(image_le32(d + 66));
        }
    }
    bool top_down = h < 0;
    h = h < 0 ? -h : h;
    if (w <= 0 || !image_size_ok((uint32_t)w, (uint32_t)h)) {
        return image_fail(error, "bad BMP size");
    }
    if (bpp == 16 && compression == BI_RGB) {   /* 5-5-5 */
        masks[0] = make_mask(0x7C00);
        masks[1] = make_mask(0x03E0);
        masks[2] = make_mask(0x001F);
    } else if (bpp == 32 && compression == BI_RGB) {
        masks[0] = make_mask(0x00FF0000);
        masks[1] = make_mask(0x0000FF00);
        masks[2] = make_mask(0x000000FF);
    }

    /* The palette (BGR0 entries, or BGR for OS/2). */
    uint32_t palette[256];
    int entry = core ? 3 : 4;
    if (bpp <= 8) {
        uint32_t count = colors ? colors : 1u << bpp;
        count = count > 256 ? 256 : count;
        const uint8_t *p = d + 14 + header;
        for (uint32_t i = 0; i < 256; i++) {
            palette[i] = 0xFF000000u;
            if (i < count && p + entry * (i + 1) <= d + n) {
                palette[i] = 0xFF000000u | (uint32_t)p[entry * i + 2] << 16 | (uint32_t)p[entry * i + 1] << 8 |
                             p[entry * i];
            }
        }
    } else if (bpp != 16 && bpp != 24 && bpp != 32) {
        return image_fail(error, "unsupported BMP bit depth");
    }
    if (data_offset >= n) {
        return image_fail(error, "truncated BMP");
    }
    struct gfx_surface *s = gfx_surface_create(w, h);
    if (!s) {
        return image_fail(error, "not enough memory");
    }
    const uint8_t *pixels = d + data_offset, *end = d + n;

    if (compression == BI_RLE8 || compression == BI_RLE4) {
        uint8_t *idx = calloc((size_t)w * h, 1);
        if (!idx || !unpack_rle(pixels, end, idx, w, h, compression == BI_RLE4)) {
            free(idx);
            gfx_surface_destroy(s);
            return image_fail(error, idx ? "broken RLE data" : "not enough memory");
        }
        for (int y = 0; y < h; y++) {
            uint32_t *row = gfx_row(s, top_down ? y : h - 1 - y);
            for (int x = 0; x < w; x++) {
                row[x] = palette[idx[(size_t)y * w + x]];
            }
        }
        free(idx);
        return s;
    }

    size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
    /* An alpha mask whose alpha is zero everywhere means "no alpha" in practice. */
    bool alpha = false;
    if (masks[3].mask && bpp >= 16) {
        for (int y = 0; y < h && !alpha; y++) {
            const uint8_t *src = pixels + stride * y;
            for (int x = 0; x < w && src + stride <= end && !alpha; x++) {
                uint32_t v = bpp == 16 ? image_le16(src + 2 * x) : image_le32(src + 4 * x);
                alpha = extract(v, masks[3]) != 0;
            }
        }
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *src = pixels + stride * y;
        uint32_t *row = gfx_row(s, top_down ? y : h - 1 - y);
        if (src + stride > end) {
            break;   /* truncated: the rest stays transparent */
        }
        for (int x = 0; x < w; x++) {
            if (bpp <= 8) {
                int per = 8 / bpp, shift = 8 - bpp * (x % per + 1);
                int index = (src[x / per] >> shift) & ((1 << bpp) - 1);
                row[x] = palette[index];
            } else if (bpp == 24) {
                row[x] = 0xFF000000u | (uint32_t)src[3 * x + 2] << 16 | (uint32_t)src[3 * x + 1] << 8 | src[3 * x];
            } else {
                uint32_t v = bpp == 16 ? image_le16(src + 2 * x) : image_le32(src + 4 * x);
                uint32_t a = alpha ? extract(v, masks[3]) : 255;
                row[x] = image_pixel(extract(v, masks[0]), extract(v, masks[1]), extract(v, masks[2]), a);
            }
        }
    }
    return s;
}
