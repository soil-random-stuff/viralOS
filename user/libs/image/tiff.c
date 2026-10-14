/*
 * TIFF (6.0): the first page (IFD).
 *
 *   compression: none, PackBits, LZW, Deflate (old and new codes)
 *   grey (black or white is zero), RGB, palette, CMYK, with alpha
 *   1, 2, 4, 8 or 16 bits per sample, strips or tiles, chunky or planar
 *   the horizontal-differencing predictor, the orientation tag
 */

#include "internal.h"

enum {
    T_WIDTH = 256, T_HEIGHT = 257, T_BITS = 258, T_COMPRESSION = 259, T_PHOTOMETRIC = 262,
    T_STRIP_OFFSETS = 273, T_ORIENTATION = 274, T_SAMPLES = 277, T_ROWS_PER_STRIP = 278,
    T_STRIP_BYTES = 279, T_PLANAR = 284, T_PREDICTOR = 317, T_COLORMAP = 320, T_TILE_WIDTH = 322,
    T_TILE_LENGTH = 323, T_TILE_OFFSETS = 324, T_TILE_BYTES = 325, T_EXTRA = 338, T_SAMPLE_FORMAT = 339,
};

struct tiff {
    const uint8_t *d;
    size_t n;
    bool le;
};

static uint32_t u16(const struct tiff *t, size_t off) {
    return off + 2 <= t->n ? (t->le ? image_le16(t->d + off) : image_be16(t->d + off)) : 0;
}

static uint32_t u32(const struct tiff *t, size_t off) {
    return off + 4 <= t->n ? (t->le ? image_le32(t->d + off) : image_be32(t->d + off)) : 0;
}

/* One directory entry, with its values (inline or at an offset). */
struct entry {
    uint32_t tag, type, count;
    size_t at;   /* where the values are */
};

static int type_size(uint32_t type) {
    switch (type) {
    case 1: case 2: case 6: case 7: return 1;   /* BYTE ASCII SBYTE UNDEFINED */
    case 3: case 8:                 return 2;   /* SHORT SSHORT */
    case 4: case 9: case 11: case 13: return 4; /* LONG SLONG FLOAT IFD */
    default:                        return 8;   /* RATIONAL SRATIONAL DOUBLE ... */
    }
}

static uint32_t value(const struct tiff *t, const struct entry *e, uint32_t i) {
    if (!e || i >= e->count) {
        return 0;
    }
    size_t at = e->at + (size_t)i * type_size(e->type);
    switch (type_size(e->type)) {
    case 1:  return at < t->n ? t->d[at] : 0;
    case 2:  return u16(t, at);
    default: return u32(t, at);
    }
}

/* --- decompression ---------------------------------------------------------------------- */

static size_t packbits(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
    size_t i = 0, o = 0;
    while (i < in_size && o < out_size) {
        int c = (int8_t)in[i++];
        if (c >= 0) {
            for (int k = 0; k <= c && i < in_size && o < out_size; k++) {
                out[o++] = in[i++];
            }
        } else if (c != -128 && i < in_size) {
            uint8_t v = in[i++];
            for (int k = 0; k <= -c && o < out_size; k++) {
                out[o++] = v;
            }
        }
    }
    return o;
}

/* TIFF's LZW: MSB-first codes of 9-12 bits, 256 = clear, 257 = end, widths grow one code early. */
static size_t lzw(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
    static uint16_t prefix[4096];
    static uint8_t suffix[4096], stack[4097];
    for (int i = 0; i < 256; i++) {
        suffix[i] = (uint8_t)i;
    }
    size_t pos = 0, o = 0;
    uint32_t bits = 0;
    int nbits = 0, width = 9, next = 258, old = -1;
    uint8_t first = 0;
    while (o < out_size) {
        while (nbits < width) {
            if (pos >= in_size) {
                return o;
            }
            bits = bits << 8 | in[pos++];
            nbits += 8;
        }
        int code = (int)((bits >> (nbits - width)) & ((1u << width) - 1));
        nbits -= width;
        if (code == 257) {
            break;
        }
        if (code == 256) {
            width = 9;
            next = 258;
            old = -1;
            continue;
        }
        if (old < 0) {
            if (code >= 256) {
                break;   /* broken data */
            }
            out[o++] = (uint8_t)code;
            old = code;
            first = (uint8_t)code;
            continue;
        }
        int in_code = code, sp = 0;
        if (code >= next) {
            if (code > next) {
                break;
            }
            stack[sp++] = first;   /* the string just being defined: old + its own first byte */
            code = old;
        }
        while (code >= 258 && sp < 4096) {
            stack[sp++] = suffix[code];
            code = prefix[code];
        }
        first = (uint8_t)code;
        stack[sp++] = first;
        while (sp && o < out_size) {
            out[o++] = stack[--sp];
        }
        if (next < 4096) {
            prefix[next] = (uint16_t)old;
            suffix[next] = first;
            next++;
        }
        old = in_code;
        if (next + 1 >= (1 << width) && width < 12) {
            width++;
        }
    }
    return o;
}

