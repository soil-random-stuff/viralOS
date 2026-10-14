/*
 * A TrueType font reader and renderer.
 *
 * A .ttf file is a directory of tables. The ones used here:
 *   head   units per em (the design grid), offset format of loca
 *   maxp   number of glyphs
 *   hhea   ascent/descent/line gap, number of horizontal metrics
 *   hmtx   advance width of each glyph
 *   cmap   character -> glyph number
 *   loca   where each glyph's outline is in glyf
 *   glyf   the outlines: contours of points, on or off the curve
 *          (off-curve points are quadratic Bezier control points)
 *   kern   pair adjustments, e.g. pulling "A" and "V" together
 *   name   the family name
 * All numbers are big-endian. Every read is bounds-checked: a broken file
 * gives wrong shapes, never a crash.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ttf.h>
#include <unistd.h>

#define CACHE_SIZE  512
#define MAX_POINTS  4096
#define MAX_DEPTH   8      /* composite glyphs nesting */

struct cached_glyph {
    bool used;
    uint32_t glyph;
    uint32_t size_q;       /* size in quarter pixels */
    int xoff, yoff;        /* bitmap's top-left relative to the pen on the baseline */
    struct gfx_alpha mask; /* NULL data for empty glyphs (space) */
};

struct ttf_font {
    uint8_t *data;
    uint32_t size;
    uint32_t glyf, loca, hmtx, cmap4, cmap12, kern_pairs;
    uint32_t kern_count;
    uint16_t units_per_em, num_glyphs, num_hmetrics;
    int16_t loca_format, ascent, descent, line_gap;
    bool kerning;
    char family[64];
    struct cached_glyph cache[CACHE_SIZE];
};

/* --- reading the file --------------------------------------------------------------- */

static uint32_t u8(const struct ttf_font *f, uint32_t off) {
    return off < f->size ? f->data[off] : 0;
}

static uint32_t u16(const struct ttf_font *f, uint32_t off) {
    return off + 2 <= f->size ? (uint32_t)f->data[off] << 8 | f->data[off + 1] : 0;
}

static int32_t i16(const struct ttf_font *f, uint32_t off) {
    return (int16_t)u16(f, off);
}

static uint32_t u32(const struct ttf_font *f, uint32_t off) {
    return off + 4 <= f->size ? u16(f, off) << 16 | u16(f, off + 2) : 0;
}

/* 2.14 fixed point, used for scales in composite glyphs. */
static float f2dot14(const struct ttf_font *f, uint32_t off) {
    return (float)i16(f, off) / 16384.0f;
}

static uint32_t find_table(const struct ttf_font *f, const char *tag, uint32_t *length) {
    uint32_t count = u16(f, 4);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t rec = 12 + 16 * i;
        if (rec + 16 <= f->size && memcmp(f->data + rec, tag, 4) == 0) {
            if (length) {
                *length = u32(f, rec + 12);
            }
            return u32(f, rec + 8);
        }
    }
    return 0;
}

static void read_family_name(struct ttf_font *f) {
    strlcpy(f->family, "?", sizeof(f->family));
    uint32_t name = find_table(f, "name", NULL);
    if (!name) {
        return;
    }
    uint32_t count = u16(f, name + 2), strings = name + u16(f, name + 4);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t rec = name + 6 + 12 * i;
        uint32_t platform = u16(f, rec), name_id = u16(f, rec + 6);
        uint32_t len = u16(f, rec + 8), off = strings + u16(f, rec + 10);
        if (name_id != 1 || (platform != 3 && platform != 0)) {
            continue;
        }
        /* UTF-16BE: keep the ASCII characters. */
        size_t n = 0;
        for (uint32_t j = 0; j + 1 < len && n < sizeof(f->family) - 1; j += 2) {
            uint32_t c = u16(f, off + j);
            f->family[n++] = c < 128 ? (char)c : '?';
        }
        f->family[n] = '\0';
        return;
    }
}

