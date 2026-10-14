/*
 * JPEG (ITU T.81), the parts photos use:
 *
 *   baseline and extended sequential (SOF0/SOF1) and progressive (SOF2)
 *   Huffman coding, 8-bit samples, 1, 3 or 4 components
 *   any chroma subsampling, restart intervals
 *   YCbCr, grey, RGB, CMYK and YCCK (Adobe)
 *   the EXIF orientation tag: pictures come out the right way up
 *
 * Every scan's coefficients are collected first (progressive scans add
 * to them in passes), then each 8x8 block goes through the inverse DCT
 * and the components are combined into RGB.
 */

#include <math.h>

#include "internal.h"

#define MAX_COMPONENTS 4

/* Zig-zag position -> position in the 8x8 block. */
static const uint8_t natural_order[64 + 16] = {
    0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    /* extra entries so a corrupt run past the end lands somewhere harmless */
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,
};

#define FAST_BITS 9

struct huffman {
    uint8_t fast_len[1 << FAST_BITS];   /* code length for a code starting with these bits (0: longer) */
    uint8_t fast_sym[1 << FAST_BITS];
    int32_t maxcode[18];                /* largest code of each length (-1: none) */
    int32_t valptr[17];                 /* where that length's symbols start */
    int32_t mincode[17];
    uint8_t symbols[256];
    bool present;
};

struct component {
    int id, h, v, tq;
    int dc_table, ac_table;
    int bw, bh;               /* blocks across and down, padded to whole MCUs */
    int16_t *coeffs;          /* bw * bh blocks of 64, natural order, not yet dequantized */
    int dc_pred;
    uint8_t *pixels;          /* after the IDCT: bw*8 x bh*8 */
};

struct jpeg {
    const uint8_t *data, *end, *pos;
    const char *error;
    uint32_t width, height;
    int ncomp, hmax, vmax, mcux, mcuy;
    bool progressive;
    struct component comp[MAX_COMPONENTS];
    uint16_t quant[4][64];    /* natural order */
    struct huffman dc[4], ac[4];
    int restart_interval;
    int adobe_transform;      /* -1: no Adobe marker */
    bool jfif;
    int orientation;          /* EXIF: 1 = upright */

    /* The entropy-coded data being read. */
    uint32_t bits;
    int nbits;
    bool hit_marker;
    int eobrun;
};

/* --- reading bits ----------------------------------------------------------------------- */

static void fill_bits(struct jpeg *j) {
    while (j->nbits <= 24) {
        uint32_t b = 0;
        if (!j->hit_marker && j->pos < j->end) {
            b = *j->pos;
            if (b == 0xFF) {
                uint8_t next = j->pos + 1 < j->end ? j->pos[1] : 0;
                if (next == 0x00) {
                    j->pos += 2;   /* a stuffed FF */
                } else {
                    j->hit_marker = true;   /* a marker: feed zeros until it's dealt with */
                    b = 0;
                }
            } else {
                j->pos++;
            }
        }
        j->bits |= b << (24 - j->nbits);
        j->nbits += 8;
    }
}

static uint32_t get_bits(struct jpeg *j, int n) {
    if (!n) {
        return 0;
    }
    fill_bits(j);
    uint32_t v = j->bits >> (32 - n);
    j->bits <<= n;
    j->nbits -= n;
    return v;
}

static int get_bit(struct jpeg *j) {
    return (int)get_bits(j, 1);
}

/* An n-bit value in JPEG's "extend" coding (top bit 0 means negative). */
static int receive_extend(struct jpeg *j, int n) {
    if (!n) {
        return 0;
    }
    int v = (int)get_bits(j, n);
    return v < (1 << (n - 1)) ? v - (1 << n) + 1 : v;
}

