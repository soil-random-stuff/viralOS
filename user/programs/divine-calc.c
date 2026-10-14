/*
 * Calculator: a standard calculator, like the ones on Windows and macOS.
 *
 *   %   CE   C    ⌫          keys: digits, . , + - * / , Enter or =, Backspace,
 *   ⅟x  x²   √x   ÷                Escape (C), Delete (CE), %, Ctrl+C / Ctrl+V
 *   7   8    9    ×                (copy / paste the number, shared with the Text Editor)
 *   4   5    6    −
 *   1   2    3    +
 *   ±   0    .    =
 *
 * Above the number, the calculation so far ("12 + 7 ="). Pressing = again repeats the last
 * operation. Numbers are doubles, shown with up to 15 significant digits.
 */

#include <gui.h>
#include <math.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CLIPBOARD    _PATH_TMP "/.clipboard-text"   /* (the Text Editor's) */
#define PAD          8
#define GAP          4
#define DISPLAY_H    112
#define MAX_DIGITS   16

#define BACKGROUND   GFX_RGB(0xF3, 0xF3, 0xF6)
#define KEY_NUMBER   GFX_RGB(0xFF, 0xFF, 0xFF)
#define KEY_OTHER    GFX_RGB(0xE9, 0xE9, 0xEE)
#define KEY_HOVER    GFX_RGBA(0, 0, 0, 14)
#define KEY_DOWN     GFX_RGBA(0, 0, 0, 30)
#define ACCENT       GFX_RGB(0x2F, 0x6F, 0xEB)
#define ACCENT_HOT   GFX_RGB(0x25, 0x5E, 0xD6)
#define INK          GFX_RGB(0x1C, 0x1C, 0x22)
#define INK_SOFT     GFX_RGB(0x6A, 0x6A, 0x74)
#define LINE         GFX_RGBA(0, 0, 0, 18)

enum key_id {
    K_PERCENT, K_CE, K_C, K_BACK,
    K_RECIP, K_SQUARE, K_SQRT, K_DIV,
    K_7, K_8, K_9, K_MUL,
    K_4, K_5, K_6, K_SUB,
    K_1, K_2, K_3, K_ADD,
    K_NEGATE, K_0, K_POINT, K_EQUALS,
    K_COUNT,
};

static const char *const labels[K_COUNT] = {
    "%", "CE", "C", "\xE2\x8C\xAB",
    "\xE2\x85\x9Fx", "x\xC2\xB2", "\xE2\x88\x9Ax", "\xC3\xB7",
    "7", "8", "9", "\xC3\x97",
    "4", "5", "6", "\xE2\x88\x92",
    "1", "2", "3", "+",
    "\xC2\xB1", "0", ".", "=",
};

static struct {
    struct gui *g;
    struct gui_window *w;
    struct ttf_font *ui, *bold;
    int hover, pressed;

    /* The calculation. */
    char entry[MAX_DIGITS + 4];   /* what's being typed ("" when showing a result) */
    double value;                /* shown when not typing */
    double acc;                  /* the left side of the pending operation */
    int op;                      /* K_ADD / K_SUB / K_MUL / K_DIV, or -1 */
    int last_op;                 /* for repeating = */
    double last_operand;
    char expression[128];        /* the line above the number */
    const char *error;           /* "Cannot divide by zero"...: any key starts over */
} c;

/* --- numbers -------------------------------------------------------------------------------------- */

/* Text for a number: up to 15 significant digits, no trailing zeros, scientific when huge or tiny. */
static void format_number(double v, char *out, size_t size) {
    if (v != v) {
        strlcpy(out, "NaN", size);
        return;
    }
    if (v == 0) {
        strlcpy(out, "0", size);
        return;
    }
    double a = fabs(v);
    int e = (int)floor(log10(a));
    char buf[64], fmt[16];
    if (e >= 16 || e < -9) {
        double m = v / pow(10, e);
        if (fabs(m) >= 9.9999999999999995) {   /* rounding carried into the next power */
            m /= 10;
            e++;
        }
        snprintf(fmt, sizeof(fmt), "%%.%df", 14);
        snprintf(buf, sizeof(buf), fmt, m);
        char *end = buf + strlen(buf);
        while (end > buf && end[-1] == '0') {
            *--end = '\0';
        }
        if (end > buf && end[-1] == '.') {
            *--end = '\0';
        }
        snprintf(out, size, "%se%s%d", buf, e < 0 ? "-" : "+", e < 0 ? -e : e);
        return;
    }
    int decimals = 14 - e;
    decimals = decimals < 0 ? 0 : decimals > 15 ? 15 : decimals;
    snprintf(fmt, sizeof(fmt), "%%.%df", decimals);
    snprintf(buf, sizeof(buf), fmt, v);
    if (strchr(buf, '.')) {
        char *end = buf + strlen(buf);
        while (end[-1] == '0') {
            *--end = '\0';
        }
        if (end[-1] == '.') {
            *--end = '\0';
        }
    }
    if (strcmp(buf, "-0") == 0) {
        strlcpy(buf, "0", sizeof(buf));
    }
    strlcpy(out, buf, size);
}