/* Pick the best character map: format 12 (all of Unicode) or 4 (the basic plane). */
static void find_cmaps(struct ttf_font *f) {
    uint32_t cmap = find_table(f, "cmap", NULL);
    uint32_t count = u16(f, cmap + 2);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t rec = cmap + 4 + 8 * i;
        uint32_t platform = u16(f, rec), encoding = u16(f, rec + 2);
        uint32_t sub = cmap + u32(f, rec + 4);
        bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        if (!unicode) {
            continue;
        }
        uint32_t format = u16(f, sub);
        if (format == 4 && !f->cmap4) {
            f->cmap4 = sub;
        } else if (format == 12 && !f->cmap12) {
            f->cmap12 = sub;
        }
    }
}

static void find_kerning(struct ttf_font *f) {
    uint32_t kern = find_table(f, "kern", NULL);
    if (!kern || u16(f, kern) != 0 || u16(f, kern + 2) == 0) {
        return;
    }
    uint32_t sub = kern + 4;
    uint32_t coverage = u16(f, sub + 4);
    if ((coverage >> 8) != 0 || !(coverage & 1)) {
        return;   /* only format 0, horizontal kerning */
    }
    f->kern_count = u16(f, sub + 6);
    f->kern_pairs = sub + 14;
}

struct ttf_font *ttf_load(const char *path) {
    struct os_file_info info;
    if (stat(path, &info) < 0 || info.type != OS_FILE || info.size < 12) {
        return NULL;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    struct ttf_font *f = calloc(1, sizeof(*f));
    uint8_t *data = malloc(info.size);
    uint64_t got = 0;
    while (f && data && got < info.size) {
        long n = read(fd, data + got, info.size - got);
        if (n <= 0) {
            break;
        }
        got += (uint64_t)n;
    }
    close(fd);
    if (!f || !data || got != info.size) {
        free(f);
        free(data);
        return NULL;
    }
    f->data = data;
    f->size = (uint32_t)info.size;

    uint32_t version = u32(f, 0);
    uint32_t head = find_table(f, "head", NULL), maxp = find_table(f, "maxp", NULL);
    uint32_t hhea = find_table(f, "hhea", NULL);
    f->hmtx = find_table(f, "hmtx", NULL);
    f->loca = find_table(f, "loca", NULL);
    f->glyf = find_table(f, "glyf", NULL);
    find_cmaps(f);
    if ((version != 0x00010000 && version != 0x74727565 /* 'true' */) || !head || !maxp ||
        !hhea || !f->hmtx || !f->loca || !f->glyf || (!f->cmap4 && !f->cmap12)) {
        ttf_free(f);
        return NULL;
    }
    f->units_per_em = (uint16_t)u16(f, head + 18);
    f->loca_format = (int16_t)i16(f, head + 50);
    f->num_glyphs = (uint16_t)u16(f, maxp + 4);
    f->ascent = (int16_t)i16(f, hhea + 4);
    f->descent = (int16_t)i16(f, hhea + 6);
    f->line_gap = (int16_t)i16(f, hhea + 8);
    f->num_hmetrics = (uint16_t)u16(f, hhea + 34);
    if (f->units_per_em == 0 || f->num_hmetrics == 0) {
        ttf_free(f);
        return NULL;
    }
    find_kerning(f);
    read_family_name(f);
    f->kerning = true;
    return f;
}

void ttf_free(struct ttf_font *f) {
    if (!f) {
        return;
    }
    for (int i = 0; i < CACHE_SIZE; i++) {
        free(f->cache[i].mask.data);
    }
    free(f->data);
    free(f);
}

/* --- characters, advances, kerning ---------------------------------------------------- */

static uint32_t glyph_index(const struct ttf_font *f, uint32_t cp) {
    if (f->cmap12) {
        uint32_t groups = u32(f, f->cmap12 + 12);
        uint32_t lo = 0, hi = groups;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2, g = f->cmap12 + 16 + 12 * mid;
            if (cp < u32(f, g)) {
                hi = mid;
            } else if (cp > u32(f, g + 4)) {
                lo = mid + 1;
            } else {
                return u32(f, g + 8) + (cp - u32(f, g));
            }
        }
        return 0;
    }
    if (cp > 0xFFFF) {
        return 0;
    }
    /* Format 4: segments of consecutive characters, found by their end code. */
    uint32_t sub = f->cmap4;
    uint32_t seg2 = u16(f, sub + 6);
    uint32_t ends = sub + 14, starts = ends + seg2 + 2;
    uint32_t deltas = starts + seg2, ranges = deltas + seg2;
    for (uint32_t i = 0; i < seg2; i += 2) {
        if (u16(f, ends + i) < cp) {
            continue;
        }
        uint32_t start = u16(f, starts + i);
        if (cp < start) {
            return 0;
        }
        uint32_t delta = u16(f, deltas + i), range = u16(f, ranges + i);
        if (range == 0) {
            return (cp + delta) & 0xFFFF;
        }
        uint32_t g = u16(f, ranges + i + range + 2 * (cp - start));
        return g ? (g + delta) & 0xFFFF : 0;
    }
    return 0;
}

