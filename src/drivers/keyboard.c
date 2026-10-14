/*
 * PS/2 keyboard. The 8042 controller translates to scancode set 1 by
 * default: one byte per make code, the same code | 0x80 on release, and an
 * 0xE0 prefix for "extended" keys (arrows, right ctrl, ...).
 */

#include <stddef.h>
#include <stdint.h>

#include "drivers/keyboard.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/idt.h"
#include "drivers/display.h"
#include "drivers/input.h"
#include "lib/panic.h"
#include "sched/sync.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_FROM_MOUSE  0x20

#define SC_BACKSPACE 0x0E

#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_CTRL     0x1D
#define SC_ALT      0x38
#define SC_CAPSLOCK 0x3A
#define SC_F12      0x58
#define SC_RELEASED 0x80
#define SC_EXTENDED 0xE0

static const char keymap_normal[] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*',
    0, ' ',
};

static const char keymap_shift[] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*',
    0, ' ',
};

_Static_assert(sizeof(keymap_normal) == sizeof(keymap_shift), "keymaps differ in size");

/* Single-producer (IRQ) / single-consumer (kernel) ring buffer. */
#define BUFFER_SIZE 128
static volatile char buffer[BUFFER_SIZE];
static volatile uint32_t head, tail;
static struct wait_queue readers = WAIT_QUEUE_INIT;
static bool (*ctrl_c_handler)(void);

#define CTRL_C 3

static bool shift, ctrl, alt, capslock, extended;

static void buffer_push(char c) {
    uint32_t next = (head + 1) % BUFFER_SIZE;
    if (next != tail) { /* drop keys if full */
        buffer[head] = c;
        head = next;
    }
}

/*
 * Keys behind the E0 prefix that we deliver, as the ANSI escape sequences
 * a terminal would send. Line editors (like the shell's) decode them.
 */
static const char *extended_sequence(uint8_t code) {
    switch (code) {
    case 0x48: return "\x1b[A";    /* up */
    case 0x50: return "\x1b[B";    /* down */
    case 0x4D: return "\x1b[C";    /* right */
    case 0x4B: return "\x1b[D";    /* left */
    case 0x47: return "\x1b[H";    /* home */
    case 0x4F: return "\x1b[F";    /* end */
    case 0x53: return "\x1b[3~";   /* delete */
    case 0x1C: return "\n";        /* keypad enter */
    case 0x35: return "/";         /* keypad slash */
    default:   return NULL;
    }
}

static char translate(uint8_t code) {
    if (code >= sizeof(keymap_normal)) {
        return 0;
    }
    char c = shift ? keymap_shift[code] : keymap_normal[code];
    if (capslock && c >= 'a' && c <= 'z') {
        c -= 'a' - 'A';
    } else if (capslock && c >= 'A' && c <= 'Z') {
        c += 'a' - 'A';
    }
    if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
        c &= 0x1F; /* Ctrl+C = 3, etc. */
    }
    return c;
}

uint32_t keyboard_modifiers(void) {
    return (shift ? OS_MOD_SHIFT : 0) | (ctrl ? OS_MOD_CTRL : 0) | (alt ? OS_MOD_ALT : 0);
}

/* While a program owns the screen, every key goes to it as an input event. */
static void send_key_event(uint8_t code, bool was_extended, bool released) {
    if (!released && ctrl && alt && code == SC_BACKSPACE && !was_extended) {
        display_zap();   /* emergency exit from a graphical program */
        return;
    }
    struct os_input_event e = {
        .type = OS_EVENT_KEY,
        .modifiers = keyboard_modifiers(),
        .key = code | (was_extended ? 0x100u : 0),
        .pressed = !released,
    };
    if (!released) {
        if (!was_extended) {
            e.ch = (uint8_t)translate(code);
        } else if (code == 0x1C || code == 0x35) {   /* keypad Enter and / */
            e.ch = (uint8_t)extended_sequence(code)[0];
        }
    }
    input_push(&e);
}

static void handle_key(uint8_t code, bool was_extended, bool released);

