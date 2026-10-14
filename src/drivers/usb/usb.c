/*
 * USB core: finding devices on the root ports and on hubs, giving them
 * addresses, reading their descriptors and handing them to a driver
 * (storage.c or hid.c). After Linux's drivers/usb/core:
 *
 *   hub.c       port reset and debounce, hub_port_init (addressing, the
 *               8-byte first look at the device descriptor), hub setup
 *               (power on the ports, poll their status)
 *   message.c   standard requests (GET_DESCRIPTOR, SET_CONFIGURATION)
 *
 * One kernel thread does everything: it polls the controllers for finished
 * transfers every 10 ms, and the ports for plugging/unplugging. Disk I/O
 * runs in other threads, so the whole stack sits behind one lock.
 */

#include <stdarg.h>

#include "drivers/usb/usb.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "sched/sched.h"
#include "sched/sync.h"
#include "drivers/timer.h"
#include "arch/x86_64/cpu.h"

#define POLL_MS        10
#define ROOT_SCAN_MS   100
#define HUB_SCAN_MS    250

/* Requests (ch9.h) */
#define GET_STATUS        0
#define CLEAR_FEATURE     1
#define SET_FEATURE       3
#define GET_DESCRIPTOR    6
#define SET_CONFIGURATION 9
#define DESC_DEVICE       1
#define DESC_CONFIG       2
#define DESC_HUB          0x29

/* Hub port features and status bits (ch11.h) */
#define PORT_RESET        4
#define PORT_POWER        8
#define C_PORT_CONNECTION 16
#define C_PORT_RESET      20
#define PS_CONNECTION     (1u << 0)
#define PS_LOW_SPEED      (1u << 9)
#define PS_HIGH_SPEED     (1u << 10)
#define PC_CONNECTION     (1u << 0)
#define PC_RESET          (1u << 4)

static struct mutex lock = MUTEX_INIT;

void usb_lock(void) {
    mutex_lock(&lock);
}

void usb_unlock(void) {
    mutex_unlock(&lock);
}

static struct usb_device devices[USB_MAX_DEVICES];
static struct usb_device *root[4][65];   /* [controller][port] */
static bool root_failed[4][65];          /* gave up on what's there until it's unplugged */

/* (Interrupts off around the three prints: one line, even with the disks thread printing too.) */
void usb_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uint64_t irq = irq_save();
    kprintf("       USB: ");
    kvprintf(fmt, ap);
    kprintf("\n");
    irq_restore(irq);
    va_end(ap);
}

static int trouble, inputs, disks;
static bool booting = true;

void usb_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uint64_t irq = irq_save();
    kprintf("[WARN] USB: ");
    kvprintf(fmt, ap);
    kprintf("\n");
    irq_restore(irq);
    va_end(ap);
    if (booting) {
        trouble++;
    }
}

void usb_note_disk(void) {
    disks++;
}

void usb_note_input(void) {
    inputs++;
}

static const char *speed_name(int speed) {
    switch (speed) {
    case USB_SPEED_LOW:   return "low speed";
    case USB_SPEED_FULL:  return "full speed";
    case USB_SPEED_HIGH:  return "high speed";
    default:              return "SuperSpeed";
    }
}

