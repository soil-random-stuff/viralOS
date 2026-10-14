#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Moving data between the kernel and a process's memory. Every user page is
 * looked up in the process's page tables first (present, user-accessible,
 * writable if we write), and accessed through the direct map, so a bad
 * pointer from a program returns -EFAULT instead of crashing the kernel.
 */
bool copy_from_user(uint64_t space, void *dst, uint64_t src, size_t len);
bool copy_to_user(uint64_t space, uint64_t dst, const void *src, size_t len);

/* Copy a NUL-terminated string of at most `max - 1` chars. Returns its length or -errno. */
int64_t copy_string_from_user(uint64_t space, char *dst, uint64_t src, size_t max);
