#include "drivers/pit.h"
#include "arch/x86_64/cpu.h"

#define PIT_BASE_HZ  1193182
#define PIT_CHANNEL2 0x42
#define PIT_COMMAND  0x43
#define PORT_B       0x61   /* bit 0: channel 2 gate, bit 1: speaker, bit 5: channel 2 output */

/*
 * Channel 2 is the one whose output can be read back through port 0x61.
 * In mode 0 the output goes low when the count is loaded and high again
 * when it reaches zero.
 */
void pit_oneshot_start(uint32_t microseconds) {
    uint32_t count = (uint32_t)((uint64_t)PIT_BASE_HZ * microseconds / 1000000);
    if (count > 0xFFFF) {
        count = 0xFFFF;
    }
    outb(PORT_B, (inb(PORT_B) & ~0x02) | 0x01);   /* speaker off, gate on */
    outb(PIT_COMMAND, 0xB0);                      /* channel 2, lo/hi byte, mode 0 */
    outb(PIT_CHANNEL2, count & 0xFF);
    outb(PIT_CHANNEL2, (count >> 8) & 0xFF);
}

void pit_oneshot_wait(void) {
    while (!(inb(PORT_B) & 0x20)) {
    }
}