/* With thousands separators in the whole part: 1234567.5 -> 1,234,567.5 (not in scientific). */
static void group_digits(const char *in, char *out, size_t size) {
    if (strchr(in, 'e') || strchr(in, 'N')) {
        strlcpy(out, in, size);
        return;
    }
    const char *digits = in[0] == '-' ? in + 1 : in;
    const char *point = strchr(digits, '.');
    int whole = point ? (int)(point - digits) : (int)strlen(digits);
    size_t o = 0;
    if (in[0] == '-' && o + 1 < size) {
        out[o++] = '-';
    }
    for (int i = 0; i < whole && o + 2 < size; i++) {
        out[o++] = digits[i];
        if ((whole - 1 - i) % 3 == 0 && i < whole - 1) {
            out[o++] = ',';
        }
    }
    out[o] = '\0';
    if (point) {
        strlcat(out, point, size);
    }
}

static double parse(const char *s) {
    double v = 0, scale = 1;
    bool neg = *s == '-', frac = false;
    s += neg;
    for (; *s; s++) {
        if (*s == '.') {
            frac = true;
        } else if (*s >= '0' && *s <= '9') {
            if (frac) {
                scale /= 10;
                v += (*s - '0') * scale;
            } else {
                v = v * 10 + (*s - '0');
            }
        }
    }
    return neg ? -v : v;
}

static bool typing(void) {
    return c.entry[0] != '\0';
}

static double current(void) {
    return typing() ? parse(c.entry) : c.value;
}

static const char *op_text(int op) {
    return op == K_ADD ? "+" : op == K_SUB ? "\xE2\x88\x92" : op == K_MUL ? "\xC3\x97" : "\xC3\xB7";
}

/* --- the calculation ------------------------------------------------------------------------------ */

static void clear_all(void) {
    c.entry[0] = '\0';
    c.value = c.acc = 0;
    c.op = c.last_op = -1;
    c.expression[0] = '\0';
    c.error = NULL;
}

static bool apply(double a, int op, double b, double *out) {
    switch (op) {
    case K_ADD: *out = a + b; break;
    case K_SUB: *out = a - b; break;
    case K_MUL: *out = a * b; break;
    case K_DIV:
        if (b == 0) {
            c.error = a == 0 ? "Result is undefined" : "Cannot divide by zero";
            return false;
        }
        *out = a / b;
        break;
    default: *out = b; break;
    }
    if (*out != *out || *out > 1e300 || *out < -1e300) {
        c.error = "Overflow";
        return false;
    }
    return true;
}

static void show_result(double v) {
    c.value = v;
    c.entry[0] = '\0';
}

