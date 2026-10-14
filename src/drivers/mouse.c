#include <stdint.h>

#include "drivers/mouse.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/idt.h"
#include "drivers/display.h"
#include "drivers/input.h"
#include "drivers/keyboard.h"

/* --- 8042 controller ------------------------------------------------------------ */

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL  0x02

#define CMD_READ_CONFIG   0x20
#define CMD_WRITE_CONFIG  0x60
#define CMD_ENABLE_AUX    0xA8
#define CMD_WRITE_AUX     0xD4   /* next data byte goes to the mouse */

#define CONFIG_AUX_IRQ    0x02
#define CONFIG_AUX_CLOCK_OFF 0x20

#define MOUSE_SET_DEFAULTS 0xF6
#define MOUSE_ENABLE       0xF4
#define MOUSE_SAMPLE_RATE  0xF3
#define MOUSE_GET_ID       0xF2
#define MOUSE_ACK          0xFA

static void wait_writable(void) {
    for (int i = 0; i < 100000 && (inb(PS2_STATUS) & STATUS_INPUT_FULL); i++) {
    }
}

static int read_data(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL) {
            return inb(PS2_DATA);
        }
    }
    return -1;
}

static void controller_command(uint8_t cmd) {
    wait_writable();
    outb(PS2_COMMAND, cmd);
}

static void controller_data(uint8_t value) {
    wait_writable();
    outb(PS2_DATA, value);
}

static bool mouse_command(uint8_t value) {
    controller_command(CMD_WRITE_AUX);
    controller_data(value);
    return read_data() == MOUSE_ACK;
}

/* --- VMware / QEMU absolute pointer ("vmmouse") --------------------------------- */

#define VMWARE_MAGIC 0x564D5868   /* "VMXh" */
#define VMWARE_PORT  0x5658

#define VMCALL_GET_VERSION       10
#define VMCALL_ABSPOINTER_DATA   39
#define VMCALL_ABSPOINTER_STATUS 40
#define VMCALL_ABSPOINTER_CMD    41

#define VMMOUSE_ENABLE           0x45414552
#define VMMOUSE_REQUEST_ABSOLUTE 0x53424152
#define VMMOUSE_VERSION_ID       0x3442554a

#define VMMOUSE_LEFT   0x20
#define VMMOUSE_RIGHT  0x10
#define VMMOUSE_MIDDLE 0x08

struct vmcall {
    uint32_t eax, ebx, ecx, edx;
};

/* The "backdoor": an `in` from a magic port that the hypervisor intercepts. */
static struct vmcall vmware_call(uint32_t command, uint32_t argument) {
    struct vmcall r = { VMWARE_MAGIC, argument, command, VMWARE_PORT };
    __asm__ volatile ("inl %%dx, %%eax"
                      : "+a"(r.eax), "+b"(r.ebx), "+c"(r.ecx), "+d"(r.edx) : : "memory");
    return r;
}

static bool vmmouse_enable(void) {
    if (vmware_call(VMCALL_GET_VERSION, 0xFFFFFFFF).ebx != VMWARE_MAGIC) {
        return false;   /* not running under VMware/QEMU with vmport */
    }
    vmware_call(VMCALL_ABSPOINTER_CMD, VMMOUSE_ENABLE);
    if ((vmware_call(VMCALL_ABSPOINTER_STATUS, 0).eax & 0xFFFF) == 0) {
        return false;
    }
    if (vmware_call(VMCALL_ABSPOINTER_DATA, 1).eax != VMMOUSE_VERSION_ID) {
        return false;
    }
    vmware_call(VMCALL_ABSPOINTER_CMD, VMMOUSE_REQUEST_ABSOLUTE);
    return true;
}

/* --- state ------------------------------------------------------------------------- */

static bool present;
static bool absolute;
static int packet_size = 3;   /* 4 with a scroll wheel */
static uint8_t packet[4];
static int packet_len;
static int32_t pos_x, pos_y;
static uint32_t buttons;

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void report(int32_t x, int32_t y, uint32_t new_buttons, int32_t wheel) {
    int32_t w = (int32_t)display_width(), h = (int32_t)display_height();
    x = clamp(x, 0, w - 1);
    y = clamp(y, 0, h - 1);
    if (x == pos_x && y == pos_y && new_buttons == buttons && wheel == 0) {
        return;
    }
    struct os_input_event e = {
        .type = OS_EVENT_MOUSE,
        .modifiers = keyboard_modifiers(),
        .buttons = new_buttons,
        .x = x,
        .y = y,
        .dx = x - pos_x,
        .dy = y - pos_y,
        .wheel = wheel,
    };
    pos_x = x;
    pos_y = y;
    buttons = new_buttons;
    input_push(&e);
}

