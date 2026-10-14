/*
 * crashme <how>: misbehave in various ways to show that a broken program
 * only takes itself down, never the kernel.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile int depth_limit = 1 << 30;   /* stops GCC proving the recursion endless */

static int recurse(int depth) {
    volatile char pad[512];
    pad[0] = (char)depth;
    if (depth > depth_limit) {
        return 0;
    }
    return recurse(depth + 1) + pad[0];
}

int main(int argc, char **argv) {
    const char *how = argc > 1 ? argv[1] : "";

    if (strcmp(how, "null") == 0) {
        printf("writing to address 0...\n");
        *(volatile int *)0 = 1;
    } else if (strcmp(how, "kernel") == 0) {
        printf("reading kernel memory at 0xffffffff80000000...\n");
        printf("got %lx\n", *(volatile uint64_t *)0xffffffff80000000ULL);
    } else if (strcmp(how, "code") == 0) {
        printf("writing into my own (read-only) code...\n");
        *(volatile uint8_t *)(uintptr_t)main = 0xC3;
    } else if (strcmp(how, "cli") == 0) {
        printf("trying to disable interrupts (privileged instruction)...\n");
        __asm__ volatile ("cli");
    } else if (strcmp(how, "stack") == 0) {
        printf("recursing until the stack runs out...\n");
        recurse(0);
    } else if (strcmp(how, "syscall") == 0) {
        /* Handing the kernel a kernel address must fail cleanly, not crash it. */
        long r = write(STDOUT_FILENO, (const void *)0xffffffff80000000ULL, 16);
        printf("write() with a kernel pointer returned %ld (%s); still alive\n",
               r, strerror((int)r));
        return 0;
    } else {
        printf("usage: crashme null|kernel|code|cli|stack|syscall\n");
        return 1;
    }
    printf("...and survived?!\n");
    return 1;
}
