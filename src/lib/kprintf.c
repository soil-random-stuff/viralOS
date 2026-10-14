#include <stdbool.h>
#include <stdint.h>

#include "arch/x86_64/cpu.h"
#include "drivers/console.h"
#include "drivers/serial.h"
#include "lib/kprintf.h"
#include "lib/string.h"

/* ksnprintf points this at its buffer (formatting runs with interrupts off, so one is enough). */
static char *sink;
static size_t sink_size, sink_len;

/* The kernel's log: the last KLOG_SIZE bytes of everything printed (shown again on a panic). */
#define KLOG_SIZE 16384
static char klog[KLOG_SIZE];
static uint64_t klog_len;

static void out(char c) {
    if (sink) {
        if (sink_len + 1 < sink_size) {
            sink[sink_len] = c;
        }
        sink_len++;
        return;
    }
    klog[klog_len++ % KLOG_SIZE] = c;
    serial_putc(c);
    console_putc(c);
}

uint64_t klog_position(void) {
    return klog_len;
}

void klog_replay(int lines, uint64_t end) {
    if (end > klog_len) {
        end = klog_len;
    }
    uint64_t start = klog_len > KLOG_SIZE ? klog_len - KLOG_SIZE : 0;
    if (end < start) {
        return;
    }
    /* Back from the end over `lines` line breaks (one more for the line being written). */
    uint64_t at = end;
    int seen = 0;
    while (at > start && seen <= lines) {
        if (klog[(at - 1) % KLOG_SIZE] == '\n') {
            seen++;
            if (seen > lines) {
                break;
            }
        }
        at--;
    }
    for (; at < end; at++) {
        console_putc(klog[at % KLOG_SIZE]);   /* (the serial port has it already) */
    }
}

static void pad(int count, char c) {
    while (count-- > 0) {
        out(c);
    }
}

static void print_field(const char *s, int len, int width, bool left, char pad_char) {
    if (!left) {
        pad(width - len, pad_char);
    }
    for (int i = 0; i < len; i++) {
        out(s[i]);
    }
    if (left) {
        pad(width - len, ' ');
    }
}

static void print_number(uint64_t v, unsigned base, bool upper, bool negative,
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
        if (zero && !left) { /* sign goes before the zero padding */
            out('-');
            print_field(&buf[i], len, width - 1, false, '0');
            return;
        }
        buf[--i] = '-';
        len++;
    }
    print_field(&buf[i], len, width, left, zero && !left ? '0' : ' ');
}

static void format(const char *fmt, va_list ap);

/*
 * Interrupts stay off for the whole call so lines from different threads
 * don't interleave mid-line (and the console's cursor can't be raced).
 */
void kvprintf(const char *fmt, va_list ap) {
    uint64_t irq = irq_save();
    format(fmt, ap);
    irq_restore(irq);
}

void kwrite(const char *buf, size_t len) {
    uint64_t irq = irq_save();
    for (size_t i = 0; i < len; i++) {
        out(buf[i]);
    }
    irq_restore(irq);
}

static void format(const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out(*fmt);
            continue;
        }
        fmt++;

        bool left = false, zero = false;
        for (;; fmt++) {
            if (*fmt == '-') { left = true; }
            else if (*fmt == '0') { zero = true; }
            else { break; }
        }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }

        int longs = 0; /* 0 = int, 1+ = 64-bit */
        while (*fmt == 'l' || *fmt == 'z') {
            longs++;
            fmt++;
        }

        switch (*fmt) {
        case 'c': {
            char c = (char)va_arg(ap, int);
            print_field(&c, 1, width, left, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) {
                s = "(null)";
            }
            print_field(s, (int)strlen(s), width, left, ' ');
            break;
        }
        case 'd':
        case 'i': {
            int64_t v = longs ? va_arg(ap, int64_t) : va_arg(ap, int);
            bool neg = v < 0;
            uint64_t mag = neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            print_number(mag, 10, false, neg, width, left, zero);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = longs ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
            unsigned base = *fmt == 'u' ? 10 : 16;
            print_number(v, base, *fmt == 'X', false, width, left, zero);
            break;
        }
        case 'p':
            out('0');
            out('x');
            print_number((uintptr_t)va_arg(ap, void *), 16, false, false, 16, false, true);
            break;
        case '%':
            out('%');
            break;
        case '\0':
            return;
        default: /* unknown conversion: print it verbatim */
            out('%');
            out(*fmt);
            break;
        }
    }
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uint64_t irq = irq_save();
    sink = buf;
    sink_size = size;
    sink_len = 0;
    format(fmt, ap);
    if (size) {
        buf[sink_len < size ? sink_len : size - 1] = '\0';
    }
    size_t len = sink_len;
    sink = NULL;
    irq_restore(irq);
    va_end(ap);
    return (int)len;
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}
