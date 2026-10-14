/*
 * USB keyboards and mice in the "boot protocol": the simple fixed reports
 * every keyboard and mouse supports for the BIOS. After Linux's
 * drivers/hid/usbhid/usbkbd.c and usbmouse.c:
 *
 *   keyboard   8 bytes: modifier bits, a reserved byte, up to 6 keys held
 *              (HID usage codes). usb_kbd_keycode[] turns them into Linux
 *              key codes, which for ordinary keys are the PC scancodes our
 *              keyboard code speaks; the extended keys are mapped below.
 *   mouse      buttons, dx, dy (and on most mice a 4th byte, the wheel)
 *
 * USB keyboards don't repeat keys themselves (PS/2 ones do), so held keys
 * repeat here: after 500 ms, 30 times a second.
 */

#include "drivers/usb/usb.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/timer.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"

#define REPEAT_DELAY_MS 500
#define REPEAT_EVERY_MS 33

enum { KIND_KEYBOARD = 1, KIND_MOUSE = 2 };

struct hid {
    struct usb_device *dev;
    int dci, kind;
    uint16_t size;            /* report buffer (the endpoint's packet size) */
    uint8_t *buf;
    uint64_t phys;
    uint8_t old[8];           /* the keyboard's previous report */
};

/* HID usage -> Linux key code (usbkbd.c). */
static const unsigned char usb_kbd_keycode[256] = {
      0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
     50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
      4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
     27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
     65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
    105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
     72, 73, 82, 83, 86,127,116,117,183,184,185,186,187,188,189,190,
    191,192,193,194,134,138,130,132,128,129,131,137,133,135,136,113,
    115,114,  0,  0,  0,121,  0, 89, 93,124, 92, 94, 95,  0,  0,  0,
    122,123, 90, 91, 85,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     29, 42, 56,125, 97, 54,100,126,164,166,165,163,161,115,114,113,
    150,158,159,128,136,177,178,176,142,152,173,140
};

/* Linux key code -> our key code (scancode set 1, | 0x100 for E0 keys); 0 if we have no use for it. */
static uint16_t os_key(unsigned linux_code) {
    switch (linux_code) {
    case 96:  return 0x11C;   /* keypad Enter */
    case 97:  return 0x11D;   /* right Ctrl */
    case 98:  return 0x135;   /* keypad / */
    case 99:  return 0x137;   /* Print Screen */
    case 100: return 0x138;   /* right Alt */
    case 102: return 0x147;   /* Home */
    case 103: return 0x148;   /* Up */
    case 104: return 0x149;   /* Page Up */
    case 105: return 0x14B;   /* Left */
    case 106: return 0x14D;   /* Right */
    case 107: return 0x14F;   /* End */
    case 108: return 0x150;   /* Down */
    case 109: return 0x151;   /* Page Down */
    case 110: return 0x152;   /* Insert */
    case 111: return 0x153;   /* Delete */
    case 125: return 0x15B;   /* left Windows */
    case 126: return 0x15C;   /* right Windows */
    case 127: return 0x15D;   /* Menu */
    default:  return linux_code >= 1 && linux_code <= 88 ? (uint16_t)linux_code : 0;
    }
}

static struct hid *hids[16];
static uint16_t repeat_key;
static uint64_t repeat_at;

static void key(uint8_t usage, bool pressed) {
    uint16_t k = os_key(usb_kbd_keycode[usage]);
    if (!k) {
        return;
    }
    keyboard_feed(k, pressed);
    bool modifier = usage >= 224;
    if (pressed && !modifier) {
        repeat_key = k;
        repeat_at = uptime_ms() + REPEAT_DELAY_MS;
    } else if (!pressed && k == repeat_key) {
        repeat_key = 0;
    }
}

static bool contains(const uint8_t *keys, uint8_t usage) {
    for (int i = 2; i < 8; i++) {
        if (keys[i] == usage) {
            return true;
        }
    }
    return false;
}

/* usb_kbd_irq: what changed between two reports. */
static void keyboard_report(struct hid *h, const uint8_t *now) {
    if (now[2] == 1) {
        return;   /* "rollover": too many keys held to say which */
    }
    for (int i = 0; i < 8; i++) {
        bool was = (h->old[0] >> i) & 1, is = (now[0] >> i) & 1;
        if (was != is) {
            key((uint8_t)(224 + i), is);
        }
    }
    for (int i = 2; i < 8; i++) {
        if (h->old[i] > 3 && !contains(now, h->old[i])) {
            key(h->old[i], false);
        }
    }
    for (int i = 2; i < 8; i++) {
        if (now[i] > 3 && !contains(h->old, now[i])) {
            key(now[i], true);
        }
    }
    memcpy(h->old, now, 8);
}

