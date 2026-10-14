/*
 * malloc for user programs: a first-fit free list (sorted by address, so
 * freed neighbours merge), carved out of memory obtained with sbrk().
 * Every block starts with a 16-byte header; payloads are 16-byte aligned.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ALIGNMENT   16
#define MAGIC_USED  0xA110CA7Eu
#define MAGIC_FREE  0xF4EEB10Cu
#define MIN_GROWTH  (64 * 1024)

struct block {
    size_t size;        /* whole block, header included */
    uint32_t magic;
    uint32_t reserved;
};

struct free_block {
    struct block header;
    struct free_block *next;
};

#define HEADER_SIZE sizeof(struct block)
#define MIN_BLOCK   32
#define ALIGN_UP(x) (((x) + ALIGNMENT - 1) & ~(size_t)(ALIGNMENT - 1))

static struct free_block *free_list;

static void die(const char *msg) {
    write(STDERR_FILENO, msg, strlen(msg));
    exit(134);   /* like abort() */
}

static int adjacent(struct free_block *a, struct free_block *b) {
    return (char *)a + a->header.size == (char *)b;
}

static void insert_free(struct free_block *b) {
    b->header.magic = MAGIC_FREE;
    struct free_block *prev = NULL, *next = free_list;
    while (next && next < b) {
        prev = next;
        next = next->next;
    }
    b->next = next;
    if (next && adjacent(b, next)) {
        b->header.size += next->header.size;
        b->next = next->next;
    }
    if (prev && adjacent(prev, b)) {
        prev->header.size += b->header.size;
        prev->next = b->next;
    } else if (prev) {
        prev->next = b;
    } else {
        free_list = b;
    }
}

static int grow(size_t need) {
    size_t bytes = need > MIN_GROWTH ? need : MIN_GROWTH;
    bytes = (bytes + 4095) & ~(size_t)4095;
    void *mem = sbrk((intptr_t)bytes);
    if (mem == (void *)-1) {
        return 0;
    }
    /* The heap starts page-aligned, and we always grow by whole pages. */
    struct free_block *b = mem;
    b->header.size = bytes;
    insert_free(b);
    return 1;
}

void *malloc(size_t size) {
    if (size == 0 || size > ((size_t)1 << 40)) {
        return NULL;
    }
    size_t need = ALIGN_UP(size + HEADER_SIZE);
    if (need < MIN_BLOCK) {
        need = MIN_BLOCK;
    }
    for (;;) {
        struct free_block **link = &free_list;
        for (struct free_block *b = free_list; b; link = &b->next, b = b->next) {
            if (b->header.size < need) {
                continue;
            }
            if (b->header.size - need >= MIN_BLOCK) {
                struct free_block *rest = (struct free_block *)((char *)b + need);
                rest->header.size = b->header.size - need;
                rest->header.magic = MAGIC_FREE;
                rest->next = b->next;
                *link = rest;
                b->header.size = need;
            } else {
                *link = b->next;
            }
            b->header.magic = MAGIC_USED;
            return (char *)b + HEADER_SIZE;
        }
        if (!grow(need)) {
            return NULL;
        }
    }
}

void free(void *ptr) {
    if (!ptr) {
        return;
    }
    struct block *b = (struct block *)((char *)ptr - HEADER_SIZE);
    if (b->magic == MAGIC_FREE) {
        die("free(): double free detected\n");
    }
    if (b->magic != MAGIC_USED) {
        die("free(): invalid pointer or corrupted heap\n");
    }
    insert_free((struct free_block *)b);
}

void *calloc(size_t count, size_t size) {
    if (size && count > (size_t)-1 / size) {
        return NULL;
    }
    void *p = malloc(count * size);
    if (p) {
        memset(p, 0, count * size);
    }
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    struct block *b = (struct block *)((char *)ptr - HEADER_SIZE);
    if (b->magic != MAGIC_USED) {
        die("realloc(): invalid pointer\n");
    }
    size_t capacity = b->size - HEADER_SIZE;
    if (size <= capacity) {
        return ptr;
    }
    void *bigger = malloc(size);
    if (bigger) {
        memcpy(bigger, ptr, capacity);
        free(ptr);
    }
    return bigger;
}