static void where(const struct usb_device *dev, char *out, size_t size) {
    /* "port 3" or "port 3.2.1" through hubs */
    int ports[8], n = 0;
    for (const struct usb_device *d = dev; d->parent && n < 8; d = d->parent) {
        ports[n++] = d->parent_port;
    }
    size_t at = 0;
    char tmp[12];
    int len = 0;
    int v = dev->root_port;
    do {
        tmp[len++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    const char *prefix = "port ";
    while (*prefix && at + 1 < size) {
        out[at++] = *prefix++;
    }
    while (len && at + 1 < size) {
        out[at++] = tmp[--len];
    }
    for (int i = n - 1; i >= 0 && at + 3 < size; i--) {
        out[at++] = '.';
        int p = ports[i];
        if (p >= 10) {
            out[at++] = (char)('0' + p / 10);
        }
        out[at++] = (char)('0' + p % 10);
    }
    out[at] = '\0';
}

/* --- devices ------------------------------------------------------------------------------------------------ */

static void remove_device(struct usb_device *dev) {
    if (!dev || !dev->used) {
        return;
    }
    for (int p = 1; p <= dev->hub_ports && p <= USB_HUB_MAX_PORTS; p++) {
        remove_device(dev->children[p]);
        dev->children[p] = NULL;
    }
    char at[32];
    where(dev, at, sizeof(at));
    usb_log("%s: %04x:%04x unplugged", at, dev->vendor, dev->product);
    hid_remove(dev);
    storage_remove(dev);
    xhci_free_device(dev);
    memset(dev, 0, sizeof(*dev));
}

static bool setup_hub(struct usb_device *dev, const char *at);

/* A device just appeared (its port is reset and enabled): address it, look at it, start a driver. */
static struct usb_device *enumerate(struct xhci *hc, int root_port, struct usb_device *parent, int parent_port,
                                    int speed) {
    struct usb_device *dev = NULL;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!devices[i].used) {
            dev = &devices[i];
            break;
        }
    }
    if (!dev) {
        usb_warn("too many devices");
        return NULL;
    }
    memset(dev, 0, sizeof(*dev));
    dev->used = true;
    dev->hc = hc;
    dev->speed = speed;
    dev->root_port = root_port;
    dev->parent = parent;
    dev->parent_port = parent_port;
    if (parent) {
        dev->depth = parent->depth + 1;
        dev->route = parent->route | (uint32_t)(parent_port > 15 ? 15 : parent_port) << (4 * parent->depth);
        if (speed != USB_SPEED_HIGH && speed != USB_SPEED_SUPER) {
            /* Low/full speed behind a high-speed hub: that hub's transaction translator does the talking. */
            if (parent->speed == USB_SPEED_HIGH) {
                dev->tt_slot = parent->slot;
                dev->tt_port = parent_port;
                dev->tt_multi = parent->tt_multi;
            } else {
                dev->tt_slot = parent->tt_slot;
                dev->tt_port = parent->tt_port;
                dev->tt_multi = parent->tt_multi;
            }
        }
    }
    char at[32];
    where(dev, at, sizeof(at));

    uint8_t desc[18];
    if (!xhci_address_device(dev)) {
        goto fail;
    }
    /* hub_port_init: first 8 bytes, to learn endpoint 0's real packet size. */
    if (xhci_control(dev, 0x80, GET_DESCRIPTOR, DESC_DEVICE << 8, 0, desc, 8) < 8) {
        usb_warn("%s: device doesn't answer (%s)", at, speed_name(speed));
        goto fail;
    }
    uint16_t mps = speed == USB_SPEED_SUPER ? (uint16_t)(1u << desc[7]) : desc[7];
    if (mps != dev->mps0 && !xhci_update_mps0(dev, mps)) {
        usb_warn("%s: couldn't set endpoint 0 size %u", at, mps);
        goto fail;
    }
    if (xhci_control(dev, 0x80, GET_DESCRIPTOR, DESC_DEVICE << 8, 0, desc, 18) < 18) {
        usb_warn("%s: no device descriptor", at);
        goto fail;
    }
    dev->device_class = desc[4];
    dev->vendor = (uint16_t)(desc[8] | desc[9] << 8);
    dev->product = (uint16_t)(desc[10] | desc[11] << 8);
    dev->str_manufacturer = desc[14];
    dev->str_product = desc[15];
    dev->str_serial = desc[16];

    /* The first configuration, whole. */
    static uint8_t config[4096];
    if (xhci_control(dev, 0x80, GET_DESCRIPTOR, DESC_CONFIG << 8, 0, config, 9) < 9) {
        usb_warn("%s: %04x:%04x: no configuration", at, dev->vendor, dev->product);
        goto fail;
    }
    int total = config[2] | config[3] << 8;
    total = total > (int)sizeof(config) ? (int)sizeof(config) : total;
    int got = xhci_control(dev, 0x80, GET_DESCRIPTOR, DESC_CONFIG << 8, 0, config, (uint16_t)total);
    if (got < 9 || xhci_control(dev, 0x00, SET_CONFIGURATION, config[5], 0, NULL, 0) < 0) {
        usb_warn("%s: %04x:%04x: couldn't configure it", at, dev->vendor, dev->product);
        goto fail;
    }
    usb_log("%s: %04x:%04x (%s, class %02x)", at, dev->vendor, dev->product, speed_name(speed), dev->device_class);

    if (dev->device_class == 0x09) {
        setup_hub(dev, at);
    } else if (!storage_probe(dev, config, got)) {
        hid_probe(dev, config, got);
    }
    return dev;

fail:
    xhci_free_device(dev);
    memset(dev, 0, sizeof(*dev));
    return NULL;
}

