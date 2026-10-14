/*
 * Framebuffer text console using a PC Screen Font (PSF1 or PSF2) linked into
 * the kernel. Only 32 bpp framebuffers are supported for now.
 */

#include <stddef.h>

#include <limine.h>

#include "drivers/console.h"
#include "arch/x86_64/cpu.h"
#include "lib/string.h"

extern const uint8_t _binary_terminus16_psf_start[];
extern const uint8_t _binary_terminus16_psf_end[];

#define PSF1_MAGIC0     0x36
#define PSF1_MAGIC1     0x04
#define PSF1_MODE512    0x01
#define PSF1_MODEHASTAB 0x02
#define PSF1_MODESEQ    0x04

#define PSF2_MAGIC          0x864ab572
#define PSF2_HAS_UNICODE    0x01

struct psf2_header {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t flags;
    uint32_t glyph_count;
    uint32_t bytes_per_glyph;
    uint32_t height;
    uint32_t width;
};

#define NO_GLYPH 0xFFFF

static void cursor_hide(void);
static void cursor_show(void);

static struct {
    bool ready;
    bool suspended;   /* a program owns the screen */

    uint8_t *fb;
    uint64_t pitch;
    uint32_t fb_width, fb_height;
    uint8_t r_shift, g_shift, b_shift;

    const uint8_t *glyphs;
    uint32_t glyph_count, glyph_w, glyph_h, bytes_per_glyph, bytes_per_row;
    uint16_t map[256]; /* Latin-1 code point -> glyph index */

    uint32_t cols, rows, cx, cy;
    uint32_t fg, bg;
} con;

static uint32_t pack_rgb(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return (r << con.r_shift) | (g << con.g_shift) | (b << con.b_shift);
}

/* Decode one UTF-8 code point; returns NULL on malformed input. */
static const uint8_t *utf8_decode(const uint8_t *p, const uint8_t *end, uint32_t *cp) {
    uint32_t c = *p++;
    int extra;
    if (c < 0x80)              { extra = 0; }
    else if ((c & 0xE0) == 0xC0) { extra = 1; c &= 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; c &= 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; c &= 0x07; }
    else { return NULL; }
    while (extra--) {
        if (p >= end || (*p & 0xC0) != 0x80) {
            return NULL;
        }
        c = (c << 6) | (*p++ & 0x3F);
    }
    *cp = c;
    return p;
}

static void map_set(uint32_t cp, uint32_t glyph) {
    if (cp < 256 && con.map[cp] == NO_GLYPH) {
        con.map[cp] = (uint16_t)glyph;
    }
}

static void parse_psf2_table(const uint8_t *p, const uint8_t *end) {
    for (uint32_t g = 0; g < con.glyph_count && p < end; g++) {
        while (p < end && *p != 0xFF) {
            if (*p == 0xFE) { /* multi-codepoint sequences: skip to the terminator */
                while (p < end && *p != 0xFF) {
                    p++;
                }
                break;
            }
            uint32_t cp;
            const uint8_t *next = utf8_decode(p, end, &cp);
            if (!next) {
                return;
            }
            map_set(cp, g);
            p = next;
        }
        p++; /* skip 0xFF */
    }
}

static void parse_psf1_table(const uint8_t *p, const uint8_t *end) {
    for (uint32_t g = 0; g < con.glyph_count && p + 1 < end; g++) {
        for (;;) {
            if (p + 1 >= end) {
                return;
            }
            uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
            p += 2;
            if (v == 0xFFFF) {
                break;
            }
            if (v == 0xFFFE) { /* sequences: skip to the terminator */
                while (p + 1 < end && (p[0] | (p[1] << 8)) != 0xFFFF) {
                    p += 2;
                }
                continue;
            }
            map_set(v, g);
        }
    }
}

