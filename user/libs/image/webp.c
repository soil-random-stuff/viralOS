/*
 * WebP (RFC 9649): the RIFF container, the lossless format (VP8L), and
 * the alpha channel of lossy pictures. Lossy frames are vp8.c's job.
 *
 *   simple lossy        RIFF WEBP + "VP8 "
 *   simple lossless     RIFF WEBP + "VP8L"
 *   extended            RIFF WEBP + "VP8X" + ["ALPH"] + "VP8 "/"VP8L"
 *   animated            the first frame ("ANMF"), placed on the canvas
 */

#include "internal.h"
#include "webp_tables.h"

struct gfx_surface *vp8_decode(const uint8_t *data, size_t size, const char **error);

/* --- bits, least significant first ------------------------------------------------------- */

struct bits {
    const uint8_t *data;
    size_t size, pos;
    uint64_t buf;
    int count;
    bool overrun;
};

static void fill(struct bits *b) {
    while (b->count <= 56) {
        if (b->pos < b->size) {
            b->buf |= (uint64_t)b->data[b->pos++] << b->count;
        } else if (b->count < 0) {
            break;
        }
        b->count += 8;   /* past the end: zeros */
    }
}

static uint32_t read_bits(struct bits *b, int n) {
    if (!n) {
        return 0;
    }
    if (b->count < n) {
        fill(b);
        if (b->pos >= b->size && b->count < n + 64) {
            /* (zeros keep coming; only notice if we've gone far past the end) */
        }
    }
    uint32_t v = (uint32_t)(b->buf & ((1ull << n) - 1));
    b->buf >>= n;
    b->count -= n;
    if (b->pos >= b->size && b->count < -64) {
        b->overrun = true;
    }
    return v;
}

/* --- prefix codes ------------------------------------------------------------------------- */

#define FAST 8
#define MAX_LEN 15

struct code {
    uint16_t fast[1 << FAST];   /* length << 12 | symbol for codes of up to FAST bits; 0: longer */
    uint16_t count[MAX_LEN + 1];
    uint16_t *symbols;          /* by code order */
    int single;                 /* the only symbol (no bits are read), or -1 */
};

static void free_code(struct code *c) {
    free(c->symbols);
    c->symbols = NULL;
}

static bool build_code(struct code *c, const uint8_t *lengths, int n) {
    memset(c->count, 0, sizeof(c->count));
    memset(c->fast, 0, sizeof(c->fast));
    c->single = -1;
    int used = 0, last = 0;
    for (int i = 0; i < n; i++) {
        c->count[lengths[i]]++;
        if (lengths[i]) {
            used++;
            last = i;
        }
    }
    if (used <= 1) {
        c->single = last;   /* one symbol (or none: symbol 0), coded in zero bits */
        return true;
    }
    /* The code has to be complete: exactly fill the space. */
    int left = 1;
    for (int len = 1; len <= MAX_LEN; len++) {
        left = (left << 1) - c->count[len];
        if (left < 0) {
            return false;
        }
    }
    if (left) {
        return false;
    }
    c->symbols = malloc(sizeof(uint16_t) * (size_t)used);
    if (!c->symbols) {
        return false;
    }
    uint16_t offset[MAX_LEN + 2];
    offset[1] = 0;
    for (int len = 1; len <= MAX_LEN; len++) {
        offset[len + 1] = (uint16_t)(offset[len] + c->count[len]);
    }
    for (int i = 0; i < n; i++) {
        if (lengths[i]) {
            c->symbols[offset[lengths[i]]++] = (uint16_t)i;
        }
    }
    /* Fast table: canonical codes, sent first bit first, so stored bit-reversed. */
    int code = 0, k = 0;
    for (int len = 1; len <= FAST; len++) {
        for (int i = 0; i < c->count[len]; i++, code++, k++) {
            int rev = 0;
            for (int b = 0; b < len; b++) {
                rev |= ((code >> b) & 1) << (len - 1 - b);
            }
            for (int f = rev; f < (1 << FAST); f += 1 << len) {
                c->fast[f] = (uint16_t)(len << 12 | c->symbols[k]);
            }
        }
        code <<= 1;
    }
    return true;
}