static int decode(struct jpeg *j, const struct huffman *h) {
    fill_bits(j);
    uint32_t peek = j->bits >> (32 - FAST_BITS);
    int len = h->fast_len[peek];
    if (len) {
        j->bits <<= len;
        j->nbits -= len;
        return h->fast_sym[peek];
    }
    /* Longer than FAST_BITS: compare against each length's largest code. */
    for (len = FAST_BITS + 1; len <= 16; len++) {
        int32_t code = (int32_t)(j->bits >> (32 - len));
        if (code <= h->maxcode[len]) {
            j->bits <<= len;
            j->nbits -= len;
            int index = h->valptr[len] + code - h->mincode[len];
            return index >= 0 && index < 256 ? h->symbols[index] : 0;
        }
    }
    j->error = "bad Huffman code";
    j->bits <<= 16;
    j->nbits -= 16;
    return 0;
}

static bool build_huffman(struct huffman *h, const uint8_t counts[16], const uint8_t *symbols, int total) {
    memset(h, 0, sizeof(*h));
    memcpy(h->symbols, symbols, (size_t)total);
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        h->valptr[len] = k;
        h->mincode[len] = code;
        int n = counts[len - 1];
        for (int i = 0; i < n; i++, code++, k++) {
            if (len <= FAST_BITS) {
                /* Every FAST_BITS-bit pattern starting with this code. */
                int shift = FAST_BITS - len;
                for (int f = 0; f < (1 << shift); f++) {
                    h->fast_len[(code << shift) | f] = (uint8_t)len;
                    h->fast_sym[(code << shift) | f] = symbols[k];
                }
            }
        }
        h->maxcode[len] = n ? code - 1 : -1;
        if (code > (1 << len)) {
            return false;   /* more codes than fit */
        }
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    h->present = true;
    return true;
}

/* --- markers ---------------------------------------------------------------------------------- */

static int segment_length(struct jpeg *j) {
    if (j->end - j->pos < 2) {
        return -1;
    }
    int len = image_be16(j->pos);
    return len >= 2 && j->end - j->pos >= len ? len : -1;
}

static bool read_dqt(struct jpeg *j, const uint8_t *p, int len) {
    while (len > 0) {
        int precision = p[0] >> 4, id = p[0] & 15;
        if (id > 3) {
            return false;
        }
        int need = 1 + 64 * (precision ? 2 : 1);
        if (len < need) {
            return false;
        }
        for (int k = 0; k < 64; k++) {
            j->quant[id][natural_order[k]] = precision ? image_be16(p + 1 + 2 * k) : p[1 + k];
        }
        p += need;
        len -= need;
    }
    return true;
}

static bool read_dht(struct jpeg *j, const uint8_t *p, int len) {
    while (len > 0) {
        if (len < 17) {
            return false;
        }
        int cls = p[0] >> 4, id = p[0] & 15;
        int total = 0;
        for (int i = 0; i < 16; i++) {
            total += p[1 + i];
        }
        if (id > 3 || cls > 1 || total > 256 || len < 17 + total) {
            return false;
        }
        if (!build_huffman(cls ? &j->ac[id] : &j->dc[id], p + 1, p + 17, total)) {
            return false;
        }
        p += 17 + total;
        len -= 17 + total;
    }
    return true;
}

static bool read_sof(struct jpeg *j, const uint8_t *p, int len) {
    if (len < 6 || p[0] != 8) {
        j->error = p[0] != 8 ? "12-bit JPEGs aren't supported" : "bad frame header";
        return false;
    }
    j->height = image_be16(p + 1);
    j->width = image_be16(p + 3);
    j->ncomp = p[5];
    if (!image_size_ok(j->width, j->height)) {
        j->error = j->height ? "picture too large" : "pictures without a height aren't supported";
        return false;
    }
    if ((j->ncomp != 1 && j->ncomp != 3 && j->ncomp != 4) || len < 6 + 3 * j->ncomp) {
        j->error = "unsupported number of colour components";
        return false;
    }
    j->hmax = j->vmax = 1;
    for (int i = 0; i < j->ncomp; i++) {
        struct component *c = &j->comp[i];
        c->id = p[6 + 3 * i];
        c->h = p[7 + 3 * i] >> 4;
        c->v = p[7 + 3 * i] & 15;
        c->tq = p[8 + 3 * i] & 3;
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4) {
            j->error = "bad sampling factors";
            return false;
        }
        j->hmax = c->h > j->hmax ? c->h : j->hmax;
        j->vmax = c->v > j->vmax ? c->v : j->vmax;
    }
    j->mcux = (int)((j->width + 8 * j->hmax - 1) / (8 * j->hmax));
    j->mcuy = (int)((j->height + 8 * j->vmax - 1) / (8 * j->vmax));
    for (int i = 0; i < j->ncomp; i++) {
        struct component *c = &j->comp[i];
        c->bw = j->mcux * c->h;
        c->bh = j->mcuy * c->v;
        c->coeffs = calloc((size_t)c->bw * c->bh, 64 * sizeof(int16_t));
        if (!c->coeffs) {
            j->error = "not enough memory";
            return false;
        }
    }
    return true;
}