static void poll_vmmouse(void) {
    uint32_t status = vmware_call(VMCALL_ABSPOINTER_STATUS, 0).eax;
    if ((status & 0xFFFF0000) == 0xFFFF0000) {
        absolute = vmmouse_enable();   /* the device reset itself; try again */
        return;
    }
    uint32_t queued = status & 0xFFFF;
    while (queued >= 4) {
        struct vmcall d = vmware_call(VMCALL_ABSPOINTER_DATA, 4);
        queued -= 4;
        uint32_t b = 0;
        b |= (d.eax & VMMOUSE_LEFT) ? OS_MOUSE_LEFT : 0;
        b |= (d.eax & VMMOUSE_RIGHT) ? OS_MOUSE_RIGHT : 0;
        b |= (d.eax & VMMOUSE_MIDDLE) ? OS_MOUSE_MIDDLE : 0;
        /* Coordinates come scaled to 0..65535 across the screen. */
        int32_t x = (int32_t)((uint64_t)d.ebx * display_width() / 65536);
        int32_t y = (int32_t)((uint64_t)d.ecx * display_height() / 65536);
        report(x, y, b, (int8_t)(d.edx & 0xFF));
    }
}

/* A complete PS/2 packet: flags, dx, dy (+ wheel). */
static void handle_packet(void) {
    uint8_t flags = packet[0];
    if (flags & 0xC0) {
        return;   /* overflow: the numbers are meaningless */
    }
    int32_t dx = packet[1] - ((flags << 4) & 0x100);   /* 9-bit two's complement */
    int32_t dy = packet[2] - ((flags << 3) & 0x100);
    int32_t wheel = packet_size == 4 ? (int8_t)packet[3] : 0;
    uint32_t b = 0;
    b |= (flags & 1) ? OS_MOUSE_LEFT : 0;
    b |= (flags & 2) ? OS_MOUSE_RIGHT : 0;
    b |= (flags & 4) ? OS_MOUSE_MIDDLE : 0;
    report(pos_x + dx, pos_y - dy, b, wheel);   /* PS/2 counts y upwards */
}

static void mouse_irq(struct interrupt_frame *frame) {
    (void)frame;
    if (!(inb(PS2_STATUS) & STATUS_OUTPUT_FULL)) {
        return;
    }
    uint8_t byte = inb(PS2_DATA);

    if (absolute) {
        /* The PS/2 bytes only announce that the vmmouse queue has news. */
        poll_vmmouse();
        return;
    }
    if (packet_len == 0 && !(byte & 0x08)) {
        return;   /* bit 3 is always set in the first byte: we're out of sync */
    }
    packet[packet_len++] = byte;
    if (packet_len == packet_size) {
        packet_len = 0;
        handle_packet();
    }
}

void mouse_feed(int32_t dx, int32_t dy, uint32_t new_buttons, int32_t wheel) {
    uint64_t flags = irq_save();   /* the PS/2 interrupt uses the same state */
    report(pos_x + dx, pos_y + dy, new_buttons, wheel);
    irq_restore(flags);
}

void mouse_init(void) {
    uint64_t irq = irq_save();

    controller_command(CMD_ENABLE_AUX);
    controller_command(CMD_READ_CONFIG);
    int config = read_data();
    if (config < 0) {
        irq_restore(irq);
        return;
    }
    config = (config | CONFIG_AUX_IRQ) & ~CONFIG_AUX_CLOCK_OFF;
    controller_command(CMD_WRITE_CONFIG);
    controller_data((uint8_t)config);

    if (!mouse_command(MOUSE_SET_DEFAULTS)) {
        irq_restore(irq);
        return;   /* no mouse answered */
    }

    /* The IntelliMouse handshake: this exact sequence of sample rates turns
       on the scroll wheel, and the device ID changes from 0 to 3. */
    mouse_command(MOUSE_SAMPLE_RATE); mouse_command(200);
    mouse_command(MOUSE_SAMPLE_RATE); mouse_command(100);
    mouse_command(MOUSE_SAMPLE_RATE); mouse_command(80);
    mouse_command(MOUSE_GET_ID);
    int id = read_data();
    packet_size = (id == 3 || id == 4) ? 4 : 3;

    mouse_command(MOUSE_ENABLE);
    present = true;
    absolute = vmmouse_enable();

    pos_x = (int32_t)display_width() / 2;
    pos_y = (int32_t)display_height() / 2;
    irq_restore(irq);

    irq_install(12, mouse_irq);
}

bool mouse_present(void) {
    return present;
}

bool mouse_absolute(void) {
    return absolute;
}