static int read_symbol(struct bits *b, const struct code *c) {
    if (c->single >= 0) {
        return c->single;
    }
    if (b->count < MAX_LEN) {
        fill(b);
    }
    uint16_t e = c->fast[b->buf & ((1 << FAST) - 1)];
    if (e) {
        int len = e >> 12;
        b->buf >>= len;
        b->count -= len;
        return e & 0xFFF;
    }
    /* Longer code: walk the lengths one bit at a time (as in DEFLATE). */
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAX_LEN; len++) {
        code |= (int)(b->buf & 1);
        b->buf >>= 1;
        b->count--;
        int n = c->count[len];
        if (code - n < first) {
            return c->symbols[index + (code - first)];
        }
        index += n;
        first = (first + n) << 1;
        code <<= 1;
    }
    b->overrun = true;
    return 0;
}

static const uint8_t code_length_order[19] = { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

static bool read_code(struct bits *b, struct code *c, int alphabet) {
    uint8_t *lengths = calloc((size_t)alphabet, 1);
    if (!lengths) {
        return false;
    }
    bool ok = true;
    if (read_bits(b, 1)) {
        /* Simple: one or two symbols of length 1. */
        int symbols = (int)read_bits(b, 1) + 1;
        int s0 = (int)read_bits(b, read_bits(b, 1) ? 8 : 1);
        if (s0 < alphabet) {
            lengths[s0] = 1;
        } else {
            ok = false;
        }
        if (symbols == 2) {
            int s1 = (int)read_bits(b, 8);
            if (s1 < alphabet) {
                lengths[s1] = 1;
            } else {
                ok = false;
            }
        }
    } else {
        uint8_t cl_lengths[19] = { 0 };
        int n = (int)read_bits(b, 4) + 4;
        for (int i = 0; i < n; i++) {
            cl_lengths[code_length_order[i]] = (uint8_t)read_bits(b, 3);
        }
        struct code cl;
        memset(&cl, 0, sizeof(cl));
        ok = build_code(&cl, cl_lengths, 19);
        int max_symbol = alphabet;
        if (ok && read_bits(b, 1)) {
            int bits = 2 + 2 * (int)read_bits(b, 3);
            max_symbol = 2 + (int)read_bits(b, bits);
            ok = max_symbol <= alphabet;
        }
        int prev = 8;
        for (int sym = 0; ok && sym < alphabet;) {
            if (max_symbol-- == 0) {
                break;
            }
            int v = read_symbol(b, &cl);
            if (v < 16) {
                lengths[sym++] = (uint8_t)v;
                if (v) {
                    prev = v;
                }
                continue;
            }
            int repeat, value = 0;
            if (v == 16) {
                repeat = 3 + (int)read_bits(b, 2);
                value = prev;
            } else if (v == 17) {
                repeat = 3 + (int)read_bits(b, 3);
            } else {
                repeat = 11 + (int)read_bits(b, 7);
            }
            if (sym + repeat > alphabet) {
                ok = false;
                break;
            }
            memset(lengths + sym, value, (size_t)repeat);
            sym += repeat;
        }
        free_code(&cl);
    }
    ok = ok && build_code(c, lengths, alphabet) && !b->overrun;
    free(lengths);
    return ok;
}

/* --- the lossless image stream ---------------------------------------------------------------- */

enum { PREDICTOR, COLOR, SUBTRACT_GREEN, COLOR_INDEXING };

struct transform {
    int type, bits, xsize;   /* xsize: the image width this transform outputs */
    uint32_t *data;          /* sub-image or colour table */
    int count;               /* colour table entries */
};

struct group {
    struct code codes[5];    /* green+length+cache, red, blue, alpha, distance */
};

#define DIV_ROUND_UP(a, b) (((a) + (b) - 1) / (b))

static uint32_t *decode_stream(struct bits *b, int w, int h, bool top, const char **error);

static int prefix_value(struct bits *b, int prefix) {
    if (prefix < 4) {
        return prefix + 1;
    }
    int extra = (prefix - 2) >> 1;
    int offset = (2 + (prefix & 1)) << extra;
    return offset + (int)read_bits(b, extra) + 1;
}

/* The pixels themselves: literals, backward references and colour cache hits. */
static uint32_t *decode_pixels(struct bits *b, int w, int h, int cache_bits, uint32_t *entropy, int entropy_bits,
                               int entropy_w, struct group *groups, const char **error) {
    size_t total = (size_t)w * h;
    uint32_t *out = malloc(total * sizeof(uint32_t));
    uint32_t *cache = cache_bits ? calloc((size_t)1 << cache_bits, sizeof(uint32_t)) : NULL;
    if (!out || (cache_bits && !cache)) {
        free(out);
        free(cache);
        *error = "not enough memory";
        return NULL;
    }
    size_t pos = 0, cached = 0;
    int x = 0, y = 0;
    struct group *g = &groups[0];
    bool jumped = true;   /* a backward reference may have landed mid-tile */
    while (pos < total) {
        if (entropy && (jumped || (x & ((1 << entropy_bits) - 1)) == 0)) {
            g = &groups[(entropy[(y >> entropy_bits) * entropy_w + (x >> entropy_bits)] >> 8) & 0xFFFF];
        }
        int s = read_symbol(b, &g->codes[0]);
        size_t len = 1;
        if (s < 256) {
            uint32_t r = (uint32_t)read_symbol(b, &g->codes[1]);
            uint32_t bl = (uint32_t)read_symbol(b, &g->codes[2]);
            uint32_t a = (uint32_t)read_symbol(b, &g->codes[3]);
            out[pos] = a << 24 | r << 16 | (uint32_t)s << 8 | bl;
        } else if (s < 256 + 24) {
            len = (size_t)prefix_value(b, s - 256);
            int dsym = read_symbol(b, &g->codes[4]);
            int dcode = prefix_value(b, dsym);
            long dist;
            if (dcode > 120) {
                dist = dcode - 120;
            } else {
                dist = vp8l_distance_map[2 * (dcode - 1)] + vp8l_distance_map[2 * (dcode - 1) + 1] * (long)w;
                dist = dist < 1 ? 1 : dist;
            }
            if ((size_t)dist > pos || pos + len > total) {
                *error = "broken WebP backward reference";
                break;
            }
            for (size_t i = 0; i < len; i++) {
                out[pos + i] = out[pos + i - (size_t)dist];
            }
        } else {
            int index = s - 280;
            if (!cache || index >= (1 << cache_bits)) {
                *error = "broken WebP colour cache code";
                break;
            }
            out[pos] = cache[index];
        }
        /* Every pixel, however it came, goes into the colour cache. */
        pos += len;
        if (cache) {
            for (; cached < pos; cached++) {
                uint32_t c = out[cached];
                cache[(0x1E35A7BDu * c) >> (32 - cache_bits)] = c;
            }
        }
        jumped = len > 1;
        x += (int)len;
        while (x >= w) {
            x -= w;
            y++;
        }
        if (b->overrun) {
            *error = "truncated WebP data";
            break;
        }
    }
    free(cache);
    if (pos < total) {
        free(out);
        if (!*error) {
            *error = "broken WebP data";
        }
        return NULL;
    }
    return out;
}

/* Colour cache, prefix codes (and for the main image, the entropy image), then the pixels. */
static uint32_t *decode_image(struct bits *b, int w, int h, bool top, const char **error) {
    int cache_bits = 0;
    if (read_bits(b, 1)) {
        cache_bits = (int)read_bits(b, 4);
        if (cache_bits < 1 || cache_bits > 11) {
            *error = "bad WebP colour cache size";
            return NULL;
        }
    }
    uint32_t *entropy = NULL;
    int entropy_bits = 0, entropy_w = 0, ngroups = 1;
    if (top && read_bits(b, 1)) {
        entropy_bits = (int)read_bits(b, 3) + 2;
        entropy_w = DIV_ROUND_UP(w, 1 << entropy_bits);
        int eh = DIV_ROUND_UP(h, 1 << entropy_bits);
        entropy = decode_stream(b, entropy_w, eh, false, error);
        if (!entropy) {
            return NULL;
        }
        for (int i = 0; i < entropy_w * eh; i++) {
            int g = (int)((entropy[i] >> 8) & 0xFFFF);
            ngroups = g + 1 > ngroups ? g + 1 : ngroups;
        }
    }
    struct group *groups = calloc((size_t)ngroups, sizeof(struct group));
    if (!groups) {
        free(entropy);
        *error = "not enough memory";
        return NULL;
    }
    int alphabet[5] = { 256 + 24 + (cache_bits ? 1 << cache_bits : 0), 256, 256, 256, 40 };
    bool ok = true;
    for (int g = 0; g < ngroups && ok; g++) {
        for (int k = 0; k < 5 && ok; k++) {
            ok = read_code(b, &groups[g].codes[k], alphabet[k]);
        }
    }
    uint32_t *pixels = NULL;
    if (!ok) {
        *error = "broken WebP prefix code";
    } else {
        pixels = decode_pixels(b, w, h, cache_bits, entropy, entropy_bits, entropy_w, groups, error);
    }
    for (int g = 0; g < ngroups; g++) {
        for (int k = 0; k < 5; k++) {
            free_code(&groups[g].codes[k]);
        }
    }
    free(groups);
    free(entropy);
    return pixels;
}

/* --- the transforms, undone ------------------------------------------------------------------------ */

static uint32_t add_pixels(uint32_t a, uint32_t b) {
    uint32_t ag = (a & 0xFF00FF00u) + (b & 0xFF00FF00u), rb = (a & 0x00FF00FFu) + (b & 0x00FF00FFu);
    return (ag & 0xFF00FF00u) | (rb & 0x00FF00FFu);
}

static uint32_t average2(uint32_t a, uint32_t b) {
    return (((a ^ b) & 0xFEFEFEFEu) >> 1) + (a & b);
}

static int channel(uint32_t p, int shift) {
    return (int)((p >> shift) & 0xFF);
}

static uint32_t select_pred(uint32_t l, uint32_t t, uint32_t tl) {
    int pl = 0, pt = 0;
    for (int s = 0; s < 32; s += 8) {
        int p = channel(l, s) + channel(t, s) - channel(tl, s);
        pl += abs(p - channel(l, s));
        pt += abs(p - channel(t, s));
    }
    return pl < pt ? l : t;
}

static uint32_t clamp_add_sub_full(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8) {
        out |= (uint32_t)image_clamp255(channel(a, s) + channel(b, s) - channel(c, s)) << s;
    }
    return out;
}

