#pragma once

/* Where the kernel's log is now; show its last `lines` lines before that position on the screen
   again (a panic). */
uint64_t klog_position(void);
void klog_replay(int lines, uint64_t end);

#include <stdarg.h>
#include <stddef.h>

/*
 * printf to the serial port and the framebuffer console.
 * Supports %c %s %d %i %u %x %X %p %%, flags '-' and '0', a width, and the
 * length modifiers l, ll and z.
 */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);
/* The same formatting into a buffer (always NUL-terminated); returns the length it wanted. */
int ksnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Write `len` raw bytes (e.g. file contents, which may contain NULs). */
void kwrite(const char *buf, size_t len);
