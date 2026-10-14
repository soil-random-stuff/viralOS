/* meminfo: memory, process and CPU totals. */

#include <stdio.h>
#include <unistd.h>

int main(void) {
    struct os_sysinfo s;
    int err = sysinfo(&s);
    if (err < 0) {
        dprintf(STDERR_FILENO, "meminfo: %s\n", strerror(err));
        return 1;
    }
    uint64_t used = s.memory_total - s.memory_free;
    printf("Memory:      %6lu KiB total, %6lu KiB used, %6lu KiB free (%lu%% used)\n",
           s.memory_total / 1024, used / 1024, s.memory_free / 1024,
           s.memory_total ? used * 100 / s.memory_total : 0);
    printf("Kernel heap: %6lu KiB in use\n", s.kernel_heap_used / 1024);
    printf("Processes:   %u\n", s.processes);
    printf("CPUs:        %u\n", s.cpus);
    printf("Uptime:      %lu.%03lu s\n", s.uptime_ms / 1000, s.uptime_ms % 1000);
    return 0;
}