static void press(int k) {
    if (c.error) {
        clear_all();
        if (k == K_C || k == K_CE || k == K_BACK) {
            return;
        }
    }
    char a[48], b[48];
    if ((k >= K_7 && k <= K_9) || (k >= K_4 && k <= K_6) || (k >= K_1 && k <= K_3) || k == K_0) {
        static const char digit_of[K_COUNT] = {
            [K_7] = '7', [K_8] = '8', [K_9] = '9', [K_4] = '4', [K_5] = '5', [K_6] = '6',
            [K_1] = '1', [K_2] = '2', [K_3] = '3', [K_0] = '0',
        };
        if (c.op < 0 && !typing() && c.expression[0]) {
            c.expression[0] = '\0';   /* a new calculation after a result */
        }
        if (strcmp(c.entry, "0") == 0 || strcmp(c.entry, "-0") == 0) {
            c.entry[strlen(c.entry) - 1] = '\0';
        }
        int count = 0;
        for (const char *p = c.entry; *p; p++) {
            count += *p >= '0' && *p <= '9';
        }
        if (count < MAX_DIGITS) {
            size_t n = strlen(c.entry);
            c.entry[n] = digit_of[k];
            c.entry[n + 1] = '\0';
        }
        return;
    }
    switch (k) {
    case K_POINT:
        if (!typing()) {
            strlcpy(c.entry, "0", sizeof(c.entry));
            if (c.op < 0) {
                c.expression[0] = '\0';
            }
        }
        if (!strchr(c.entry, '.')) {
            strlcat(c.entry, ".", sizeof(c.entry));
        }
        return;
    case K_BACK:
        if (typing()) {
            c.entry[strlen(c.entry) - 1] = '\0';
            if (strcmp(c.entry, "-") == 0 || !c.entry[0]) {
                strlcpy(c.entry, "0", sizeof(c.entry));
            }
        } else if (c.op < 0) {
            c.expression[0] = '\0';
        }
        return;
    case K_CE:
        strlcpy(c.entry, "0", sizeof(c.entry));
        return;
    case K_C:
        clear_all();
        return;
    case K_NEGATE:
        if (typing()) {
            if (c.entry[0] == '-') {
                memmove(c.entry, c.entry + 1, strlen(c.entry));
            } else if (strlen(c.entry) < sizeof(c.entry) - 1) {
                memmove(c.entry + 1, c.entry, strlen(c.entry) + 1);
                c.entry[0] = '-';
            }
        } else {
            c.value = -c.value;
        }
        return;
    case K_PERCENT: {
        /* Like Windows: a percentage of the left side for + and -, a fraction for × and ÷. */
        double v = current();
        v = c.op == K_ADD || c.op == K_SUB ? c.acc * v / 100 : v / 100;
        show_result(v);
        return;
    }
    case K_RECIP:
    case K_SQUARE:
    case K_SQRT: {
        double v = current(), r;
        format_number(v, a, sizeof(a));
        if (k == K_RECIP) {
            if (v == 0) {
                c.error = "Cannot divide by zero";
                return;
            }
            r = 1 / v;
            snprintf(b, sizeof(b), "1/(%s)", a);
        } else if (k == K_SQUARE) {
            r = v * v;
            snprintf(b, sizeof(b), "sqr(%s)", a);
        } else {
            if (v < 0) {
                c.error = "Invalid input";
                return;
            }
            r = sqrt(v);
            snprintf(b, sizeof(b), "\xE2\x88\x9A(%s)", a);
        }
        if (c.op >= 0) {
            format_number(c.acc, a, sizeof(a));
            snprintf(c.expression, sizeof(c.expression), "%s %s %s", a, op_text(c.op), b);
        } else {
            strlcpy(c.expression, b, sizeof(c.expression));
        }
        show_result(r);
        return;
    }
    case K_ADD:
    case K_SUB:
    case K_MUL:
    case K_DIV: {
        double v = current();
        if (c.op >= 0 && typing()) {
            double r;
            if (!apply(c.acc, c.op, v, &r)) {
                return;
            }
            c.acc = r;
        } else if (c.op < 0) {
            c.acc = v;
        }   /* (an operator right after another: just this one instead) */
        c.op = k;
        show_result(c.acc);
        format_number(c.acc, a, sizeof(a));
        snprintf(c.expression, sizeof(c.expression), "%s %s", a, op_text(k));
        return;
    }
    case K_EQUALS: {
        double left, right, r;
        int op;
        if (c.op >= 0) {
            left = c.acc;
            op = c.op;
            right = current();
        } else if (c.last_op >= 0) {
            left = current();   /* = again: the same operation on the result */
            op = c.last_op;
            right = c.last_operand;
        } else {
            format_number(current(), a, sizeof(a));
            snprintf(c.expression, sizeof(c.expression), "%s =", a);
            show_result(current());
            return;
        }
        if (!apply(left, op, right, &r)) {
            c.op = -1;
            return;
        }
        format_number(left, a, sizeof(a));
        format_number(right, b, sizeof(b));
        snprintf(c.expression, sizeof(c.expression), "%s %s %s =", a, op_text(op), b);
        c.last_op = op;
        c.last_operand = right;
        c.op = -1;
        show_result(r);
        return;
    }
    default:
        return;
    }
}

