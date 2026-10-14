#include "arch/x86_64/pic.h"
#include "arch/x86_64/cpu.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01

/* Vectors the PICs are parked on; nothing should ever arrive there. */
#define PIC_PARKED_BASE 0xE0

void pic_disable(void) {
    /* Initialization sequence: ICW1..ICW4 to each chip. */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);  io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);  io_wait();
    outb(PIC1_DATA, PIC_PARKED_BASE);       io_wait();
    outb(PIC2_DATA, PIC_PARKED_BASE + 8);   io_wait();
    outb(PIC1_DATA, 1 << 2);                io_wait(); /* slave is on master IRQ 2 */
    outb(PIC2_DATA, 2);                     io_wait(); /* slave's cascade identity */
    outb(PIC1_DATA, ICW4_8086);             io_wait();
    outb(PIC2_DATA, ICW4_8086);             io_wait();

    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}