static float advance(const struct ttf_font *f, uint32_t glyph) {
    uint32_t i = glyph < f->num_hmetrics ? glyph : f->num_hmetrics - 1u;
    return (float)u16(f, f->hmtx + 4 * i);
}

static float kern(const struct ttf_font *f, uint32_t left, uint32_t right) {
    if (!f->kerning || !f->kern_count) {
        return 0;
    }
    uint32_t key = left << 16 | right;
    uint32_t lo = 0, hi = f->kern_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, pair = f->kern_pairs + 6 * mid;
        uint32_t k = u32(f, pair);
        if (key < k) {
            hi = mid;
        } else if (key > k) {
            lo = mid + 1;
        } else {
            return (float)i16(f, pair + 4);
        }
    }
    return 0;
}

/* --- outlines ------------------------------------------------------------------------ */

struct point {
    float x, y;
    bool on_curve;
};

struct outline {
    struct point *points;
    int count;
    int *ends;          /* index of each contour's last point */
    int contours;
};

static bool outline_reserve(struct outline *o, int points, int contours) {
    if (o->count + points > MAX_POINTS) {
        return false;
    }
    struct point *p = realloc(o->points, sizeof(*p) * (size_t)(o->count + points));
    int *e = realloc(o->ends, sizeof(*e) * (size_t)(o->contours + contours));
    if (p) {
        o->points = p;
    }
    if (e) {
        o->ends = e;
    }
    return p && e;
}

static uint32_t glyph_offset(const struct ttf_font *f, uint32_t glyph, uint32_t *length) {
    if (glyph >= f->num_glyphs) {
        return 0;
    }
    uint32_t a, b;
    if (f->loca_format == 0) {
        a = u16(f, f->loca + 2 * glyph) * 2;
        b = u16(f, f->loca + 2 * glyph + 2) * 2;
    } else {
        a = u32(f, f->loca + 4 * glyph);
        b = u32(f, f->loca + 4 * glyph + 4);
    }
    *length = b > a ? b - a : 0;
    return f->glyf + a;
}

/*
 * Append glyph `glyph`'s points to `o`, transformed by the 2x3 matrix
 * m = { a, b, c, d, e, f }: x' = a*x + c*y + e, y' = b*x + d*y + f.
 */