/* message.c usb_string: a string descriptor in US English, as printable ASCII (others become '?'). */
bool usb_string(struct usb_device *dev, uint8_t index, char *out, size_t size) {
    out[0] = '\0';
    if (!index || size < 2) {
        return false;
    }
    uint8_t buf[255];
    int got = xhci_control(dev, 0x80, GET_DESCRIPTOR, (uint16_t)(3 << 8 | index), 0x0409, buf, sizeof(buf));
    if (got < 4 || buf[1] != 3) {
        return false;
    }
    int len = buf[0] < got ? buf[0] : got;
    size_t o = 0;
    for (int i = 2; i + 1 < len && o + 1 < size; i += 2) {
        uint16_t c = (uint16_t)(buf[i] | buf[i + 1] << 8);
        out[o++] = c >= 0x20 && c < 0x7F ? (char)c : '?';
    }
    while (o && out[o - 1] == ' ') {
        o--;
    }
    out[o] = '\0';
    return o > 0;
}

/* --- hubs (hub.c, USB 2 only) ------------------------------------------------------------------------------- */

static bool hub_port_status(struct usb_device *hub, int port, uint16_t *status, uint16_t *change) {
    uint8_t s[4];
    if (xhci_control(hub, 0xA3, GET_STATUS, 0, (uint16_t)port, s, 4) < 4) {
        return false;
    }
    *status = (uint16_t)(s[0] | s[1] << 8);
    *change = (uint16_t)(s[2] | s[3] << 8);
    return true;
}

static void hub_scan(struct usb_device *hub) {
    for (int port = 1; port <= hub->hub_ports; port++) {
        uint16_t status, change;
        if (!hub_port_status(hub, port, &status, &change)) {
            return;   /* the hub went away; its own port will notice */
        }
        if (change & PC_CONNECTION) {
            xhci_control(hub, 0x23, CLEAR_FEATURE, C_PORT_CONNECTION, (uint16_t)port, NULL, 0);
        }
        struct usb_device *child = hub->children[port];
        uint16_t bit = (uint16_t)(1u << port);
        if (!(status & PS_CONNECTION)) {
            if (child) {
                remove_device(child);
                hub->children[port] = NULL;
            }
            hub->hub_failed &= (uint16_t)~bit;
            continue;
        }
        if ((child || (hub->hub_failed & bit)) && !(change & PC_CONNECTION)) {
            continue;   /* already running (or given up on) */
        }
        if (child) {
            remove_device(child);   /* replugged between two looks */
            hub->children[port] = NULL;
        }
        sched_sleep_ms(100);   /* debounce */
        xhci_control(hub, 0x23, SET_FEATURE, PORT_RESET, (uint16_t)port, NULL, 0);
        bool reset = false;
        for (int i = 0; i < 50 && !reset; i++) {
            sched_sleep_ms(10);
            reset = hub_port_status(hub, port, &status, &change) && (change & PC_RESET);
        }
        xhci_control(hub, 0x23, CLEAR_FEATURE, C_PORT_RESET, (uint16_t)port, NULL, 0);
        if (!reset || !(status & PS_CONNECTION)) {
            hub->hub_failed |= bit;
            continue;
        }
        sched_sleep_ms(10);   /* reset recovery */
        int speed = status & PS_LOW_SPEED ? USB_SPEED_LOW : status & PS_HIGH_SPEED ? USB_SPEED_HIGH : USB_SPEED_FULL;
        hub->children[port] = enumerate(hub->hc, hub->root_port, hub, port, speed);
        if (!hub->children[port]) {
            hub->hub_failed |= bit;
        }
    }
}