/* EXIF (inside APP1): only the orientation tag of the first directory matters to us. */
static void read_exif(struct jpeg *j, const uint8_t *p, int len) {
    if (len < 14 || memcmp(p, "Exif\0\0", 6) != 0) {
        return;
    }
    const uint8_t *t = p + 6;
    int tlen = len - 6;
    bool le = t[0] == 'I';
    #define U16(q) (le ? image_le16(q) : image_be16(q))
    #define U32(q) (le ? image_le32(q) : image_be32(q))
    uint32_t ifd = U32(t + 4);
    if (ifd + 2 > (uint32_t)tlen) {
        return;
    }
    int count = U16(t + ifd);
    for (int i = 0; i < count; i++) {
        uint32_t e = ifd + 2 + 12 * (uint32_t)i;
        if (e + 12 > (uint32_t)tlen) {
            return;
        }
        if (U16(t + e) == 0x0112) {
            int o = U16(t + e + 8);
            j->orientation = o >= 1 && o <= 8 ? o : 1;
        }
    }
    #undef U16
    #undef U32
}

/* --- decoding blocks ------------------------------------------------------------------------------ */

static void decode_block_baseline(struct jpeg *j, struct component *c, int16_t *blk) {
    int t = decode(j, &j->dc[c->dc_table]);
    int diff = t ? receive_extend(j, t) : 0;
    c->dc_pred += diff;
    blk[0] = (int16_t)c->dc_pred;
    const struct huffman *ac = &j->ac[c->ac_table];
    for (int k = 1; k < 64;) {
        int rs = decode(j, ac);
        int r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r != 15) {
                break;   /* end of block */
            }
            k += 16;
            continue;
        }
        k += r;
        blk[natural_order[k > 63 ? 64 : k]] = (int16_t)receive_extend(j, s);
        k++;
    }
}

static void decode_dc_progressive(struct jpeg *j, struct component *c, int16_t *blk, int ah, int al) {
    if (!ah) {
        int t = decode(j, &j->dc[c->dc_table]);
        int diff = t ? receive_extend(j, t) : 0;
        c->dc_pred += diff;
        blk[0] = (int16_t)(c->dc_pred * (1 << al));
    } else if (get_bit(j)) {
        blk[0] = (int16_t)(blk[0] | (1 << al));
    }
}

static void decode_ac_first(struct jpeg *j, struct component *c, int16_t *blk, int ss, int se, int al) {
    if (j->eobrun) {
        j->eobrun--;
        return;
    }
    const struct huffman *ac = &j->ac[c->ac_table];
    for (int k = ss; k <= se;) {
        int rs = decode(j, ac);
        int r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r < 15) {
                j->eobrun = (1 << r) - 1;
                if (r) {
                    j->eobrun += (int)get_bits(j, r);
                }
                break;
            }
            k += 16;
            continue;
        }
        k += r;
        blk[natural_order[k > 63 ? 64 : k]] = (int16_t)(receive_extend(j, s) * (1 << al));
        k++;
    }
}