static uint32_t clamp_add_sub_half(uint32_t a, uint32_t b) {
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8) {
        int x = channel(a, s), y = channel(b, s);
        out |= (uint32_t)image_clamp255(x + (x - y) / 2) << s;
    }
    return out;
}

static void undo_predictor(const struct transform *t, uint32_t *px, int w, int h) {
    int sw = DIV_ROUND_UP(w, 1 << t->bits);
    for (int y = 0; y < h; y++) {
        uint32_t *row = px + (size_t)y * w, *up = row - w;
        for (int x = 0; x < w; x++) {
            uint32_t pred;
            if (y == 0) {
                pred = x == 0 ? 0xFF000000u : row[x - 1];
            } else if (x == 0) {
                pred = up[0];
            } else {
                uint32_t L = row[x - 1], T = up[x], TL = up[x - 1];
                uint32_t TR = x == w - 1 ? row[0] : up[x + 1];   /* (the rightmost column: this row's first) */
                int mode = (int)((t->data[(y >> t->bits) * sw + (x >> t->bits)] >> 8) & 15);
                switch (mode) {
                case 1:  pred = L; break;
                case 2:  pred = T; break;
                case 3:  pred = TR; break;
                case 4:  pred = TL; break;
                case 5:  pred = average2(average2(L, TR), T); break;
                case 6:  pred = average2(L, TL); break;
                case 7:  pred = average2(L, T); break;
                case 8:  pred = average2(TL, T); break;
                case 9:  pred = average2(T, TR); break;
                case 10: pred = average2(average2(L, TL), average2(T, TR)); break;
                case 11: pred = select_pred(L, T, TL); break;
                case 12: pred = clamp_add_sub_full(L, T, TL); break;
                case 13: pred = clamp_add_sub_half(average2(L, T), TL); break;
                default: pred = 0xFF000000u; break;
                }
            }
            row[x] = add_pixels(row[x], pred);
        }
    }
}

