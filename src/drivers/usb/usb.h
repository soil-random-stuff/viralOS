#pragma once

/*
 * USB: xHCI host controllers (xhci.c), device enumeration and hubs
 * (usb.c), keyboards and mice (hid.c). Everything runs in one kernel
 * thread ("usb") that polls the controllers' event rings; no interrupts.
 *
 * Shaped after Linux's drivers/usb (host/xhci*.c, core/hub.c, core/message.c,
 * storage/transport.c) and drivers/hid/usbhid (usbkbd.c, usbmouse.c), much
 * simplified: control, interrupt and bulk transfers, one configuration, USB 2 hubs.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Speeds as xHCI numbers them (PORTSC / slot context "speed"). */
enum { USB_SPEED_FULL = 1, USB_SPEED_LOW = 2, USB_SPEED_HIGH = 3, USB_SPEED_SUPER = 4 };

#define USB_MAX_DEVICES   32
#define USB_HUB_MAX_PORTS 15

struct xhci;
struct ring;

struct usb_device {
    bool used;
    struct xhci *hc;
    int slot;                     /* xHCI slot id */
    int speed;
    int root_port;                /* 1-based, on hc */
    uint32_t route;               /* route string through hubs */
    int depth;                    /* 0: on a root port */
    struct usb_device *parent;    /* the hub it's on (NULL: root port) */
    int parent_port;
    int tt_slot, tt_port;         /* low/full speed behind a high-speed hub: its transaction translator */
    bool tt_multi;

    uint16_t vendor, product;
    uint8_t device_class;
    uint8_t str_manufacturer, str_product, str_serial;   /* string descriptor indexes (0: none) */
    uint16_t mps0;                /* endpoint 0 max packet size */

    /* xHCI contexts and transfer rings (by device context index). */
    void *in_ctx, *out_ctx;
    uint64_t in_ctx_phys, out_ctx_phys;
    struct ring *rings[32];
    int max_dci;
    void *buffer;                 /* a DMA page for control transfers */
    uint64_t buffer_phys;

    /* A hub. */
    bool is_hub;
    int hub_ports;
    int hub_power_ms;
    struct usb_device *children[USB_HUB_MAX_PORTS + 1];
    uint64_t hub_next_poll;
    uint16_t hub_failed;          /* ports whose device couldn't be set up (until unplugged) */

    /* Endpoint 0's latest transfer event, for xhci_control (filled by the event poller). */
    bool ctrl_event_seen;
    uint64_t ctrl_event_trb;
    uint32_t ctrl_event_code, ctrl_event_residual;

    /* A keyboard / mouse (hid.c), by interrupt endpoint's DCI. */
    void *hid[32];

    /* Bulk endpoints (storage.c): their latest transfer event, for xhci_bulk. */
    bool bulk[32];
    struct {
        bool seen;
        uint64_t trb;
        uint32_t code, residual;
    } bulk_event[32];
    void *storage;
};

/* --- xhci.c: what the core uses ------------------------------------------------------------------ */

int xhci_init_all(void);                         /* finds and starts every controller; returns how many */
int xhci_count(void);
struct xhci *xhci_at(int i);
int xhci_port_count(struct xhci *hc);
/* Is something plugged in? *replugged: the connection changed since we last looked (unplugged
   and plugged back, maybe). Acknowledges the port's change bits. */
bool xhci_port_connected(struct xhci *hc, int port, bool *replugged);
bool xhci_port_reset(struct xhci *hc, int port, int *speed);   /* reset, then enabled? */

/* Give the device a slot and an address (enumeration's first step). */
bool xhci_address_device(struct usb_device *dev);
/* Endpoint 0's real max packet size, once the device descriptor says it. */
bool xhci_update_mps0(struct usb_device *dev, uint16_t mps);
/* Tell the controller this device is a hub (ports, multi-TT). */
bool xhci_set_hub(struct usb_device *dev, int ports, bool multi_tt);
/* Add an interrupt IN endpoint; returns its DCI or -1. */
int xhci_add_interrupt_in(struct usb_device *dev, uint8_t address, uint16_t mps, uint8_t interval);
/* Queue a read on an interrupt endpoint; completion comes to usb_interrupt_done. */
bool xhci_queue_interrupt(struct usb_device *dev, int dci, uint64_t buffer_phys, uint32_t length);
/* Add a bulk endpoint pair (address bit 7 = IN); burst = SuperSpeed companion bMaxBurst (else 0).
   Returns false on failure; the DCIs land in *in_dci / *out_dci. */
bool xhci_add_bulk(struct usb_device *dev, uint8_t in_addr, uint16_t in_mps, uint8_t in_burst, uint8_t out_addr,
                   uint16_t out_mps, uint8_t out_burst, int *in_dci, int *out_dci);
/* One bulk transfer to/from DMA memory, waiting for it. Returns 0 (with *actual bytes moved), USB_STALL
   (the endpoint halted; the controller side is already reset, the caller clears the device's halt) or
   -1 (error or timeout; the transfer is cancelled). */
#define USB_STALL (-2)
int xhci_bulk(struct usb_device *dev, int dci, uint64_t buffer_phys, uint32_t length, uint32_t *actual,
              int timeout_ms);
/* Reset an endpoint that halted, and skip what was queued on it. */
void xhci_reset_endpoint(struct usb_device *dev, int dci);
/* DMA memory below the controller's address limit (zeroed); 0 on failure. */
uint64_t xhci_dma_alloc(struct usb_device *dev, int pages, void **virt);
void xhci_dma_free(uint64_t phys, int pages);
void xhci_free_device(struct usb_device *dev);   /* disable its slot, free its memory */

/* A control transfer; returns bytes transferred (IN) / 0, or -1 on error (a stall included). */
int xhci_control(struct usb_device *dev, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                 void *data, uint16_t length);
/* Handle whatever the controllers have posted (transfers, port changes). */
void xhci_poll(void);

/* --- usb.c ----------------------------------------------------------------------------------------------- */

void usb_init(void);
/* An interrupt IN transfer finished (code: xHCI completion code, length: bytes received). */
void usb_interrupt_done(struct usb_device *dev, int dci, int code, uint32_t length);
void usb_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Like usb_log, for things that went wrong: at boot, the log then stays on screen a while. */
void usb_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void usb_note_input(void);   /* a keyboard or mouse is working */
void usb_note_disk(void);    /* a disk is working */
/* Read string descriptor `index` (US English) as ASCII; false if there's none. */
bool usb_string(struct usb_device *dev, uint8_t index, char *out, size_t size);
/* One lock over the whole USB stack (controllers, rings, devices). The usb thread holds it while it
   works; other threads (disk I/O) take it around each transfer. */
void usb_lock(void);
void usb_unlock(void);

/* --- storage.c: USB mass storage (Bulk-Only Transport + SCSI) -------------------------------------- */

/* Look through a configuration for a mass storage interface; true if it took the device. */
bool storage_probe(struct usb_device *dev, const uint8_t *config, int length);
void storage_remove(struct usb_device *dev);

/* --- hid.c ----------------------------------------------------------------------------------------------- */

/* Look through a configuration for boot keyboards/mice and start them. */
void hid_probe(struct usb_device *dev, const uint8_t *config, int length);
void hid_report(struct usb_device *dev, int dci, int code, uint32_t length);
void hid_remove(struct usb_device *dev);
void hid_tick(void);   /* key repeat */