/* Successive approximation: one more bit for every coefficient already known, plus new ones. */
static void decode_ac_refine(struct jpeg *j, struct component *c, int16_t *blk, int ss, int se, int al) {
    int bit = 1 << al;
    if (j->eobrun) {
        j->eobrun--;
        for (int k = ss; k <= se; k++) {
            int16_t *p = &blk[natural_order[k]];
            if (*p && get_bit(j) && !(*p & bit)) {
                *p = (int16_t)(*p + (*p > 0 ? bit : -bit));
            }
        }
        return;
    }
    const struct huffman *ac = &j->ac[c->ac_table];
    int k = ss;
    do {
        int rs = decode(j, ac);
        int r = rs >> 4, s = rs & 15, v = 0;
        if (!s) {
            if (r < 15) {
                j->eobrun = (1 << r) - 1;
                if (r) {
                    j->eobrun += (int)get_bits(j, r);
                }
                r = 64;   /* refine the rest of this block, place nothing */
            }
            /* r == 15: a run of 16 zeros (refining any non-zero ones on the way) */
        } else {
            v = get_bit(j) ? bit : -bit;
        }
        while (k <= se) {
            int16_t *p = &blk[natural_order[k++]];
            if (*p) {
                if (get_bit(j) && !(*p & bit)) {
                    *p = (int16_t)(*p + (*p > 0 ? bit : -bit));
                }
            } else {
                if (r == 0) {
                    *p = (int16_t)v;
                    break;
                }
                r--;
            }
        }
    } while (k <= se && !j->error);
}

/* After a restart interval: the bits start over at the RSTn marker. */
static void restart(struct jpeg *j) {
    j->bits = 0;
    j->nbits = 0;
    j->hit_marker = false;
    j->eobrun = 0;
    /* Find the RSTn marker and skip it. */
    while (j->pos + 1 < j->end && !(j->pos[0] == 0xFF && j->pos[1] >= 0xD0 && j->pos[1] <= 0xD7)) {
        j->pos++;
    }
    if (j->pos + 1 < j->end) {
        j->pos += 2;
    }
    for (int i = 0; i < j->ncomp; i++) {
        j->comp[i].dc_pred = 0;
    }
}

static bool read_scan(struct jpeg *j, const uint8_t *p, int len) {
    int n = p[0];
    if (n < 1 || n > j->ncomp || len < 1 + 2 * n + 3) {
        return false;
    }
    struct component *sc[MAX_COMPONENTS];
    for (int i = 0; i < n; i++) {
        int id = p[1 + 2 * i], tables = p[2 + 2 * i];
        sc[i] = NULL;
        for (int k = 0; k < j->ncomp; k++) {
            if (j->comp[k].id == id) {
                sc[i] = &j->comp[k];
            }
        }
        if (!sc[i]) {
            return false;
        }
        sc[i]->dc_table = (tables >> 4) & 3;
        sc[i]->ac_table = tables & 3;
    }
    int ss = p[1 + 2 * n], se = p[2 + 2 * n], ah = p[3 + 2 * n] >> 4, al = p[3 + 2 * n] & 15;
    if (!j->progressive) {
        ss = 0;
        se = 63;
    }
    if (ss > se || se > 63 || (j->progressive && ss == 0 && se != 0) || (j->progressive && ss > 0 && n != 1)) {
        j->error = "bad scan header";
        return false;
    }
    for (int i = 0; i < n; i++) {
        bool need_dc = ss == 0 && ah == 0, need_ac = se > 0;
        if ((need_dc && !j->dc[sc[i]->dc_table].present) || (need_ac && !j->ac[sc[i]->ac_table].present)) {
            j->error = "missing Huffman table";
            return false;
        }
        sc[i]->dc_pred = 0;
    }

    j->pos += 2 + len;   /* past the length and the header: the entropy-coded data */
    j->bits = 0;
    j->nbits = 0;
    j->hit_marker = false;
    j->eobrun = 0;

    int restart_count = j->restart_interval;
    if (n == 1) {
        /* Not interleaved: the component's own blocks, just those covering the picture. */
        struct component *c = sc[0];
        int w = (int)((j->width * c->h + j->hmax - 1) / j->hmax + 7) / 8;
        int h = (int)((j->height * c->v + j->vmax - 1) / j->vmax + 7) / 8;
        for (int by = 0; by < h && !j->error; by++) {
            for (int bx = 0; bx < w && !j->error; bx++) {
                int16_t *blk = c->coeffs + ((size_t)by * c->bw + bx) * 64;
                if (!j->progressive) {
                    decode_block_baseline(j, c, blk);
                } else if (ss == 0) {
                    decode_dc_progressive(j, c, blk, ah, al);
                } else if (ah == 0) {
                    decode_ac_first(j, c, blk, ss, se, al);
                } else {
                    decode_ac_refine(j, c, blk, ss, se, al);
                }
                if (j->restart_interval && --restart_count == 0) {
                    restart(j);
                    restart_count = j->restart_interval;
                }
            }
        }
    } else {
        for (int my = 0; my < j->mcuy && !j->error; my++) {
            for (int mx = 0; mx < j->mcux && !j->error; mx++) {
                for (int i = 0; i < n; i++) {
                    struct component *c = sc[i];
                    for (int v = 0; v < c->v; v++) {
                        for (int h = 0; h < c->h; h++) {
                            int bx = mx * c->h + h, by = my * c->v + v;
                            int16_t *blk = c->coeffs + ((size_t)by * c->bw + bx) * 64;
                            if (!j->progressive) {
                                decode_block_baseline(j, c, blk);
                            } else {
                                decode_dc_progressive(j, c, blk, ah, al);
                            }
                        }
                    }
                }
                if (j->restart_interval && --restart_count == 0) {
                    restart(j);
                    restart_count = j->restart_interval;
                }
            }
        }
    }
    /* Back to the markers: skip to the next one that isn't a restart. */
    while (j->pos + 1 < j->end) {
        if (j->pos[0] == 0xFF && j->pos[1] != 0x00 && !(j->pos[1] >= 0xD0 && j->pos[1] <= 0xD7)) {
            break;
        }
        j->pos++;
    }
    return !j->error;
}

