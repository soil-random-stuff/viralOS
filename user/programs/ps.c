/* ps: list processes. */

#include <stdio.h>
#include <unistd.h>

static const char *state_name(uint32_t state) {
    switch (state) {
    case OS_PROC_RUNNING:  return "running";
    case OS_PROC_SLEEPING: return "sleeping";
    case OS_PROC_BLOCKED:  return "waiting";
    case OS_PROC_ZOMBIE:   return "exited";
    default:               return "?";
    }
}

int main(void) {
    printf("  PID  PARENT  USER      STATE     CPU(ms)  MEMORY  NAME\n");
    struct os_proc_info p;
    for (uint64_t i = 0; procinfo(i, &p) == 0; i++) {
        printf("%5ld  ", p.pid);
        if (p.parent) {
            printf("%6ld", p.parent);
        } else {
            printf("kernel");
        }
        printf("  %-9s %-8s %8lu  %4luK  %s\n", p.user[0] ? p.user : "-", state_name(p.state), p.cpu_ms,
               p.memory / 1024, p.name);
    }
    return 0;
}