static int delta(int t, int c) {
    return ((int)(int8_t)t * (int)(int8_t)c) >> 5;
}

static void undo_color(const struct transform *t, uint32_t *px, int w, int h) {
    int sw = DIV_ROUND_UP(w, 1 << t->bits);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t cte = t->data[(y >> t->bits) * sw + (x >> t->bits)];
            int g2r = (int)(cte & 0xFF), g2b = (int)((cte >> 8) & 0xFF), r2b = (int)((cte >> 16) & 0xFF);
            uint32_t p = px[(size_t)y * w + x];
            int green = channel(p, 8), red = channel(p, 16), blue = channel(p, 0);
            red = (red + delta(g2r, green)) & 0xFF;
            blue = (blue + delta(g2b, green)) & 0xFF;
            blue = (blue + delta(r2b, red)) & 0xFF;
            px[(size_t)y * w + x] = (p & 0xFF00FF00u) | (uint32_t)red << 16 | (uint32_t)blue;
        }
    }
}

static void undo_subtract_green(uint32_t *px, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t g = (px[i] >> 8) & 0xFF;
        px[i] = (px[i] & 0xFF00FF00u) | (((px[i] >> 16) + g) & 0xFF) << 16 | ((px[i] + g) & 0xFF);
    }
}

/* Indices -> colours, unbundling pixels packed several to a byte. In place, back to front. */
static void undo_color_indexing(const struct transform *t, uint32_t *px, int w, int h) {
    int bits = t->count > 16 ? 0 : t->count > 4 ? 1 : t->count > 2 ? 2 : 3;
    int in_w = DIV_ROUND_UP(w, 1 << bits), per = 1 << bits, size = 8 >> bits, mask = (1 << size) - 1;
    for (int y = h - 1; y >= 0; y--) {
        for (int x = w - 1; x >= 0; x--) {
            uint32_t packed = px[(size_t)y * in_w + (x >> bits)];
            int index = (int)((packed >> 8) & 0xFF);
            if (bits) {
                index = (index >> ((x & (per - 1)) * size)) & mask;
            }
            px[(size_t)y * w + x] = index < t->count ? t->data[index] : 0;
        }
    }
}