/* --- inverse DCT and colour ------------------------------------------------------------------------- */

static float idct_table[8][8];   /* [x][u]: C(u)/2 * cos((2x+1) u pi / 16) */

static void idct_init(void) {
    static bool done;
    if (done) {
        return;
    }
    for (int x = 0; x < 8; x++) {
        for (int u = 0; u < 8; u++) {
            float cu = u == 0 ? 0.70710678f : 1.0f;
            idct_table[x][u] = cu / 2 * (float)cos((2 * x + 1) * u * M_PI / 16);
        }
    }
    done = true;
}

static void idct_block(const int16_t *in, const uint16_t *q, uint8_t *out, int stride) {
    float tmp[64];
    /* Columns: for each u, combine the rows v. Columns that are only DC come out flat. */
    for (int u = 0; u < 8; u++) {
        bool flat = true;
        for (int v = 1; v < 8 && flat; v++) {
            flat = in[v * 8 + u] == 0;
        }
        if (flat) {
            float dc = in[u] * q[u] * idct_table[0][0];
            for (int y = 0; y < 8; y++) {
                tmp[y * 8 + u] = dc;
            }
            continue;
        }
        float c[8];
        for (int v = 0; v < 8; v++) {
            c[v] = (float)(in[v * 8 + u] * q[v * 8 + u]);
        }
        for (int y = 0; y < 8; y++) {
            const float *t = idct_table[y];
            tmp[y * 8 + u] = c[0] * t[0] + c[1] * t[1] + c[2] * t[2] + c[3] * t[3] + c[4] * t[4] + c[5] * t[5] +
                             c[6] * t[6] + c[7] * t[7];
        }
    }
    /* Rows. */
    for (int y = 0; y < 8; y++) {
        const float *r = tmp + y * 8;
        for (int x = 0; x < 8; x++) {
            const float *t = idct_table[x];
            float v = r[0] * t[0] + r[1] * t[1] + r[2] * t[2] + r[3] * t[3] + r[4] * t[4] + r[5] * t[5] +
                      r[6] * t[6] + r[7] * t[7];
            out[y * stride + x] = image_clamp255((int)(v + (v >= 0 ? 0.5f : -0.5f)) + 128);
        }
    }
}

static bool reconstruct(struct jpeg *j) {
    idct_init();
    for (int i = 0; i < j->ncomp; i++) {
        struct component *c = &j->comp[i];
        int stride = c->bw * 8;
        c->pixels = malloc((size_t)stride * c->bh * 8);
        if (!c->pixels) {
            return false;
        }
        for (int by = 0; by < c->bh; by++) {
            for (int bx = 0; bx < c->bw; bx++) {
                idct_block(c->coeffs + ((size_t)by * c->bw + bx) * 64, j->quant[c->tq],
                           c->pixels + (size_t)by * 8 * stride + bx * 8, stride);
            }
        }
        free(c->coeffs);
        c->coeffs = NULL;
    }
    return true;
}