static bool decompress(int compression, const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
    switch (compression) {
    case 1:
        memcpy(out, in, in_size < out_size ? in_size : out_size);
        return true;
    case 32773:
        packbits(in, in_size, out, out_size);
        return true;
    case 5:
        lzw(in, in_size, out, out_size);
        return true;
    case 8:
    case 32946:
        return zlib_decompress(in, in_size, out, out_size) >= 0;
    default:
        return false;
    }
}

/* --- the picture --------------------------------------------------------------------------- */

struct layout {
    uint32_t width, height, spp, bits, photometric, planar, predictor, extra;
    bool tiled;
    uint32_t tile_w, tile_h;   /* strips: tile_w = width, tile_h = rows per strip */
};

static uint32_t sample_at(const struct tiff *t, const uint8_t *row, uint32_t index, uint32_t bits) {
    switch (bits) {
    case 8:  return row[index];
    case 16: return t->le ? image_le16(row + 2 * index) : image_be16(row + 2 * index);
    default: {
        uint32_t bit = index * bits;
        return (row[bit / 8] >> (8 - bits - bit % 8)) & ((1u << bits) - 1);
    }
    }
}

static void undo_predictor(const struct tiff *t, uint8_t *row, uint32_t width, uint32_t spp, uint32_t bits) {
    if (bits == 8) {
        for (uint32_t i = spp; i < width * spp; i++) {
            row[i] = (uint8_t)(row[i] + row[i - spp]);
        }
    } else if (bits == 16) {
        for (uint32_t i = spp; i < width * spp; i++) {
            uint32_t v = sample_at(t, row, i, 16) + sample_at(t, row, i - spp, 16);
            if (t->le) {
                row[2 * i] = (uint8_t)v;
                row[2 * i + 1] = (uint8_t)(v >> 8);
            } else {
                row[2 * i] = (uint8_t)(v >> 8);
                row[2 * i + 1] = (uint8_t)v;
            }
        }
    }
}

static struct entry *find(struct entry *list, int len, uint32_t id) {
    for (int i = 0; i < len; i++) {
        if (list[i].tag == id) {
            return &list[i];
        }
    }
    return NULL;
}

struct gfx_surface *image_decode_tiff(const uint8_t *d, size_t n, const char **error) {
    struct tiff t = { d, n, d[0] == 'I' };
    uint32_t ifd = u32(&t, 4);
    uint32_t count = u16(&t, ifd);
    if (!ifd || ifd + 2 + 12 * (size_t)count > n) {
        return image_fail(error, "broken TIFF directory");
    }
    struct entry tags[32];
    int ntags = 0;
    for (uint32_t i = 0; i < count && ntags < 32; i++) {
        size_t at = ifd + 2 + 12 * (size_t)i;
        struct entry e = { u16(&t, at), u16(&t, at + 2), u32(&t, at + 4), at + 8 };
        if ((uint64_t)e.count * type_size(e.type) > 4) {
            e.at = u32(&t, at + 8);
        }
        if ((uint64_t)e.at + (uint64_t)e.count * type_size(e.type) <= n) {
            tags[ntags++] = e;
        }
    }
    #define TAG(id) find(tags, ntags, id)

