/* memtest: exercise malloc/realloc/free and the brk() heap behind them. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COUNT 1000

static uint32_t rng = 12345;

static uint32_t next_random(void) {
    rng = rng * 1103515245 + 12345;
    return rng >> 8;
}

static int check(const uint8_t *p, size_t n, uint8_t value) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] != value) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    static uint8_t *blocks[COUNT];
    static size_t sizes[COUNT];
    uintptr_t heap_before = (uintptr_t)sbrk(0);

    for (int i = 0; i < COUNT; i++) {
        sizes[i] = 1 + next_random() % 4096;
        blocks[i] = malloc(sizes[i]);
        if (!blocks[i]) {
            printf("memtest: malloc failed at %d\n", i);
            return 1;
        }
        memset(blocks[i], (uint8_t)i, sizes[i]);
    }
    uintptr_t heap_full = (uintptr_t)sbrk(0);

    /* Free every other block, grow the rest, and check nothing got overwritten. */
    for (int i = 0; i < COUNT; i += 2) {
        free(blocks[i]);
        blocks[i] = NULL;
    }
    for (int i = 1; i < COUNT; i += 2) {
        uint8_t *bigger = realloc(blocks[i], sizes[i] * 2);
        if (!bigger || !check(bigger, sizes[i], (uint8_t)i)) {
            printf("memtest: realloc lost data in block %d\n", i);
            return 1;
        }
        blocks[i] = bigger;
    }
    for (int i = 1; i < COUNT; i += 2) {
        free(blocks[i]);
    }

    /* One large allocation: touch every page so it's really there. */
    size_t big_size = 16 * 1024 * 1024;
    uint8_t *big = malloc(big_size);
    if (!big) {
        printf("memtest: 16 MiB malloc failed\n");
        return 1;
    }
    for (size_t off = 0; off < big_size; off += 4096) {
        big[off] = (uint8_t)off;
    }
    free(big);

    int *zeros = calloc(1000, sizeof(int));
    for (int i = 0; zeros && i < 1000; i++) {
        if (zeros[i] != 0) {
            printf("memtest: calloc memory not zeroed\n");
            return 1;
        }
    }
    free(zeros);

    uintptr_t heap_after = (uintptr_t)sbrk(0);
    printf("memtest: OK - %d blocks, realloc, 16 MiB block, calloc\n", COUNT);
    printf("  heap start %p, after %d blocks +%lu KiB, at the end +%lu KiB\n",
           (void *)heap_before, COUNT, (unsigned long)(heap_full - heap_before) / 1024,
           (unsigned long)(heap_after - heap_before) / 1024);
    return 0;
}