/* --- clipboard ------------------------------------------------------------------------------------ */

static void copy_number(void) {
    char text[48];
    if (typing()) {
        strlcpy(text, c.entry, sizeof(text));
    } else {
        format_number(c.value, text, sizeof(text));
    }
    int fd = open(CLIPBOARD, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        write(fd, text, strlen(text));
        close(fd);
    }
}

static void paste_number(void) {
    char text[64];
    int fd = open(CLIPBOARD, O_RDONLY);
    if (fd < 0) {
        return;
    }
    long n = read(fd, text, sizeof(text) - 1);
    close(fd);
    text[n > 0 ? n : 0] = '\0';
    /* Keep a plain number: digits, one point, a leading minus; commas and spaces dropped. */
    char clean[MAX_DIGITS + 4];
    size_t o = 0;
    int digits = 0;
    bool point = false;
    for (const char *p = text; *p && o < sizeof(clean) - 1; p++) {
        if (*p >= '0' && *p <= '9' && digits < MAX_DIGITS) {
            clean[o++] = *p;
            digits++;
        } else if (*p == '.' && !point) {
            clean[o++] = '.';
            point = true;
        } else if (*p == '-' && o == 0) {
            clean[o++] = '-';
        } else if (*p != ',' && *p != ' ' && *p != '\n' && *p != '\r') {
            return;   /* not a number */
        }
    }
    clean[o] = '\0';
    if (digits) {
        if (c.error) {
            clear_all();
        }
        strlcpy(c.entry, clean, sizeof(c.entry));
    }
}

/* --- drawing -------------------------------------------------------------------------------------- */

static struct gfx_rect key_rect(int k) {
    struct gfx_surface *s = gui_window_surface(c.w);
    int cols = 4, rows = K_COUNT / 4;
    int top = DISPLAY_H;
    int kw = (s->width - 2 * PAD - (cols - 1) * GAP) / cols;
    int kh = (s->height - top - PAD - (rows - 1) * GAP) / rows;
    return gfx_rect(PAD + (k % cols) * (kw + GAP), top + (k / cols) * (kh + GAP), kw, kh);
}

static int key_at(int x, int y) {
    for (int k = 0; k < K_COUNT; k++) {
        if (gfx_rect_contains(key_rect(k), x, y)) {
            return k;
        }
    }
    return -1;
}

static bool is_number_key(int k) {
    return (k >= K_7 && k <= K_9) || (k >= K_4 && k <= K_6) || (k >= K_1 && k <= K_3) || k == K_0 ||
           k == K_NEGATE || k == K_POINT;
}

static void draw(void) {
    struct gfx_surface *s = gui_window_surface(c.w);
    gfx_clear(s, BACKGROUND);

    /* The calculation so far, then the number (smaller if it doesn't fit). */
    float right = s->width - PAD - 10;
    if (c.expression[0]) {
        float w = ttf_text_width(c.ui, 15, c.expression);
        gfx_set_clip(s, gfx_rect(PAD, 0, s->width - 2 * PAD, DISPLAY_H));
        ttf_draw_text(s, c.ui, 15, right - w, 34, INK_SOFT, c.expression);
        gfx_reset_clip(s);
    }
    char shown[64], grouped[80];
    if (c.error) {
        strlcpy(grouped, c.error, sizeof(grouped));
    } else {
        if (typing()) {
            strlcpy(shown, c.entry, sizeof(shown));
        } else {
            format_number(c.value, shown, sizeof(shown));
        }
        group_digits(shown, grouped, sizeof(grouped));
    }
    float size = c.error ? 30 : 46;
    float room = s->width - 2 * PAD - 20;
    while (size > 14 && ttf_text_width(c.bold, size, grouped) > room) {
        size -= 2;
    }
    float w = ttf_text_width(c.bold, size, grouped);
    ttf_draw_text(s, c.bold, size, right - w, DISPLAY_H - 22, INK, grouped);

    for (int k = 0; k < K_COUNT; k++) {
        struct gfx_rect r = key_rect(k);
        bool equals = k == K_EQUALS;
        gfx_color fill = equals ? (c.hover == k ? ACCENT_HOT : ACCENT) : is_number_key(k) ? KEY_NUMBER : KEY_OTHER;
        gfx_fill_rounded_rect(s, r, 7, fill);
        if (!equals) {
            gfx_stroke_rounded_rect(s, r, 7, 1, LINE);
            if (c.pressed == k) {
                gfx_fill_rounded_rect(s, r, 7, KEY_DOWN);
            } else if (c.hover == k) {
                gfx_fill_rounded_rect(s, r, 7, KEY_HOVER);
            }
        } else if (c.pressed == k) {
            gfx_fill_rounded_rect(s, r, 7, KEY_DOWN);
        }
        bool digit = is_number_key(k) && k != K_NEGATE;
        struct ttf_font *f = digit || equals ? c.bold : c.ui;
        float ts = digit ? 19 : equals ? 22 : 17;
        float tw = ttf_text_width(f, ts, labels[k]);
        ttf_draw_text(s, f, ts, r.x + (r.w - tw) / 2, r.y + r.h / 2.0f + ts * 0.36f,
                      equals ? GFX_RGB(255, 255, 255) : INK, labels[k]);
    }
    gui_window_invalidate(c.w, NULL);
}