/*
 * Where picture coordinate p falls in a component sampled at f out of fmax
 * (chroma is usually at half size): the two samples around it and the
 * second one's weight out of 256. Sample centres line up as in libjpeg's
 * "fancy" upsampling: at half size, 3/4 of the nearer and 1/4 of the other.
 */
struct tap {
    int i0, i1, w;
};

static struct tap tap_at(uint32_t p, int f, int fmax, int size) {
    int denom = 2 * fmax, n = (int)(2 * p + 1) * f - fmax;
    int i0 = n >= 0 ? n / denom : -1, frac = n - i0 * denom;
    struct tap t = { i0, i0 + 1, frac * 256 / denom };
    t.i0 = t.i0 < 0 ? 0 : t.i0 >= size ? size - 1 : t.i0;
    t.i1 = t.i1 >= size ? size - 1 : t.i1;
    return t;
}

/* One component's sample for a picture pixel, interpolated if the component is smaller. */
static inline int sample(const struct jpeg *j, const struct component *c, uint32_t x, uint32_t y) {
    size_t stride = (size_t)c->bw * 8;
    if (c->h == j->hmax && c->v == j->vmax) {
        return c->pixels[y * stride + x];
    }
    int cw = (int)((j->width * (uint32_t)c->h + (uint32_t)j->hmax - 1) / (uint32_t)j->hmax);
    int ch = (int)((j->height * (uint32_t)c->v + (uint32_t)j->vmax - 1) / (uint32_t)j->vmax);
    struct tap tx = tap_at(x, c->h, j->hmax, cw), ty = tap_at(y, c->v, j->vmax, ch);
    const uint8_t *r0 = c->pixels + (size_t)ty.i0 * stride, *r1 = c->pixels + (size_t)ty.i1 * stride;
    int top = r0[tx.i0] * (256 - tx.w) + r0[tx.i1] * tx.w;
    int bottom = r1[tx.i0] * (256 - tx.w) + r1[tx.i1] * tx.w;
    return (top * (256 - ty.w) + bottom * ty.w + 32768) >> 16;
}

static void ycc_to_rgb(int y, int cb, int cr, int *r, int *g, int *b) {
    cb -= 128;
    cr -= 128;
    /* JFIF: full-range BT.601, in 16.16 fixed point. */
    *r = image_clamp255(y + ((91881 * cr + 32768) >> 16));
    *g = image_clamp255(y - ((22554 * cb + 46802 * cr - 32768) >> 16));
    *b = image_clamp255(y + ((116130 * cb + 32768) >> 16));
}

static struct gfx_surface *to_surface(struct jpeg *j) {
    struct gfx_surface *s = gfx_surface_create((int)j->width, (int)j->height);
    if (!s) {
        return NULL;
    }
    /* What the three/four components mean. */
    bool rgb = j->ncomp == 3 && (j->adobe_transform == 0 ||
               (j->comp[0].id == 'R' && j->comp[1].id == 'G' && j->comp[2].id == 'B'));
    for (uint32_t y = 0; y < j->height; y++) {
        uint32_t *row = gfx_row(s, (int)y);
        for (uint32_t x = 0; x < j->width; x++) {
            int r, g, b;
            if (j->ncomp == 1) {
                r = g = b = sample(j, &j->comp[0], x, y);
            } else if (j->ncomp == 3) {
                int c0 = sample(j, &j->comp[0], x, y), c1 = sample(j, &j->comp[1], x, y),
                    c2 = sample(j, &j->comp[2], x, y);
                if (rgb) {
                    r = c0, g = c1, b = c2;
                } else {
                    ycc_to_rgb(c0, c1, c2, &r, &g, &b);
                }
            } else {
                /* Adobe CMYK is stored inverted; YCCK is YCbCr (then inverted) plus K. */
                int c0 = sample(j, &j->comp[0], x, y), c1 = sample(j, &j->comp[1], x, y),
                    c2 = sample(j, &j->comp[2], x, y), k = sample(j, &j->comp[3], x, y);
                if (j->adobe_transform == 2) {
                    ycc_to_rgb(c0, c1, c2, &c0, &c1, &c2);
                    c0 = 255 - c0;
                    c1 = 255 - c1;
                    c2 = 255 - c2;
                }
                r = (c0 * k + 127) / 255;
                g = (c1 * k + 127) / 255;
                b = (c2 * k + 127) / 255;
            }
            row[x] = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        }
    }
    return s;
}

