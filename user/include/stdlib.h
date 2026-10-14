#pragma once

#include <stddef.h>

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

__attribute__((noreturn)) void exit(int code);

int atoi(const char *s);
long strtol(const char *s, char **end, int base);
int abs(int x);
#define alloca(size) __builtin_alloca(size)

/* Sort `count` items of `size` bytes (heapsort: no extra memory, never quadratic). */
void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *));