    struct layout l = { 0 };
    l.width = value(&t, TAG(T_WIDTH), 0);
    l.height = value(&t, TAG(T_HEIGHT), 0);
    l.spp = TAG(T_SAMPLES) ? value(&t, TAG(T_SAMPLES), 0) : 1;
    l.bits = TAG(T_BITS) ? value(&t, TAG(T_BITS), 0) : 1;
    uint32_t compression = TAG(T_COMPRESSION) ? value(&t, TAG(T_COMPRESSION), 0) : 1;
    l.photometric = TAG(T_PHOTOMETRIC) ? value(&t, TAG(T_PHOTOMETRIC), 0) : 1;
    l.planar = TAG(T_PLANAR) ? value(&t, TAG(T_PLANAR), 0) : 1;
    l.predictor = TAG(T_PREDICTOR) ? value(&t, TAG(T_PREDICTOR), 0) : 1;
    l.extra = TAG(T_EXTRA) ? value(&t, TAG(T_EXTRA), 0) : 0;
    int orientation = TAG(T_ORIENTATION) ? (int)value(&t, TAG(T_ORIENTATION), 0) : 1;

    if (!image_size_ok(l.width, l.height)) {
        return image_fail(error, "bad TIFF size");
    }
    if (l.bits != 1 && l.bits != 2 && l.bits != 4 && l.bits != 8 && l.bits != 16) {
        return image_fail(error, "unsupported TIFF bit depth");
    }
    if (TAG(T_SAMPLE_FORMAT) && value(&t, TAG(T_SAMPLE_FORMAT), 0) == 3) {
        return image_fail(error, "floating-point TIFFs aren't supported");
    }
    if (compression == 7 || compression == 6) {
        return image_fail(error, "JPEG-compressed TIFFs aren't supported");
    }
    if (compression != 1 && compression != 5 && compression != 8 && compression != 32946 && compression != 32773) {
        return image_fail(error, "unsupported TIFF compression");
    }
    uint32_t base;   /* colour channels before any extra (alpha) ones */
    switch (l.photometric) {
    case 0: case 1: case 3: base = 1; break;
    case 2:                 base = 3; break;
    case 5:                 base = 4; break;
    default: return image_fail(error, "unsupported TIFF colour space");
    }
    if (l.spp < base || l.spp > 8) {
        return image_fail(error, "unsupported TIFF samples");
    }

    struct entry *offsets, *bytes;
    if (TAG(T_TILE_OFFSETS)) {
        l.tiled = true;
        l.tile_w = value(&t, TAG(T_TILE_WIDTH), 0);
        l.tile_h = value(&t, TAG(T_TILE_LENGTH), 0);
        offsets = TAG(T_TILE_OFFSETS);
        bytes = TAG(T_TILE_BYTES);
    } else {
        l.tile_w = l.width;
        l.tile_h = TAG(T_ROWS_PER_STRIP) ? value(&t, TAG(T_ROWS_PER_STRIP), 0) : l.height;
        l.tile_h = l.tile_h > l.height || !l.tile_h ? l.height : l.tile_h;
        offsets = TAG(T_STRIP_OFFSETS);
        bytes = TAG(T_STRIP_BYTES);
    }
    if (!offsets || !l.tile_w || !l.tile_h || l.tile_w > 65536 || l.tile_h > 65536) {
        return image_fail(error, "broken TIFF layout");
    }