static bool load_font(void) {
    const uint8_t *data = _binary_terminus16_psf_start;
    const uint8_t *end = _binary_terminus16_psf_end;
    size_t size = (size_t)(end - data);
    const uint8_t *table = NULL;
    bool psf2 = false;

    if (size >= sizeof(struct psf2_header) &&
        ((const struct psf2_header *)data)->magic == PSF2_MAGIC) {
        const struct psf2_header *h = (const struct psf2_header *)data;
        con.glyphs = data + h->header_size;
        con.glyph_count = h->glyph_count;
        con.glyph_w = h->width;
        con.glyph_h = h->height;
        con.bytes_per_glyph = h->bytes_per_glyph;
        if (h->flags & PSF2_HAS_UNICODE) {
            table = con.glyphs + (size_t)h->glyph_count * h->bytes_per_glyph;
        }
        psf2 = true;
    } else if (size >= 4 && data[0] == PSF1_MAGIC0 && data[1] == PSF1_MAGIC1) {
        uint8_t mode = data[2];
        con.glyphs = data + 4;
        con.glyph_count = (mode & PSF1_MODE512) ? 512 : 256;
        con.glyph_w = 8;
        con.glyph_h = data[3];
        con.bytes_per_glyph = data[3];
        if (mode & (PSF1_MODEHASTAB | PSF1_MODESEQ)) {
            table = con.glyphs + (size_t)con.glyph_count * con.bytes_per_glyph;
        }
    } else {
        return false;
    }
    con.bytes_per_row = (con.glyph_w + 7) / 8;

    for (int i = 0; i < 256; i++) {
        con.map[i] = NO_GLYPH;
    }
    if (table && table < end) {
        if (psf2) {
            parse_psf2_table(table, end);
        } else {
            parse_psf1_table(table, end);
        }
    } else {
        for (uint32_t i = 0; i < 256 && i < con.glyph_count; i++) {
            con.map[i] = (uint16_t)i;
        }
    }

    uint16_t fallback = con.map['?'] != NO_GLYPH ? con.map['?'] : 0;
    for (int i = 0; i < 256; i++) {
        if (con.map[i] == NO_GLYPH) {
            con.map[i] = fallback;
        }
    }
    return true;
}

static void draw_glyph(uint32_t col, uint32_t row, uint8_t ch) {
    const uint8_t *glyph = con.glyphs + (size_t)con.map[ch] * con.bytes_per_glyph;
    uint32_t px = col * con.glyph_w;
    uint32_t py = row * con.glyph_h;

    for (uint32_t y = 0; y < con.glyph_h; y++) {
        uint32_t *line = (uint32_t *)(con.fb + (py + y) * con.pitch) + px;
        const uint8_t *bits = glyph + y * con.bytes_per_row;
        for (uint32_t x = 0; x < con.glyph_w; x++) {
            bool on = bits[x / 8] & (0x80 >> (x % 8));
            line[x] = on ? con.fg : con.bg;
        }
    }
}

static void fill_rows(uint32_t first_px_row, uint32_t count) {
    for (uint32_t y = first_px_row; y < first_px_row + count; y++) {
        uint32_t *line = (uint32_t *)(con.fb + y * con.pitch);
        for (uint32_t x = 0; x < con.fb_width; x++) {
            line[x] = con.bg;
        }
    }
}

static void scroll(void) {
    size_t row_bytes = con.glyph_h * con.pitch;
    memmove(con.fb, con.fb + row_bytes, (con.rows - 1) * row_bytes);
    fill_rows((con.rows - 1) * con.glyph_h, con.glyph_h);
}

static void newline(void) {
    con.cx = 0;
    if (++con.cy >= con.rows) {
        scroll();
        con.cy = con.rows - 1;
    }
}

bool console_init(struct limine_framebuffer *fb) {
    if (fb->bpp != 32 || fb->memory_model != LIMINE_FRAMEBUFFER_RGB) {
        return false;
    }
    if (!load_font()) {
        return false;
    }

    con.fb = fb->address;
    con.pitch = fb->pitch;
    con.fb_width = (uint32_t)fb->width;
    con.fb_height = (uint32_t)fb->height;
    con.r_shift = fb->red_mask_shift;
    con.g_shift = fb->green_mask_shift;
    con.b_shift = fb->blue_mask_shift;

    con.cols = con.fb_width / con.glyph_w;
    con.rows = con.fb_height / con.glyph_h;
    con.ready = true;

    console_set_color(0xD0D0D0, 0x101018);   /* DEFAULT_FG / DEFAULT_BG below */
    console_clear();
    return true;
}