static bool load_outline(const struct ttf_font *f, uint32_t glyph, struct outline *o,
                         const float m[6], int depth) {
    uint32_t length;
    uint32_t off = glyph_offset(f, glyph, &length);
    if (!length) {
        return true;   /* an empty glyph, like space */
    }
    int contours = i16(f, off);

    if (contours >= 0) {
        int n = (int)u16(f, off + 10 + 2 * (uint32_t)(contours - 1)) + 1;
        if (contours == 0 || n <= 0 || !outline_reserve(o, n, contours)) {
            return contours == 0;
        }
        int base = o->count;
        for (int i = 0; i < contours; i++) {
            o->ends[o->contours + i] = base + (int)u16(f, off + 10 + 2 * (uint32_t)i);
        }
        uint32_t p = off + 10 + 2 * (uint32_t)contours;
        p += 2 + u16(f, p);   /* skip the hinting instructions */

        /* Flags, with "repeat this flag N more times" compression. */
        uint8_t *flags = malloc((size_t)n);
        if (!flags) {
            return false;
        }
        for (int i = 0; i < n;) {
            uint8_t fl = (uint8_t)u8(f, p++);
            flags[i++] = fl;
            if (fl & 8) {
                for (uint32_t r = u8(f, p++); r > 0 && i < n; r--) {
                    flags[i++] = fl;
                }
            }
        }
        /* Coordinates: deltas, each 1 byte (bit 1/2 set) or 2 bytes, or "same as before". */
        int32_t x = 0, y = 0;
        for (int i = 0; i < n; i++) {
            if (flags[i] & 2) {
                int32_t d = (int32_t)u8(f, p++);
                x += (flags[i] & 16) ? d : -d;
            } else if (!(flags[i] & 16)) {
                x += i16(f, p);
                p += 2;
            }
            o->points[base + i].x = (float)x;
        }
        for (int i = 0; i < n; i++) {
            if (flags[i] & 4) {
                int32_t d = (int32_t)u8(f, p++);
                y += (flags[i] & 32) ? d : -d;
            } else if (!(flags[i] & 32)) {
                y += i16(f, p);
                p += 2;
            }
            struct point *pt = &o->points[base + i];
            float px = pt->x, py = (float)y;
            pt->x = m[0] * px + m[2] * py + m[4];
            pt->y = m[1] * px + m[3] * py + m[5];
            pt->on_curve = flags[i] & 1;
        }
        free(flags);
        o->count += n;
        o->contours += contours;
        return true;
    }

    /* A composite glyph: other glyphs, each moved and possibly scaled. */
    if (depth >= MAX_DEPTH) {
        return false;
    }
    uint32_t p = off + 10;
    for (;;) {
        uint32_t flags = u16(f, p), component = u16(f, p + 2);
        p += 4;
        float dx, dy;
        if (flags & 1) {   /* ARG_1_AND_2_ARE_WORDS */
            dx = (float)i16(f, p);
            dy = (float)i16(f, p + 2);
            p += 4;
        } else {
            dx = (float)(int8_t)u8(f, p);
            dy = (float)(int8_t)u8(f, p + 1);
            p += 2;
        }
        if (!(flags & 2)) {
            dx = dy = 0;   /* aligning by point numbers isn't supported */
        }
        float a = 1, b = 0, c = 0, d = 1;
        if (flags & 8) {            /* one scale */
            a = d = f2dot14(f, p);
            p += 2;
        } else if (flags & 0x40) {  /* separate x and y scales */
            a = f2dot14(f, p);
            d = f2dot14(f, p + 2);
            p += 4;
        } else if (flags & 0x80) {  /* a full 2x2 matrix */
            a = f2dot14(f, p);
            b = f2dot14(f, p + 2);
            c = f2dot14(f, p + 4);
            d = f2dot14(f, p + 6);
            p += 8;
        }
        /* Apply the component's transform, then ours. */
        float cm[6] = {
            m[0] * a + m[2] * b, m[1] * a + m[3] * b,
            m[0] * c + m[2] * d, m[1] * c + m[3] * d,
            m[0] * dx + m[2] * dy + m[4], m[1] * dx + m[3] * dy + m[5],
        };
        if (!load_outline(f, component, o, cm, depth + 1)) {
            return false;
        }
        if (!(flags & 0x20)) {   /* MORE_COMPONENTS */
            return true;
        }
    }
}

