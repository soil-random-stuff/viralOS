#pragma once

#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
void *memchr(const void *s, int c, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strcpy(char *dest, const char *src);
char *strcat(char *dest, const char *src);
/* Copy with truncation, always NUL-terminated (if size > 0). Returns strlen(src). */
size_t strlcpy(char *dest, const char *src, size_t size);
/* Append, truncating, always NUL-terminated (if there was a NUL within size). Returns the full length. */
size_t strlcat(char *dest, const char *src, size_t size);
char *strdup(const char *s);