/* usb_mouse_irq */
static void mouse_report(const uint8_t *d, uint32_t length) {
    uint32_t buttons = d[0] & 7;   /* left, right, middle: the same bits as OS_MOUSE_* */
    int32_t wheel = length >= 4 ? -(int8_t)d[3] : 0;   /* HID: positive = away from you */
    mouse_feed((int8_t)d[1], (int8_t)d[2], buttons, wheel);
}

void hid_report(struct usb_device *dev, int dci, int code, uint32_t residual) {
    struct hid *h = dev->hid[dci];
    if (!h) {
        return;
    }
    if (code == 1 || code == 13) {   /* success, short packet */
        uint32_t got = residual <= h->size ? h->size - residual : 0;
        if (h->kind == KIND_KEYBOARD && got >= 8) {
            keyboard_report(h, h->buf);
        } else if (h->kind == KIND_MOUSE && got >= 3) {
            mouse_report(h->buf, got);
        }
    }
    xhci_queue_interrupt(dev, dci, h->phys, h->size);   /* and the next one */
}

void hid_tick(void) {
    if (repeat_key && uptime_ms() >= repeat_at) {
        keyboard_feed(repeat_key, true);
        repeat_at += REPEAT_EVERY_MS;
    }
}

void hid_probe(struct usb_device *dev, const uint8_t *config, int length) {
    int kind = 0, iface = -1;
    for (int i = 0; i + 2 <= length && config[i] >= 2; i += config[i]) {
        const uint8_t *d = config + i;
        if (d[1] == 4 && d[0] >= 9) {   /* interface */
            iface = d[2];
            kind = d[5] == 3 && d[6] == 1 ? d[7] : 0;   /* HID, boot subclass: 1 keyboard, 2 mouse */
            continue;
        }
        if (d[1] != 5 || d[0] < 7 || (kind != KIND_KEYBOARD && kind != KIND_MOUSE)) {
            continue;
        }
        uint8_t address = d[2], attributes = d[3], interval = d[6];
        uint16_t mps = (uint16_t)(d[4] | d[5] << 8);
        if (!(address & 0x80) || (attributes & 3) != 3) {
            continue;   /* not an interrupt IN endpoint */
        }
        int found = kind;
        kind = 0;   /* one endpoint per interface */
        /* SET_PROTOCOL(boot), SET_IDLE(0): the fixed reports, only when something changes. */
        xhci_control(dev, 0x21, 0x0B, 0, (uint16_t)iface, NULL, 0);
        xhci_control(dev, 0x21, 0x0A, 0, (uint16_t)iface, NULL, 0);   /* (often stalls; that's fine) */
        int dci = xhci_add_interrupt_in(dev, address, mps, interval);
        if (dci < 0) {
            continue;
        }
        struct hid *h = kzalloc(sizeof(*h));
        uint64_t phys = pmm_alloc_contiguous(1, 0x100000000ull);
        int slot = -1;
        for (int s = 0; s < 16; s++) {
            if (!hids[s]) {
                slot = s;
                break;
            }
        }
        if (!h || !phys || slot < 0) {
            kfree(h);
            if (phys) {
                pmm_free_page(phys);
            }
            continue;
        }
        h->dev = dev;
        h->dci = dci;
        h->kind = found;
        h->size = (uint16_t)((mps & 0x7FF) > 64 ? 64 : (mps & 0x7FF));
        h->phys = phys;
        h->buf = phys_to_virt(phys);
        memset(h->buf, 0, 4096);
        hids[slot] = h;
        dev->hid[dci] = h;
        usb_log("  %s ready", found == KIND_KEYBOARD ? "keyboard" : "mouse");
        usb_note_input();
        xhci_queue_interrupt(dev, dci, h->phys, h->size);
    }
}

void hid_remove(struct usb_device *dev) {
    for (int s = 0; s < 16; s++) {
        struct hid *h = hids[s];
        if (!h || h->dev != dev) {
            continue;
        }
        if (h->kind == KIND_KEYBOARD) {
            /* Let go of whatever it was holding (Shift stuck down otherwise). */
            static const uint8_t none[8];
            keyboard_report(h, none);
        }
        pmm_free_page(h->phys);
        kfree(h);
        hids[s] = NULL;
    }
    memset(dev->hid, 0, sizeof(dev->hid));
}