/* --- rendering a glyph -------------------------------------------------------------- */

static struct point midpoint(struct point a, struct point b) {
    return (struct point){ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, true };
}

/*
 * Walk one contour: on-curve points are joined by lines, off-curve points
 * are control points, and two off-curve points in a row imply an on-curve
 * point halfway between them.
 */
static void trace_contour(struct gfx_raster *r, const struct point *pts, int n, float ox, float oy) {
    if (n < 2) {
        return;
    }
    int first_on = -1;
    for (int i = 0; i < n; i++) {
        if (pts[i].on_curve) {
            first_on = i;
            break;
        }
    }
    struct point start;
    int from;   /* first point to visit after `start` */
    if (first_on >= 0) {
        start = pts[first_on];
        from = first_on + 1;
    } else {
        start = midpoint(pts[n - 1], pts[0]);
        from = 0;
    }
    int visits = first_on >= 0 ? n - 1 : n;

    struct point cur = start, ctrl = { 0, 0, false };
    bool have_ctrl = false;
    for (int k = 0; k < visits; k++) {
        struct point q = pts[(from + k) % n];
        if (q.on_curve) {
            if (have_ctrl) {
                gfx_raster_quad(r, cur.x - ox, cur.y - oy, ctrl.x - ox, ctrl.y - oy, q.x - ox, q.y - oy);
            } else {
                gfx_raster_line(r, cur.x - ox, cur.y - oy, q.x - ox, q.y - oy);
            }
            cur = q;
            have_ctrl = false;
        } else {
            if (have_ctrl) {
                struct point mid = midpoint(ctrl, q);
                gfx_raster_quad(r, cur.x - ox, cur.y - oy, ctrl.x - ox, ctrl.y - oy, mid.x - ox, mid.y - oy);
                cur = mid;
            }
            ctrl = q;
            have_ctrl = true;
        }
    }
    if (have_ctrl) {
        gfx_raster_quad(r, cur.x - ox, cur.y - oy, ctrl.x - ox, ctrl.y - oy, start.x - ox, start.y - oy);
    } else {
        gfx_raster_line(r, cur.x - ox, cur.y - oy, start.x - ox, start.y - oy);
    }
}

/*
 * Unhinted glyphs come out a little light at small sizes. A mild gamma
 * boost of the partial coverage (about 1.25) evens that out.
 */
static uint8_t boost(uint8_t c) {
    float v = (float)c;
    return (uint8_t)(0.6f * v + 0.4f * sqrtf(v * 255.0f) + 0.5f);
}

static bool render_glyph(struct ttf_font *f, uint32_t glyph, float size, struct cached_glyph *out) {
    float scale = size / (float)f->units_per_em;
    float m[6] = { scale, 0, 0, -scale, 0, 0 };   /* font units -> pixels, y pointing down */
    struct outline o = { 0 };
    out->mask = (struct gfx_alpha){ NULL, 0, 0 };
    out->xoff = out->yoff = 0;

    bool ok = load_outline(f, glyph, &o, m, 0);
    if (!ok || o.count == 0) {
        free(o.points);
        free(o.ends);
        return ok;
    }
    float minx = o.points[0].x, maxx = minx, miny = o.points[0].y, maxy = miny;
    for (int i = 1; i < o.count; i++) {
        minx = fminf(minx, o.points[i].x);
        maxx = fmaxf(maxx, o.points[i].x);
        miny = fminf(miny, o.points[i].y);
        maxy = fmaxf(maxy, o.points[i].y);
    }
    int x0 = (int)floorf(minx) - 1, y0 = (int)floorf(miny) - 1;
    int w = (int)ceilf(maxx) - x0 + 2, h = (int)ceilf(maxy) - y0 + 2;

    struct gfx_raster r;
    uint8_t *bitmap = malloc((size_t)w * (size_t)h);
    if (bitmap && gfx_raster_init(&r, w, h)) {
        int start = 0;
        for (int c = 0; c < o.contours; c++) {
            int end = o.ends[c];
            if (end >= start && end < o.count) {
                trace_contour(&r, o.points + start, end - start + 1, (float)x0, (float)y0);
            }
            start = end + 1;
        }
        gfx_raster_coverage(&r, bitmap);
        gfx_raster_free(&r);
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
            bitmap[i] = boost(bitmap[i]);
        }
        out->mask = (struct gfx_alpha){ bitmap, w, h };
        out->xoff = x0;
        out->yoff = y0;
    } else {
        free(bitmap);
        ok = false;
    }
    free(o.points);
    free(o.ends);
    return ok;
}