static void keyboard_irq(struct interrupt_frame *frame) {
    (void)frame;
    uint8_t status = inb(PS2_STATUS);
    if (!(status & PS2_STATUS_OUTPUT_FULL) || (status & PS2_STATUS_FROM_MOUSE)) {
        return;   /* nothing for us (mouse bytes are IRQ 12's business) */
    }
    uint8_t sc = inb(PS2_DATA);

    if (sc == SC_EXTENDED) {
        extended = true;
        return;
    }
    bool released = sc & SC_RELEASED;
    uint8_t code = sc & ~SC_RELEASED;
    bool was_extended = extended;
    extended = false;
    handle_key(code, was_extended, released);
}

/* A key in scancode set 1 terms, from whichever keyboard (PS/2 or USB). */
static void handle_key(uint8_t code, bool was_extended, bool released) {
    bool modifier = true;
    switch (code) {
    case SC_LSHIFT:
    case SC_RSHIFT:
        /* E0 2A / E0 36 are "fake shifts" some keys send around themselves. */
        if (was_extended) {
            return;
        }
        shift = !released;
        break;
    case SC_CTRL:      /* left, or right with E0 */
        ctrl = !released;
        break;
    case SC_ALT:
        alt = !released;
        break;
    case SC_CAPSLOCK:
        if (!released) {
            capslock = !capslock;
        }
        break;
    default:
        modifier = false;
        break;
    }

    /* Ctrl+Alt+Shift+F12: stop the system on purpose, with the panic report (like Windows'
       CrashOnCtrlScroll): for trying the report out, or for a look at a system that hangs. */
    if (!released && !was_extended && ctrl && alt && shift && code == SC_F12) {
        panic("stopped on purpose from the keyboard (Ctrl+Alt+Shift+F12)");
    }

    if (display_claimed()) {
        send_key_event(code, was_extended, released);
        return;
    }
    if (modifier) {
        return;
    }

    if (released) {
        return;
    }
    if (was_extended) {
        const char *seq = extended_sequence(code);
        if (seq) {
            while (*seq) {
                buffer_push(*seq++);
            }
            wait_queue_wake_all(&readers);
        }
        return;
    }

    char c = translate(code);
    if (c == CTRL_C && ctrl_c_handler && ctrl_c_handler()) {
        wait_queue_wake_all(&readers);   /* so a killed reader notices */
    } else if (c) {
        buffer_push(c);   /* includes an unclaimed Ctrl+C, as character 3 */
        wait_queue_wake_all(&readers);
    }
}

void keyboard_feed(uint16_t key, bool pressed) {
    uint64_t flags = irq_save();   /* the PS/2 interrupt uses the same state */
    handle_key((uint8_t)key, key & 0x100, !pressed);
    irq_restore(flags);
}

void keyboard_set_ctrl_c_handler(bool (*handler)(void)) {
    ctrl_c_handler = handler;
}

void keyboard_wake_readers(void) {
    wait_queue_wake_all(&readers);
}

void keyboard_init(void) {
    /* Throw away anything the firmware left in the controller's output buffer. (Bounded: on
       machines without a PS/2 controller the status port reads 0xFF, "always full".) */
    for (int i = 0; i < 64 && (inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL); i++) {
        inb(PS2_DATA);
    }
    irq_install(1, keyboard_irq);
}

bool keyboard_try_getchar(char *out) {
    if (head == tail) {
        return false;
    }
    *out = buffer[tail];
    tail = (tail + 1) % BUFFER_SIZE;
    return true;
}

char keyboard_getchar(void) {
    char c;
    uint64_t irq = irq_save();
    /* Checking and sleeping both happen with interrupts off, so a key can't
       arrive in between and leave us asleep with input waiting. */
    while (!keyboard_try_getchar(&c)) {
        wait_queue_sleep(&readers);
    }
    irq_restore(irq);
    return c;
}

bool keyboard_getchar_unless(char *out, bool (*abort)(void)) {
    uint64_t irq = irq_save();
    bool got;
    while (!(got = keyboard_try_getchar(out)) && !abort()) {
        wait_queue_sleep(&readers);
    }
    irq_restore(irq);
    return got;
}