    /* Decode everything into one buffer of the whole picture: spp samples per pixel (chunky). */
    uint32_t planes = l.planar == 2 ? l.spp : 1, per_plane = l.planar == 2 ? 1 : l.spp;
    size_t row_bytes = ((size_t)l.width * l.spp * l.bits + 7) / 8;
    size_t tile_row = ((size_t)l.tile_w * per_plane * l.bits + 7) / 8;
    uint8_t *image = calloc((size_t)l.height, row_bytes);
    uint8_t *tile = malloc(tile_row * l.tile_h);
    if (!image || !tile) {
        free(image);
        free(tile);
        return image_fail(error, "not enough memory");
    }
    uint32_t across = (l.width + l.tile_w - 1) / l.tile_w, down = (l.height + l.tile_h - 1) / l.tile_h;
    const char *problem = NULL;
    for (uint32_t pl = 0; pl < planes && !problem; pl++) {
        for (uint32_t ty = 0; ty < down && !problem; ty++) {
            for (uint32_t tx = 0; tx < across && !problem; tx++) {
                uint32_t index = pl * across * down + ty * across + tx;
                uint32_t off = value(&t, offsets, index);
                uint32_t len = bytes ? value(&t, bytes, index) : (uint32_t)(n - off);
                if (off >= n) {
                    continue;   /* missing: leave it blank */
                }
                if (off + (uint64_t)len > n) {
                    len = (uint32_t)(n - off);
                }
                memset(tile, 0, tile_row * l.tile_h);
                if (!decompress((int)compression, d + off, len, tile, tile_row * l.tile_h)) {
                    problem = "broken TIFF data";
                    break;
                }
                for (uint32_t y = 0; y < l.tile_h; y++) {
                    uint32_t iy = ty * l.tile_h + y;
                    if (iy >= l.height) {
                        break;
                    }
                    uint8_t *src = tile + tile_row * y;
                    if (l.predictor == 2) {
                        undo_predictor(&t, src, l.tile_w, per_plane, l.bits);
                    }
                    /* Copy the part of this tile row that's inside the picture. */
                    uint32_t x0 = tx * l.tile_w, w = l.width - x0 < l.tile_w ? l.width - x0 : l.tile_w;
                    if (l.planar == 2) {
                        for (uint32_t x = 0; x < w; x++) {
                            uint32_t v = sample_at(&t, src, x, l.bits);
                            /* re-pack into the chunky image at sample (x0 + x) * spp + pl */
                            size_t s = (size_t)(x0 + x) * l.spp + pl, bit = s * l.bits;
                            uint8_t *dst = image + row_bytes * iy;
                            if (l.bits == 8) {
                                dst[s] = (uint8_t)v;
                            } else if (l.bits == 16) {
                                dst[2 * s] = (uint8_t)(t.le ? v : v >> 8);
                                dst[2 * s + 1] = (uint8_t)(t.le ? v >> 8 : v);
                            } else {
                                dst[bit / 8] |= (uint8_t)(v << (8 - l.bits - bit % 8));
                            }
                        }
                    } else if ((x0 * l.spp * l.bits) % 8 == 0) {
                        memcpy(image + row_bytes * iy + (size_t)x0 * l.spp * l.bits / 8, src,
                               ((size_t)w * l.spp * l.bits + 7) / 8);
                    }
                }
            }
        }
    }
    free(tile);
    if (problem) {
        free(image);
        return image_fail(error, problem);
    }

    /* Samples -> pixels. */
    uint32_t max = (1u << l.bits) - 1;
    uint32_t palette[256] = { 0 };
    struct entry *map = TAG(T_COLORMAP);
    if (l.photometric == 3) {
        uint32_t colors = 1u << (l.bits > 8 ? 8 : l.bits);
        if (!map || map->count < 3 * colors) {
            free(image);
            return image_fail(error, "TIFF palette missing");
        }
        for (uint32_t i = 0; i < colors; i++) {
            palette[i] = 0xFF000000u | (value(&t, map, i) >> 8) << 16 | (value(&t, map, colors + i) >> 8) << 8 |
                         value(&t, map, 2 * colors + i) >> 8;
        }
    }
    struct gfx_surface *s = gfx_surface_create((int)l.width, (int)l.height);
    if (!s) {
        free(image);
        return image_fail(error, "not enough memory");
    }
    bool has_alpha = l.spp > base && (l.extra == 1 || l.extra == 2);
    for (uint32_t y = 0; y < l.height; y++) {
        const uint8_t *row = image + row_bytes * y;
        uint32_t *out = gfx_row(s, (int)y);
        for (uint32_t x = 0; x < l.width; x++) {
            uint32_t v[8];
            for (uint32_t c = 0; c < l.spp; c++) {
                uint32_t raw = sample_at(&t, row, x * l.spp + c, l.bits);
                v[c] = l.photometric == 3 && c == 0 ? raw : (raw * 255 + max / 2) / max;
            }
            uint32_t a = has_alpha ? v[base] : 255, r, g, b;
            switch (l.photometric) {
            case 0:  r = g = b = 255 - v[0]; break;
            case 1:  r = g = b = v[0]; break;
            case 3: {
                uint32_t c = palette[v[0] & 255];
                r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
                break;
            }
            case 5:
                r = (255 - v[0]) * (255 - v[3]) / 255;
                g = (255 - v[1]) * (255 - v[3]) / 255;
                b = (255 - v[2]) * (255 - v[3]) / 255;
                break;
            default: r = v[0], g = v[1], b = v[2]; break;
            }
            if (has_alpha && l.extra == 1) {
                /* Associated alpha: the colours are premultiplied already. */
                out[x] = a << 24 | (r > a ? a : r) << 16 | (g > a ? a : g) << 8 | (b > a ? a : b);
            } else {
                out[x] = image_pixel(r, g, b, a);
            }
        }
    }
    free(image);
    #undef TAG
    return orientation != 1 ? image_orient(s, orientation) : s;
}