void console_set_color(uint32_t fg_rgb, uint32_t bg_rgb) {
    con.fg = pack_rgb(fg_rgb);
    con.bg = pack_rgb(bg_rgb);
}

void console_clear(void) {
    if (!con.ready || con.suspended) {
        return;
    }
    uint64_t irq = irq_save();
    cursor_hide();
    fill_rows(0, con.fb_height);
    con.cx = con.cy = 0;
    cursor_show();
    irq_restore(irq);
}

/* --- ANSI escape sequences ----------------------------------------------------------
 *
 * Enough of the VT100/ANSI set for colored output and full-screen programs:
 *   ESC [ n ; ... m   colors: 0 reset, 1 bright, 30-37/90-97 text, 40-47/100-107 background,
 *                     39/49 default text/background
 *   ESC [ 2 J         clear the screen
 *   ESC [ row ; col H move the cursor (1-based; no numbers = top left)
 *   ESC [ K           clear to the end of the line
 */

#define DEFAULT_FG 0xD0D0D0
#define DEFAULT_BG 0x101018

static const uint32_t ansi_palette[16] = {
    0x101018, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xD0D0D0,
    0x5C6370, 0xFF7B86, 0xB5E890, 0xFFD787, 0x7FD4FF, 0xE0A0FF, 0x7FE0E8, 0xFFFFFF,
};

#define ANSI_MAX_PARAMS 4

static struct {
    enum { ANSI_TEXT, ANSI_ESCAPE, ANSI_CSI } state;
    int params[ANSI_MAX_PARAMS];
    int count;
    bool bright;
} ansi;

static void apply_sgr(int code) {
    if (code == 0) {
        ansi.bright = false;
        console_set_color(DEFAULT_FG, DEFAULT_BG);
    } else if (code == 1) {
        ansi.bright = true;
    } else if (code == 22) {
        ansi.bright = false;
    } else if (code >= 30 && code <= 37) {
        con.fg = pack_rgb(ansi_palette[code - 30 + (ansi.bright ? 8 : 0)]);
    } else if (code >= 90 && code <= 97) {
        con.fg = pack_rgb(ansi_palette[code - 90 + 8]);
    } else if (code == 39) {
        con.fg = pack_rgb(DEFAULT_FG);
    } else if (code >= 40 && code <= 47) {
        con.bg = pack_rgb(ansi_palette[code - 40]);
    } else if (code >= 100 && code <= 107) {
        con.bg = pack_rgb(ansi_palette[code - 100 + 8]);
    } else if (code == 49) {
        con.bg = pack_rgb(DEFAULT_BG);
    }
}

static void clear_to_end_of_line(void) {
    for (uint32_t col = con.cx; col < con.cols; col++) {
        draw_glyph(col, con.cy, ' ');
    }
}

static void run_csi(char final) {
    int p0 = ansi.count > 0 ? ansi.params[0] : 0;
    switch (final) {
    case 'm':
        if (ansi.count == 0) {
            apply_sgr(0);
        }
        for (int i = 0; i < ansi.count; i++) {
            apply_sgr(ansi.params[i]);
        }
        break;
    case 'J':
        if (p0 == 2) {
            fill_rows(0, con.fb_height);
            con.cx = con.cy = 0;
        }
        break;
    case 'H': {
        uint32_t row = p0 > 0 ? (uint32_t)p0 - 1 : 0;
        uint32_t col = ansi.count > 1 && ansi.params[1] > 0 ? (uint32_t)ansi.params[1] - 1 : 0;
        con.cy = row < con.rows ? row : con.rows - 1;
        con.cx = col < con.cols ? col : con.cols - 1;
        break;
    }
    case 'K':
        clear_to_end_of_line();
        break;
    }
}