/* An image stream: (for the main image) transforms, then the coded pixels; transforms undone. */
static uint32_t *decode_stream(struct bits *b, int w, int h, bool top, const char **error) {
    struct transform transforms[4];
    int nt = 0, xsize = w;
    uint32_t *pixels = NULL;
    bool seen[4] = { false };
    while (top && read_bits(b, 1)) {
        int type = (int)read_bits(b, 2);
        if (nt == 4 || seen[type]) {
            *error = "bad WebP transforms";
            goto fail;
        }
        seen[type] = true;
        struct transform *t = &transforms[nt];
        memset(t, 0, sizeof(*t));
        t->type = type;
        t->xsize = xsize;
        if (type == PREDICTOR || type == COLOR) {
            t->bits = (int)read_bits(b, 3) + 2;
            t->data = decode_stream(b, DIV_ROUND_UP(xsize, 1 << t->bits), DIV_ROUND_UP(h, 1 << t->bits), false,
                                    error);
            if (!t->data) {
                goto fail;
            }
        } else if (type == COLOR_INDEXING) {
            t->count = (int)read_bits(b, 8) + 1;
            uint32_t *table = decode_stream(b, t->count, 1, false, error);
            if (!table) {
                goto fail;
            }
            /* The table is delta-coded: each entry adds to the one before. */
            t->data = calloc(256, sizeof(uint32_t));
            if (!t->data) {
                free(table);
                *error = "not enough memory";
                goto fail;
            }
            t->data[0] = table[0];
            for (int i = 1; i < t->count; i++) {
                t->data[i] = add_pixels(table[i], t->data[i - 1]);
            }
            free(table);
            int bits = t->count > 16 ? 0 : t->count > 4 ? 1 : t->count > 2 ? 2 : 3;
            xsize = DIV_ROUND_UP(xsize, 1 << bits);
        }
        nt++;
    }
    uint32_t *coded = decode_image(b, xsize, h, top, error);
    if (!coded) {
        goto fail;
    }
    /* Room for the full width (colour indexing makes it wider). */
    if (xsize != w) {
        pixels = realloc(coded, (size_t)w * h * sizeof(uint32_t));
        if (!pixels) {
            free(coded);
            *error = "not enough memory";
            goto fail;
        }
    } else {
        pixels = coded;
    }
    for (int i = nt - 1; i >= 0; i--) {
        struct transform *t = &transforms[i];
        switch (t->type) {
        case PREDICTOR:      undo_predictor(t, pixels, t->xsize, h); break;
        case COLOR:          undo_color(t, pixels, t->xsize, h); break;
        case SUBTRACT_GREEN: undo_subtract_green(pixels, (size_t)t->xsize * h); break;
        default:             undo_color_indexing(t, pixels, t->xsize, h); break;
        }
    }
    for (int i = 0; i < nt; i++) {
        free(transforms[i].data);
    }
    return pixels;

fail:
    for (int i = 0; i < nt; i++) {
        free(transforms[i].data);
    }
    return NULL;
}

