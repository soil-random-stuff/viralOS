#pragma once

#include <stdarg.h>
#include <stddef.h>

/*
 * Formatted output: %c %s %d %i %u %x %X %p %f %%, flags '-' and '0', a width,
 * a precision (%.3f, %.5s) and the l / ll / z length modifiers.
 */
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vdprintf(int fd, const char *fmt, va_list ap);
/* Format into str (at most size bytes, always NUL-terminated); returns the full length. */
int snprintf(char *str, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
int puts(const char *s);       /* appends a newline */
int putchar(int c);