/* --- putting it together -------------------------------------------------------------------------------- */

static void cleanup(struct jpeg *j) {
    for (int i = 0; i < MAX_COMPONENTS; i++) {
        free(j->comp[i].coeffs);
        free(j->comp[i].pixels);
    }
}

struct gfx_surface *image_decode_jpeg(const uint8_t *data, size_t size, const char **error) {
    struct jpeg *j = calloc(1, sizeof(*j));
    if (!j) {
        return image_fail(error, "not enough memory");
    }
    j->data = data;
    j->end = data + size;
    j->pos = data + 2;   /* past SOI */
    j->adobe_transform = -1;
    j->orientation = 1;
    bool have_frame = false;

    while (!j->error) {
        /* Next marker (fill bytes FF FF ... allowed). */
        while (j->pos < j->end && *j->pos != 0xFF) {
            j->pos++;
        }
        while (j->pos < j->end && *j->pos == 0xFF) {
            j->pos++;
        }
        if (j->pos >= j->end) {
            break;   /* no EOI: show what we have */
        }
        int marker = *j->pos++;
        if (marker == 0xD9) {
            break;   /* EOI */
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;   /* no length */
        }
        int len = segment_length(j);
        if (len < 0) {
            j->error = "truncated file";
            break;
        }
        const uint8_t *p = j->pos + 2;
        int body = len - 2;
        switch (marker) {
        case 0xC0:
        case 0xC1:
        case 0xC2:
            if (have_frame) {
                j->error = "more than one frame";
                break;
            }
            j->progressive = marker == 0xC2;
            have_frame = read_sof(j, p, body);
            if (!have_frame && !j->error) {
                j->error = "bad frame header";
            }
            break;
        case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB:
        case 0xCD: case 0xCE: case 0xCF:
            j->error = marker >= 0xC9 ? "arithmetic-coded JPEGs aren't supported"
                                      : "lossless/hierarchical JPEGs aren't supported";
            break;
        case 0xC4:
            if (!read_dht(j, p, body)) {
                j->error = "bad Huffman table";
            }
            break;
        case 0xDB:
            if (!read_dqt(j, p, body)) {
                j->error = "bad quantisation table";
            }
            break;
        case 0xDD:
            j->restart_interval = body >= 2 ? image_be16(p) : 0;
            break;
        case 0xE0:
            j->jfif = body >= 5 && memcmp(p, "JFIF", 5) == 0;
            break;
        case 0xE1:
            read_exif(j, p, body);
            break;
        case 0xEE:
            if (body >= 12 && memcmp(p, "Adobe", 5) == 0) {
                j->adobe_transform = p[11];
            }
            break;
        case 0xDA:
            if (!have_frame) {
                j->error = "scan before the frame header";
                break;
            }
            if (!read_scan(j, p, body) && !j->error) {
                j->error = "bad scan";
            }
            continue;   /* read_scan left pos at the next marker */
        default:
            break;      /* APPn, COM, ...: skip */
        }
        j->pos += len;
    }

    struct gfx_surface *s = NULL;
    if (!have_frame && !j->error) {
        j->error = "no picture in the file";
    }
    if (!j->error) {
        if (!reconstruct(j)) {
            j->error = "not enough memory";
        } else {
            s = to_surface(j);
            if (!s) {
                j->error = "not enough memory";
            } else if (j->orientation != 1) {
                s = image_orient(s, j->orientation);
            }
        }
    }
    const char *why = j->error;
    cleanup(j);
    free(j);
    if (!s) {
        return image_fail(error, why ? why : "not enough memory");
    }
    return s;
}
