/* 16550 UART on COM1. In QEMU this is what `-serial stdio` prints. */

#include "drivers/serial.h"
#include "arch/x86_64/cpu.h"

#define COM1 0x3F8

enum {
    REG_DATA        = 0, /* DLAB=0: data, DLAB=1: divisor low  */
    REG_INT_ENABLE  = 1, /* DLAB=0: IER,  DLAB=1: divisor high */
    REG_FIFO_CTRL   = 2,
    REG_LINE_CTRL   = 3,
    REG_MODEM_CTRL  = 4,
    REG_LINE_STATUS = 5,
};

#define LSR_TX_EMPTY 0x20

void serial_init(void) {
    outb(COM1 + REG_INT_ENABLE, 0x00);  /* no UART interrupts */
    outb(COM1 + REG_LINE_CTRL,  0x80);  /* DLAB on to set the baud divisor */
    outb(COM1 + REG_DATA,       0x01);  /* divisor 1 = 115200 baud */
    outb(COM1 + REG_INT_ENABLE, 0x00);
    outb(COM1 + REG_LINE_CTRL,  0x03);  /* 8 data bits, no parity, 1 stop bit */
    outb(COM1 + REG_FIFO_CTRL,  0xC7);  /* enable + clear FIFOs, 14-byte threshold */
    outb(COM1 + REG_MODEM_CTRL, 0x03);  /* DTR + RTS */
}

static void serial_put_raw(char c) {
    while (!(inb(COM1 + REG_LINE_STATUS) & LSR_TX_EMPTY)) {
    }
    outb(COM1 + REG_DATA, (uint8_t)c);
}

void serial_putc(char c) {
    if (c == '\n') {
        serial_put_raw('\r');
    }
    serial_put_raw(c);
}

void serial_write(const char *s) {
    while (*s) {
        serial_putc(*s++);
    }
}
