/* The first user program: shows that it runs as its own process. */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    char cwd[256];
    getcwd(cwd, sizeof(cwd));

    printf("Hello from user mode!\n");
    printf("  pid:       %ld\n", getpid());
    printf("  main() at: %p (user address space)\n", (void *)main);
    printf("  stack at:  %p\n", (void *)&argc);
    printf("  cwd:       %s\n", cwd);
    printf("  uptime:    %lu ms\n", uptime_ms());
    printf("  arguments: %d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("    argv[%d] = \"%s\"\n", i, argv[i]);
    }
    return 0;
}