static struct gfx_surface *argb_to_surface(const uint32_t *px, int w, int h) {
    struct gfx_surface *s = gfx_surface_create(w, h);
    if (!s) {
        return NULL;
    }
    for (int y = 0; y < h; y++) {
        uint32_t *row = gfx_row(s, y);
        for (int x = 0; x < w; x++) {
            uint32_t p = px[(size_t)y * w + x];
            row[x] = image_pixel((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF, p >> 24);
        }
    }
    return s;
}

static struct gfx_surface *decode_vp8l(const uint8_t *data, size_t size, const char **error) {
    if (size < 5 || data[0] != 0x2F) {
        return image_fail(error, "not a lossless WebP");
    }
    struct bits b = { data + 1, size - 1, 0, 0, 0, false };
    int w = (int)read_bits(&b, 14) + 1, h = (int)read_bits(&b, 14) + 1;
    read_bits(&b, 1);   /* alpha used: a hint only */
    if (read_bits(&b, 3) != 0) {
        return image_fail(error, "unknown lossless WebP version");
    }
    if (!image_size_ok((uint32_t)w, (uint32_t)h)) {
        return image_fail(error, "picture too large");
    }
    const char *why = NULL;
    uint32_t *px = decode_stream(&b, w, h, true, &why);
    if (!px) {
        return image_fail(error, why ? why : "broken lossless WebP");
    }
    struct gfx_surface *s = argb_to_surface(px, w, h);
    free(px);
    return s ? s : image_fail(error, "not enough memory");
}

/* --- alpha for lossy pictures --------------------------------------------------------------------------- */

static bool apply_alpha(struct gfx_surface *s, const uint8_t *data, size_t size) {
    if (size < 1) {
        return false;
    }
    int w = s->width, h = s->height;
    int filter = (data[0] >> 2) & 3, method = data[0] & 3;
    uint8_t *alpha = malloc((size_t)w * h);
    if (!alpha) {
        return false;
    }
    if (method == 0) {
        if (size - 1 < (size_t)w * h) {
            free(alpha);
            return false;
        }
        memcpy(alpha, data + 1, (size_t)w * h);
    } else {
        /* A lossless stream without header; the alpha is in the green channel. */
        struct bits b = { data + 1, size - 1, 0, 0, 0, false };
        const char *why = NULL;
        uint32_t *px = decode_stream(&b, w, h, true, &why);
        if (!px) {
            free(alpha);
            return false;
        }
        for (size_t i = 0; i < (size_t)w * h; i++) {
            alpha[i] = (uint8_t)(px[i] >> 8);
        }
        free(px);
    }
    /* Undo the filtering: each value was coded relative to a neighbour. */
    for (int y = 0; y < h && filter; y++) {
        uint8_t *row = alpha + (size_t)y * w, *up = row - w;
        for (int x = 0; x < w; x++) {
            int pred;
            if (x == 0 && y == 0) {
                pred = 0;
            } else if (y == 0) {
                pred = row[x - 1];
            } else if (x == 0) {
                pred = up[0];
            } else if (filter == 1) {
                pred = row[x - 1];
            } else if (filter == 2) {
                pred = up[x];
            } else {
                pred = image_clamp255(row[x - 1] + up[x] - up[x - 1]);
            }
            row[x] = (uint8_t)(row[x] + pred);
        }
    }
    for (int y = 0; y < h; y++) {
        uint32_t *row = gfx_row(s, y);
        for (int x = 0; x < w; x++) {
            uint32_t p = row[x];
            row[x] = image_pixel((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF, alpha[(size_t)y * w + x]);
        }
    }
    free(alpha);
    return true;
}

/* --- the container ------------------------------------------------------------------------------------- */

struct chunk {
    const uint8_t *data;
    size_t size;
};

/* Find the frame's pieces among the chunks in [p, end). */
static void find_chunks(const uint8_t *p, const uint8_t *end, struct chunk *vp8, struct chunk *vp8l,
                        struct chunk *alph, struct chunk *anmf) {
    while (end - p >= 8) {
        uint32_t size = image_le32(p + 4);
        const uint8_t *body = p + 8;
        if (size > (size_t)(end - body)) {
            size = (uint32_t)(end - body);
        }
        if (!memcmp(p, "VP8 ", 4) && !vp8->data) {
            *vp8 = (struct chunk){ body, size };
        } else if (!memcmp(p, "VP8L", 4) && !vp8l->data) {
            *vp8l = (struct chunk){ body, size };
        } else if (!memcmp(p, "ALPH", 4) && !alph->data) {
            *alph = (struct chunk){ body, size };
        } else if (anmf && !memcmp(p, "ANMF", 4) && !anmf->data) {
            *anmf = (struct chunk){ body, size };
        }
        p = body + size + (size & 1);
    }
}

static struct gfx_surface *decode_frame(struct chunk vp8, struct chunk vp8l, struct chunk alph, const char **error) {
    if (vp8l.data) {
        return decode_vp8l(vp8l.data, vp8l.size, error);
    }
    if (!vp8.data) {
        return image_fail(error, "no picture in the WebP file");
    }
    struct gfx_surface *s = vp8_decode(vp8.data, vp8.size, error);
    if (s && alph.data) {
        apply_alpha(s, alph.data, alph.size);   /* broken alpha: show it opaque */
    }
    return s;
}

struct gfx_surface *image_decode_webp(const uint8_t *d, size_t n, const char **error) {
    if (n < 20) {
        return image_fail(error, "truncated WebP");
    }
    uint32_t riff = image_le32(d + 4);
    const uint8_t *end = d + 8 + (riff < n - 8 ? riff : n - 8);
    struct chunk vp8 = { 0 }, vp8l = { 0 }, alph = { 0 }, anmf = { 0 };
    find_chunks(d + 12, end, &vp8, &vp8l, &alph, &anmf);
    if (vp8.data || vp8l.data) {
        return decode_frame(vp8, vp8l, alph, error);
    }
    if (!anmf.data || anmf.size < 16) {
        return image_fail(error, "no picture in the WebP file");
    }
    /* Animated: the first frame, at its place on the canvas (from VP8X). */
    if (memcmp(d + 12, "VP8X", 4) != 0) {
        return image_fail(error, "broken animated WebP");
    }
    int cw = (int)((d[24] | d[25] << 8 | d[26] << 16) + 1), ch = (int)((d[27] | d[28] << 8 | d[29] << 16) + 1);
    int fx = 2 * (int)(anmf.data[0] | anmf.data[1] << 8 | anmf.data[2] << 16);
    int fy = 2 * (int)(anmf.data[3] | anmf.data[4] << 8 | anmf.data[5] << 16);
    struct chunk fvp8 = { 0 }, fvp8l = { 0 }, falph = { 0 };
    find_chunks(anmf.data + 16, anmf.data + anmf.size, &fvp8, &fvp8l, &falph, NULL);
    struct gfx_surface *frame = decode_frame(fvp8, fvp8l, falph, error);
    if (!frame || !image_size_ok((uint32_t)cw, (uint32_t)ch)) {
        gfx_surface_destroy(frame);
        return frame ? image_fail(error, "bad WebP canvas") : NULL;
    }
    struct gfx_surface *canvas = gfx_surface_create(cw, ch);
    if (!canvas) {
        gfx_surface_destroy(frame);
        return image_fail(error, "not enough memory");
    }
    gfx_copy(canvas, fx, fy, frame, NULL);
    gfx_surface_destroy(frame);
    return canvas;
}
