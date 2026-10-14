/*
 * GIF (87a/89a): the first frame, placed on the logical screen, with its
 * transparent colour and interlacing. Animations show their first frame.
 */

#include "internal.h"

#define MAX_CODES 4096

/* LZW, as GIF does it: variable-width codes (up to 12 bits), LSB first. */
static bool lzw_decode(const uint8_t *data, size_t size, int min_size, uint8_t *out, size_t out_size) {
    if (min_size < 2 || min_size > 11) {
        return false;
    }
    static uint16_t prefix[MAX_CODES];
    static uint8_t suffix[MAX_CODES], stack[MAX_CODES + 1];
    int clear = 1 << min_size, eoi = clear + 1;
    int size_bits = min_size + 1, next = clear + 2, old = -1;
    uint8_t first = 0;
    uint32_t bits = 0;
    int nbits = 0;
    size_t pos = 0, written = 0;
    for (int i = 0; i < clear; i++) {
        prefix[i] = 0xFFFF;
        suffix[i] = (uint8_t)i;
    }
    while (written < out_size) {
        while (nbits < size_bits) {
            if (pos >= size) {
                return written > 0;   /* ran out: keep what we have */
            }
            bits |= (uint32_t)data[pos++] << nbits;
            nbits += 8;
        }
        int code = (int)(bits & ((1u << size_bits) - 1));
        bits >>= size_bits;
        nbits -= size_bits;
        if (code == clear) {
            size_bits = min_size + 1;
            next = clear + 2;
            old = -1;
            continue;
        }
        if (code == eoi) {
            break;
        }
        if (old < 0) {
            if (code >= clear) {
                return false;
            }
            out[written++] = (uint8_t)code;
            first = (uint8_t)code;
            old = code;
            continue;
        }
        int in = code, sp = 0;
        if (code >= next) {
            if (code > next) {
                return false;
            }
            stack[sp++] = first;   /* the KwKwK case */
            code = old;
        }
        while (code >= clear) {
            if (sp >= MAX_CODES) {
                return false;
            }
            stack[sp++] = suffix[code];
            code = prefix[code];
        }
        first = (uint8_t)code;
        stack[sp++] = first;
        while (sp && written < out_size) {
            out[written++] = stack[--sp];
        }
        if (next < MAX_CODES) {
            prefix[next] = (uint16_t)old;
            suffix[next] = first;
            next++;
            if (next == (1 << size_bits) && size_bits < 12) {
                size_bits++;
            }
        }
        old = in;
    }
    return true;
}

/* Sub-blocks (a length byte, then data, ending with 0) joined into one buffer. */
static uint8_t *read_sub_blocks(const uint8_t **p, const uint8_t *end, size_t *size) {
    size_t total = 0;
    const uint8_t *q = *p;
    while (q < end && *q) {
        total += *q;
        q += 1 + *q;
    }
    uint8_t *buf = malloc(total ? total : 1);
    if (!buf) {
        return NULL;
    }
    size_t n = 0;
    q = *p;
    while (q < end && *q) {
        size_t len = *q++;
        if (q + len > end) {
            len = (size_t)(end - q);
        }
        memcpy(buf + n, q, len);
        n += len;
        q += len;
    }
    *p = q < end ? q + 1 : end;
    *size = n;
    return buf;
}

static void skip_sub_blocks(const uint8_t **p, const uint8_t *end) {
    const uint8_t *q = *p;
    while (q < end && *q) {
        q += 1 + *q;
    }
    *p = q < end ? q + 1 : end;
}

static void read_palette(const uint8_t *p, int count, uint32_t *palette) {
    for (int i = 0; i < count; i++) {
        palette[i] = 0xFF000000u | (uint32_t)p[3 * i] << 16 | (uint32_t)p[3 * i + 1] << 8 | p[3 * i + 2];
    }
}

struct gfx_surface *image_decode_gif(const uint8_t *d, size_t n, const char **error) {
    if (n < 13) {
        return image_fail(error, "truncated GIF");
    }
    const uint8_t *p = d + 6, *end = d + n;
    int sw = image_le16(p), sh = image_le16(p + 2), flags = p[4];
    p += 7;
    uint32_t global[256] = { 0 };
    if (flags & 0x80) {
        int count = 2 << (flags & 7);
        if (p + 3 * count > end) {
            return image_fail(error, "truncated GIF");
        }
        read_palette(p, count, global);
        p += 3 * count;
    }
    int transparent = -1;

    while (p < end) {
        int kind = *p++;
        if (kind == 0x3B) {
            break;   /* trailer: no image at all */
        }
        if (kind == 0x21) {   /* extension */
            if (p >= end) {
                break;
            }
            int label = *p++;
            if (label == 0xF9 && p + 5 < end && p[0] >= 4) {
                transparent = p[1] & 1 ? p[4] : -1;   /* graphic control */
            }
            skip_sub_blocks(&p, end);
            continue;
        }
        if (kind != 0x2C || p + 9 > end) {
            break;
        }
        /* The first image. */
        int ix = image_le16(p), iy = image_le16(p + 2), iw = image_le16(p + 4), ih = image_le16(p + 6);
        int iflags = p[8];
        p += 9;
        uint32_t palette[256];
        memcpy(palette, global, sizeof(palette));
        if (iflags & 0x80) {
            int count = 2 << (iflags & 7);
            if (p + 3 * count > end) {
                return image_fail(error, "truncated GIF");
            }
            read_palette(p, count, palette);
            p += 3 * count;
        }
        if (transparent >= 0) {
            palette[transparent] = 0;
        }
        /* Some files have a screen smaller than the image: grow it. */
        if (sw < ix + iw) {
            sw = ix + iw;
        }
        if (sh < iy + ih) {
            sh = iy + ih;
        }
        if (!image_size_ok((uint32_t)sw, (uint32_t)sh) || !iw || !ih || p >= end) {
            return image_fail(error, "bad GIF size");
        }
        int min_size = *p++;
        size_t size;
        uint8_t *lzw = read_sub_blocks(&p, end, &size);
        uint8_t *indices = calloc((size_t)iw * ih, 1);
        struct gfx_surface *s = gfx_surface_create(sw, sh);   /* transparent where the frame isn't */
        if (!lzw || !indices || !s) {
            free(lzw);
            free(indices);
            gfx_surface_destroy(s);
            return image_fail(error, "not enough memory");
        }
        bool ok = lzw_decode(lzw, size, min_size, indices, (size_t)iw * ih);
        free(lzw);
        if (!ok) {
            free(indices);
            gfx_surface_destroy(s);
            return image_fail(error, "broken GIF data");
        }
        /* Interlaced rows come in four passes: every 8th from 0, every 8th from 4, every 4th from 2, every 2nd from 1. */
        static const int start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
        int row = 0;
        for (int pass = 0; pass < ((iflags & 0x40) ? 4 : 1); pass++) {
            int y0 = (iflags & 0x40) ? start[pass] : 0, dy = (iflags & 0x40) ? step[pass] : 1;
            for (int y = y0; y < ih; y += dy, row++) {
                uint32_t *out = gfx_row(s, iy + y) + ix;
                const uint8_t *in = indices + (size_t)row * iw;
                for (int x = 0; x < iw; x++) {
                    out[x] = palette[in[x]];
                }
            }
        }
        free(indices);
        return s;
    }
    return image_fail(error, "no picture in the GIF");
}