/* --- input ---------------------------------------------------------------------------------------- */

#define SC_C 0x2E
#define SC_V 0x2F

static int key_for(const struct gui_event *ev) {
    if (ev->modifiers & OS_MOD_CTRL) {
        return -1;
    }
    switch (ev->key) {
    case OS_KEY_ENTER: return K_EQUALS;
    case OS_KEY_BACKSPACE: return K_BACK;
    case OS_KEY_ESCAPE: return K_C;
    case OS_KEY_DELETE: return K_CE;
    }
    switch (ev->ch) {
    case '0': return K_0;
    case '1': return K_1;
    case '2': return K_2;
    case '3': return K_3;
    case '4': return K_4;
    case '5': return K_5;
    case '6': return K_6;
    case '7': return K_7;
    case '8': return K_8;
    case '9': return K_9;
    case '.': case ',': return K_POINT;
    case '+': return K_ADD;
    case '-': return K_SUB;
    case '*': case 'x': case 'X': return K_MUL;
    case '/': return K_DIV;
    case '=': case '\n': case '\r': return K_EQUALS;
    case '%': return K_PERCENT;
    case 'r': case 'R': return K_RECIP;
    case 'q': case 'Q': return K_SQUARE;
    case '@': return K_SQRT;
    }
    return -1;
}

int main(void) {
    c.g = gui_open();
    if (!c.g) {
        return 1;
    }
    c.ui = gui_font(c.g, GUI_FONT_REGULAR);
    c.bold = gui_font(c.g, GUI_FONT_BOLD);
    if (!c.ui || !c.bold) {
        return 1;
    }
    c.w = gui_window_create(c.g, "Calculator", -1, -1, 320, 470);
    if (!c.w) {
        return 1;
    }
    c.hover = c.pressed = -1;
    clear_all();
    draw();

    struct gui_event ev;
    for (;;) {
        if (!gui_wait_event(c.g, &ev, -1)) {
            continue;
        }
        switch (ev.type) {
        case GUI_EVENT_QUIT:
        case GUI_EVENT_CLOSE:
            gui_close(c.g);
            return 0;
        case GUI_EVENT_RESIZE:
            break;
        case GUI_EVENT_MOUSE_MOVE: {
            int k = key_at(ev.x, ev.y);
            if (k == c.hover) {
                continue;
            }
            c.hover = k;
            gui_window_set_cursor(c.w, k >= 0 ? GUI_CURSOR_HAND : GUI_CURSOR_ARROW);
            break;
        }
        case GUI_EVENT_MOUSE_DOWN:
            if (ev.button == OS_MOUSE_LEFT) {
                c.pressed = key_at(ev.x, ev.y);
            }
            break;
        case GUI_EVENT_MOUSE_UP:
            if (ev.button == OS_MOUSE_LEFT && c.pressed >= 0 && key_at(ev.x, ev.y) == c.pressed) {
                press(c.pressed);
            }
            c.pressed = -1;
            break;
        case GUI_EVENT_KEY:
            if ((ev.modifiers & OS_MOD_CTRL) && ev.key == SC_C) {
                copy_number();
                continue;
            }
            if ((ev.modifiers & OS_MOD_CTRL) && ev.key == SC_V) {
                paste_number();
                break;
            }
            {
                int k = key_for(&ev);
                if (k < 0) {
                    continue;
                }
                press(k);
            }
            break;
        default:
            continue;
        }
        draw();
    }
}