/* --- cursor ---------------------------------------------------------------------------
 *
 * An underline at the next write position, drawn by inverting pixels: doing
 * it twice restores them, so we don't need to remember what was underneath.
 * It's taken down before anything is drawn and put back afterwards.
 */

#define CURSOR_HEIGHT 2

static struct {
    bool drawn;
    uint32_t col, row;
} cursor;

static void invert_underline(uint32_t col, uint32_t row) {
    uint32_t px = col * con.glyph_w;
    uint32_t py = (row + 1) * con.glyph_h - CURSOR_HEIGHT;
    for (uint32_t y = 0; y < CURSOR_HEIGHT; y++) {
        uint32_t *line = (uint32_t *)(con.fb + (py + y) * con.pitch) + px;
        for (uint32_t x = 0; x < con.glyph_w; x++) {
            line[x] ^= 0x00FFFFFF;
        }
    }
}

static void cursor_hide(void) {
    if (cursor.drawn) {
        invert_underline(cursor.col, cursor.row);
        cursor.drawn = false;
    }
}

static void cursor_show(void) {
    if (!cursor.drawn && con.cx < con.cols && con.cy < con.rows) {
        cursor.col = con.cx;
        cursor.row = con.cy;
        invert_underline(cursor.col, cursor.row);
        cursor.drawn = true;
    }
}

/* Returns true if `c` was part of an escape sequence (and so not printed). */
static bool ansi_consume(char c) {
    switch (ansi.state) {
    case ANSI_TEXT:
        if (c == '\x1b') {
            ansi.state = ANSI_ESCAPE;
            return true;
        }
        return false;
    case ANSI_ESCAPE:
        if (c == '[') {
            ansi.state = ANSI_CSI;
            ansi.count = 0;
            ansi.params[0] = 0;
        } else {
            ansi.state = ANSI_TEXT;   /* unsupported escape: drop it */
        }
        return true;
    case ANSI_CSI:
        if (c >= '0' && c <= '9') {
            if (ansi.count == 0) {
                ansi.count = 1;
            }
            int *p = &ansi.params[ansi.count - 1];
            if (*p < 10000) {
                *p = *p * 10 + (c - '0');
            }
        } else if (c == ';') {
            if (ansi.count == 0) {
                ansi.count = 1;   /* empty first parameter */
            }
            if (ansi.count < ANSI_MAX_PARAMS) {
                ansi.params[ansi.count++] = 0;
            }
        } else {
            if (c >= '@' && c <= '~') {
                run_csi(c);
            }
            ansi.state = ANSI_TEXT;
        }
        return true;
    }
    return false;
}

static void put_char(char c);

void console_suspend(bool suspended) {
    uint64_t irq = irq_save();
    if (suspended) {
        cursor.drawn = false;   /* the program will draw over it */
        con.suspended = true;
    } else if (con.suspended) {
        con.suspended = false;
        ansi.state = ANSI_TEXT;
        console_set_color(DEFAULT_FG, DEFAULT_BG);
        console_clear();
    }
    irq_restore(irq);
}

void console_panic_screen(uint32_t fg_rgb, uint32_t bg_rgb) {
    if (!con.ready) {
        return;
    }
    con.suspended = false;   /* whatever program had the screen, the console has it now */
    ansi.state = ANSI_TEXT;
    cursor.drawn = false;
    console_set_color(fg_rgb, bg_rgb);
    console_clear();
}

void console_putc(char c) {
    if (!con.ready || con.suspended) {
        return;
    }
    cursor_hide();
    if (!ansi_consume(c)) {
        put_char(c);
    }
    cursor_show();
}

static void put_char(char c) {
    switch (c) {
    case '\n':
        newline();
        break;
    case '\r':
        con.cx = 0;
        break;
    case '\t':
        do {
            put_char(' ');
        } while (con.cx % 8 != 0);
        break;
    case '\b':
        if (con.cx > 0) {
            con.cx--;
        }
        break;
    default:
        if (con.cx >= con.cols) {
            newline();
        }
        draw_glyph(con.cx++, con.cy, (uint8_t)c);
        break;
    }
}