static struct cached_glyph *get_glyph(struct ttf_font *f, uint32_t glyph, float size) {
    uint32_t size_q = (uint32_t)(size * 4 + 0.5f);
    uint32_t home = (glyph * 2654435761u ^ size_q * 40503u) % CACHE_SIZE;
    for (uint32_t i = 0; i < 8; i++) {
        struct cached_glyph *c = &f->cache[(home + i) % CACHE_SIZE];
        if (c->used && c->glyph == glyph && c->size_q == size_q) {
            return c;
        }
        if (!c->used) {
            home = (home + i) % CACHE_SIZE;
            break;
        }
    }
    /* Not cached: render into the free slot found, or evict the home slot. */
    struct cached_glyph *c = &f->cache[home];
    free(c->mask.data);
    c->used = render_glyph(f, glyph, size_q / 4.0f, c);
    c->glyph = glyph;
    c->size_q = size_q;
    return c->used ? c : NULL;
}

/* --- text ----------------------------------------------------------------------------- */

static uint32_t next_codepoint(const char **text) {
    const uint8_t *s = (const uint8_t *)*text;
    uint32_t c = *s++;
    int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0) {
        *text = (const char *)s;
        return 0xFFFD;
    }
    c &= extra == 0 ? 0x7F : extra == 1 ? 0x1F : extra == 2 ? 0x0F : 0x07;
    while (extra--) {
        if ((*s & 0xC0) != 0x80) {
            *text = (const char *)s;
            return 0xFFFD;
        }
        c = c << 6 | (*s++ & 0x3F);
    }
    *text = (const char *)s;
    return c;
}

float ttf_draw_text(struct gfx_surface *s, struct ttf_font *f, float size, float x, float y,
                    gfx_color color, const char *text) {
    float scale = size / (float)f->units_per_em;
    int baseline = (int)floorf(y + 0.5f);
    uint32_t prev = 0;
    while (*text) {
        uint32_t glyph = glyph_index(f, next_codepoint(&text));
        if (prev) {
            x += kern(f, prev, glyph) * scale;
        }
        if (s) {
            struct cached_glyph *c = get_glyph(f, glyph, size);
            if (c && c->mask.data) {
                gfx_fill_mask(s, (int)floorf(x + 0.5f) + c->xoff, baseline + c->yoff, &c->mask, color);
            }
        }
        x += advance(f, glyph) * scale;
        prev = glyph;
    }
    return x;
}

float ttf_text_width(struct ttf_font *f, float size, const char *text) {
    return ttf_draw_text(NULL, f, size, 0, 0, 0, text);
}

void ttf_get_metrics(struct ttf_font *f, float size, struct ttf_metrics *out) {
    float scale = size / (float)f->units_per_em;
    out->ascent = (float)f->ascent * scale;
    out->descent = -(float)f->descent * scale;
    out->line_height = (float)(f->ascent - f->descent + f->line_gap) * scale;
}

void ttf_set_kerning(struct ttf_font *f, bool enabled) {
    f->kerning = enabled;
}

const char *ttf_family_name(struct ttf_font *f) {
    return f->family;
}

uint32_t ttf_glyph_count(struct ttf_font *f) {
    return f->num_glyphs;
}

uint32_t ttf_kerning_pairs(struct ttf_font *f) {
    return f->kern_count;
}