static bool setup_hub(struct usb_device *dev, const char *at) {
    if (dev->speed == USB_SPEED_SUPER) {
        /* A USB 3 hub's keyboards and mice show up on its USB 2 half, which is a hub of its own. */
        usb_log("%s: USB 3 hub (its USB 2 side does the keyboards and mice)", at);
        return false;
    }
    uint8_t d[9];
    if (xhci_control(dev, 0xA0, GET_DESCRIPTOR, DESC_HUB << 8, 0, d, sizeof(d)) < 7) {
        usb_warn("%s: no hub descriptor", at);
        return false;
    }
    int ports = d[2] > USB_HUB_MAX_PORTS ? USB_HUB_MAX_PORTS : d[2];
    if (!xhci_set_hub(dev, ports, false)) {
        usb_warn("%s: couldn't set the hub up", at);
        return false;
    }
    dev->hub_power_ms = d[5] * 2;
    for (int p = 1; p <= ports; p++) {
        xhci_control(dev, 0x23, SET_FEATURE, PORT_POWER, (uint16_t)p, NULL, 0);
    }
    sched_sleep_ms((uint64_t)(dev->hub_power_ms < 100 ? 100 : dev->hub_power_ms));
    usb_log("%s: hub with %d ports", at, ports);
    hub_scan(dev);
    return true;
}

/* --- root ports ------------------------------------------------------------------------------------------- */

static void root_scan(int c) {
    struct xhci *hc = xhci_at(c);
    for (int port = 1; port <= xhci_port_count(hc) && port <= 64; port++) {
        int speed;
        bool changed;
        bool connected = xhci_port_connected(hc, port, &changed);
        struct usb_device *dev = root[c][port];
        if (!connected) {
            if (dev) {
                remove_device(dev);
                root[c][port] = NULL;
            }
            root_failed[c][port] = false;
            continue;
        }
        if ((dev || root_failed[c][port]) && !changed) {
            continue;
        }
        if (dev) {
            remove_device(dev);
            root[c][port] = NULL;
        }
        sched_sleep_ms(100);   /* debounce */
        if (!xhci_port_reset(hc, port, &speed)) {
            usb_warn("port %d: couldn't enable it", port);
            root_failed[c][port] = true;
            continue;
        }
        root[c][port] = enumerate(hc, port, NULL, 0, speed);
        root_failed[c][port] = !root[c][port];
    }
}

void usb_interrupt_done(struct usb_device *dev, int dci, int code, uint32_t length) {
    hid_report(dev, dci, code, length);
}

static void usb_thread(void *arg) {
    (void)arg;
    uint64_t next_root = 0;
    for (;;) {
        usb_lock();
        xhci_poll();
        hid_tick();
        uint64_t now = uptime_ms();
        if (now >= next_root) {
            for (int c = 0; c < xhci_count(); c++) {
                root_scan(c);
            }
            next_root = now + ROOT_SCAN_MS;
        }
        for (int i = 0; i < USB_MAX_DEVICES; i++) {
            struct usb_device *d = &devices[i];
            if (d->used && d->is_hub && now >= d->hub_next_poll) {
                hub_scan(d);
                d->hub_next_poll = now + HUB_SCAN_MS;
            }
        }
        usb_unlock();
        sched_sleep_ms(POLL_MS);
    }
}

void usb_init(void) {
    if (!xhci_init_all()) {
        return;
    }
    /* Give devices a moment to connect (USB 3 links train), then look at every port now, so
       what's found shows in the boot log; afterwards the thread watches for changes. */
    sched_sleep_ms(300);
    usb_lock();
    for (int c = 0; c < xhci_count(); c++) {
        root_scan(c);
    }
    usb_unlock();
    booting = false;
    if (trouble) {
        kprintf("[WARN] USB had problems: keeping this on screen for 15 seconds (take a photo!)\n");
        sched_sleep_ms(15000);
    } else {
        kprintf("[ OK ] USB: %d keyboard/mouse interface%s, %d disk%s\n", inputs, inputs == 1 ? "" : "s", disks,
                disks == 1 ? "" : "s");
    }
    thread_create("usb", usb_thread, NULL);
}
