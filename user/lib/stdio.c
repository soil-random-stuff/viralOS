/*
 * printf and friends. Output is collected in a small buffer and handed to
 * write() in pieces, so one printf is usually one system call. snprintf
 * runs the same formatter into the caller's buffer instead.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct out {
    int fd;
    char *str;          /* snprintf: write here instead of to fd */
    size_t str_size;
    char buf[256];
    size_t len;
    int total;          /* characters produced (even those that didn't fit in str) */
};

static void flush(struct out *o) {
    if (o->len && !o->str) {
        write(o->fd, o->buf, o->len);
    }
    o->len = 0;
}

static void put(struct out *o, char c) {
    if (o->str) {
        if ((size_t)o->total + 1 < o->str_size) {
            o->str[o->total] = c;
        }
        o->total++;
        return;
    }
    if (o->len == sizeof(o->buf)) {
        flush(o);
    }
    o->buf[o->len++] = c;
    o->total++;
}

static void pad(struct out *o, int count, char c) {
    while (count-- > 0) {
        put(o, c);
    }
}

static void field(struct out *o, const char *s, int len, int width, bool left, char pad_char) {
    if (!left) {
        pad(o, width - len, pad_char);
    }
    for (int i = 0; i < len; i++) {
        put(o, s[i]);
    }
    if (left) {
        pad(o, width - len, ' ');
    }
}

static void number(struct out *o, uint64_t v, unsigned base, bool upper, bool negative,
                   int width, bool left, bool zero) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[24];
    int i = sizeof(buf);
    do {
        buf[--i] = digits[v % base];
        v /= base;
    } while (v);
    int len = (int)sizeof(buf) - i;
    if (negative) {
        if (zero && !left) {
            put(o, '-');
            field(o, &buf[i], len, width - 1, false, '0');
            return;
        }
        buf[--i] = '-';
        len++;
    }
    field(o, &buf[i], len, width, left, zero && !left ? '0' : ' ');
}

/* %f: fixed-point with `precision` digits after the point (max 17). */
static void float_number(struct out *o, double v, int precision, int width, bool left,
                         bool zero) {
    char buf[48];
    int len = 0;
    bool negative = v < 0;
    if (v != v) {
        field(o, "nan", 3, width, left, ' ');
        return;
    }
    if (negative) {
        v = -v;
    }
    if (v > 1.8e19) {   /* doesn't fit our integer part; no %e yet */
        field(o, negative ? "-inf" : "inf", negative ? 4 : 3, width, left, ' ');
        return;
    }
    if (precision > 17) {
        precision = 17;
    }

    double rounding = 0.5;
    for (int i = 0; i < precision; i++) {
        rounding /= 10;
    }
    v += rounding;

    uint64_t whole = (uint64_t)v;
    double fraction = v - (double)whole;

    char digits[24];
    int n = 0;
    do {
        digits[n++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole);
    while (n) {
        buf[len++] = digits[--n];
    }
    if (precision > 0) {
        buf[len++] = '.';
        for (int i = 0; i < precision; i++) {
            fraction *= 10;
            int d = (int)fraction;
            buf[len++] = (char)('0' + d);
            fraction -= d;
        }
    }

    if (negative) {
        if (zero && !left) {
            put(o, '-');
            field(o, buf, len, width - 1, false, '0');
            return;
        }
        memmove(buf + 1, buf, (size_t)len);
        buf[0] = '-';
        len++;
    }
    field(o, buf, len, width, left, zero && !left ? '0' : ' ');
}

static int format(struct out *out, const char *fmt, va_list ap);

int vdprintf(int fd, const char *fmt, va_list ap) {
    struct out o = { .fd = fd };
    return format(&o, fmt, ap);
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
    struct out o = { .str = str, .str_size = size };
    int n = format(&o, fmt, ap);
    if (size > 0) {
        str[(size_t)n < size ? (size_t)n : size - 1] = '\0';
    }
    return n;
}

int snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return n;
}

static int format(struct out *out, const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(out, *fmt);
            continue;
        }
        fmt++;
        bool left = false, zero = false, alt = false;
        for (;; fmt++) {
            if (*fmt == '-') { left = true; }
            else if (*fmt == '0') { zero = true; }
            else if (*fmt == '#') { alt = true; }    /* %#x -> 0x1f */
            else { break; }
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        int precision = -1;
        if (*fmt == '.') {
            precision = 0;
            fmt++;
            while (*fmt >= '0' && *fmt <= '9') {
                precision = precision * 10 + (*fmt++ - '0');
            }
        }
        int longs = 0;
        while (*fmt == 'l' || *fmt == 'z') {
            longs++;
            fmt++;
        }
        switch (*fmt) {
        case 'c': {
            char c = (char)va_arg(ap, int);
            field(out, &c, 1, width, left, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) {
                s = "(null)";
            }
            int len = (int)strlen(s);
            if (precision >= 0 && precision < len) {
                len = precision;   /* %.3s prints at most 3 characters */
            }
            field(out, s, len, width, left, ' ');
            break;
        }
        case 'f':
            float_number(out, va_arg(ap, double), precision < 0 ? 6 : precision,
                         width, left, zero);
            break;
        case 'd':
        case 'i': {
            int64_t v = longs ? va_arg(ap, int64_t) : va_arg(ap, int);
            bool neg = v < 0;
            number(out, neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v, 10, false, neg,
                   width, left, zero);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = longs ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
            if (alt && *fmt != 'u' && v != 0) {
                put(out, '0');
                put(out, *fmt);
                width -= 2;
            }
            number(out, v, *fmt == 'u' ? 10 : 16, *fmt == 'X', false, width, left, zero);
            break;
        }
        case 'p':
            put(out, '0');
            put(out, 'x');
            number(out, (uintptr_t)va_arg(ap, void *), 16, false, false, 0, false, false);
            break;
        case '%':
            put(out, '%');
            break;
        case '\0':
            flush(out);
            return out->total;
        default:
            put(out, '%');
            put(out, *fmt);
            break;
        }
    }
    flush(out);
    return out->total;
}

int dprintf(int fd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(STDOUT_FILENO, fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s) {
    size_t len = strlen(s);
    write(STDOUT_FILENO, s, len);
    write(STDOUT_FILENO, "\n", 1);
    return (int)len + 1;
}

int putchar(int c) {
    char ch = (char)c;
    write(STDOUT_FILENO, &ch, 1);
    return (unsigned char)ch;
}
