/*
 * xHCI (USB 3 host controller), after Linux's drivers/usb/host:
 *
 *   pci-quirks.c   taking the controller from the BIOS (quirk_usb_handoff_xhci),
 *                  Intel's EHCI -> xHCI port switch (usb_enable_intel_xhci_ports)
 *   xhci.c         reset, run (xhci_reset, xhci_run), addressing devices
 *   xhci-mem.c     the device context array, scratchpad, rings, contexts (xhci_mem_init),
 *                  endpoint intervals (xhci_get_endpoint_interval)
 *   xhci-ring.c    TRBs, cycle bits, link TRBs, doorbells, the event ring
 *   xhci-hub.c     root ports (xhci_port_state_to_neutral, port reset)
 *
 * Much reduced: one interrupter, polled (no interrupts), one-segment rings,
 * control, interrupt and bulk transfers (no streams, no isochronous).
 */

#include "drivers/usb/usb.h"
#include "drivers/pci.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include "drivers/timer.h"

/* --- registers ---------------------------------------------------------------------------------------- */

/* Capability registers */
#define CAPLENGTH   0x00
#define HCSPARAMS1  0x04
#define HCSPARAMS2  0x08
#define HCCPARAMS1  0x10
#define DBOFF       0x14
#define RTSOFF      0x18

/* Operational registers (after CAPLENGTH) */
#define USBCMD      0x00
#define  CMD_RUN     (1u << 0)
#define  CMD_RESET   (1u << 1)
#define  CMD_INTE    (1u << 2)
#define USBSTS      0x04
#define  STS_HALT    (1u << 0)
#define  STS_CNR     (1u << 11)
#define PAGESIZE    0x08
#define CRCR        0x18
#define DCBAAP      0x30
#define CONFIG      0x38
#define PORTSC(n)   (0x400 + 0x10 * ((n) - 1))

/* PORTSC bits (xhci-port.h) */
#define PORT_CCS    (1u << 0)
#define PORT_PED    (1u << 1)
#define PORT_PR     (1u << 4)
#define PORT_PLS(p) (((p) >> 5) & 0xF)
#define PORT_PP     (1u << 9)
#define PORT_SPEED(p) (((p) >> 10) & 0xF)
#define PORT_CSC    (1u << 17)
#define PORT_PRC    (1u << 21)
#define PORT_WPR    (1u << 31)
#define PORT_CHANGES (0x7Fu << 17)
/* Bits to write back unchanged (xhci_port_state_to_neutral): read-only state and RWS. */
#define PORT_RO     ((1u << 0) | (1u << 3) | (0xFu << 10) | (1u << 30))
#define PORT_RWS    ((0xFu << 5) | (1u << 9) | (0x3u << 14) | (0x7u << 25))

/* Interrupter 0 (runtime registers + 0x20) */
#define IMAN        0x20
#define ERSTSZ      0x28
#define ERSTBA      0x30
#define ERDP        0x38
#define  ERDP_EHB    (1u << 3)

/* Extended capabilities */
#define EXT_LEGACY       1
#define EXT_PROTOCOL     2
#define LEGACY_BIOS_OWNED (1u << 16)
#define LEGACY_OS_OWNED   (1u << 24)
#define LEGACY_DISABLE_SMI ((0x7u << 1) + (0xFFu << 5) + (0x7u << 17))
#define LEGACY_SMI_EVENTS  (0x7u << 29)

/* TRB types */
enum {
    TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6,
    TRB_ENABLE_SLOT = 9, TRB_DISABLE_SLOT = 10, TRB_ADDRESS_DEVICE = 11, TRB_CONFIGURE_EP = 12,
    TRB_EVALUATE_CONTEXT = 13, TRB_RESET_EP = 14, TRB_STOP_EP = 15, TRB_SET_TR_DEQUEUE = 16,
    TRB_TRANSFER_EVENT = 32, TRB_COMMAND_COMPLETION = 33, TRB_PORT_STATUS_CHANGE = 34,
};
#define TRB_CYCLE    (1u << 0)
#define TRB_TOGGLE   (1u << 1)    /* link TRB: flip the cycle bit */
#define TRB_ISP      (1u << 2)
#define TRB_CHAIN    (1u << 4)
#define TRB_IOC      (1u << 5)
#define TRB_IDT      (1u << 6)
#define TRB_TYPE(t)  ((uint32_t)(t) << 10)
#define TRB_DIR_IN   (1u << 16)

/* Completion codes */
enum { CC_SUCCESS = 1, CC_STALL = 6, CC_SHORT_PACKET = 13 };

#define RING_TRBS    256            /* one 4 KiB page */
#define MAX_HC       4
#define MAX_PORTS    64

struct trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
};

struct ring {
    struct trb *trbs;
    uint64_t phys;
    int enqueue;
    uint32_t cycle;
};

struct xhci {
    struct pci_device pci;
    volatile uint8_t *cap, *op, *rt, *db;
    uint32_t mmio_size;                  /* bytes mapped at cap */
    int max_slots, max_ports, ctx_size;
    bool ac64;
    uint8_t port_major[MAX_PORTS + 1];   /* 2 or 3, from the supported protocol capabilities */
    uint32_t port_events;                /* (first 32 ports) a status change was posted */
    uint64_t *dcbaa;
    struct ring cmd;
    struct trb *events;
    uint64_t events_phys;
    int event_index;
    uint32_t event_cycle;
    struct usb_device *slots[256];

    /* The last command completion / control transfer result seen by the poller. */
    uint64_t done_cmd;                   /* TRB address it completed */
    uint32_t done_cmd_code, done_cmd_slot;
};

static struct xhci controllers[MAX_HC];
static int controller_count;

static inline uint32_t rd(volatile uint8_t *base, uint32_t off) { return *(volatile uint32_t *)(base + off); }
static inline void wr(volatile uint8_t *base, uint32_t off, uint32_t v) { *(volatile uint32_t *)(base + off) = v; }
static inline void wr64(volatile uint8_t *base, uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(base + off) = (uint32_t)v;
    *(volatile uint32_t *)(base + off + 4) = (uint32_t)(v >> 32);
}

static bool wait_bits(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t want, int ms) {
    for (int i = 0; i <= ms; i += 10) {
        if ((rd(base, off) & mask) == want) {
            return true;
        }
        sched_sleep_ms(10);
    }
    return (rd(base, off) & mask) == want;
}

/* DMA memory: zeroed pages the controller can reach. */
static uint64_t dma_alloc(struct xhci *hc, int pages, void **virt) {
    uint64_t phys = pmm_alloc_contiguous((uint64_t)pages, hc->ac64 ? ~0ull : 0x100000000ull);
    if (phys) {
        *virt = phys_to_virt(phys);
        memset(*virt, 0, (size_t)pages * 4096);
    }
    return phys;
}

static void dma_free(uint64_t phys, int pages) {
    for (int i = 0; i < pages && phys; i++) {
        pmm_free_page(phys + (uint64_t)i * 4096);
    }
}

/* --- rings (xhci-ring.c) ---------------------------------------------------------------------------------- */

static bool ring_init(struct xhci *hc, struct ring *r) {
    void *v;
    r->phys = dma_alloc(hc, 1, &v);
    if (!r->phys) {
        return false;
    }
    r->trbs = v;
    r->enqueue = 0;
    r->cycle = 1;
    /* The last TRB links back to the first, flipping the cycle bit. */
    r->trbs[RING_TRBS - 1].param = r->phys;
    r->trbs[RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE;
    return true;
}

/* Put a TRB on a ring (the cycle bit last, so the controller never sees half of it). */
static uint64_t ring_push(struct ring *r, uint64_t param, uint32_t status, uint32_t control) {
    struct trb *t = &r->trbs[r->enqueue];
    uint64_t addr = r->phys + (uint64_t)r->enqueue * sizeof(struct trb);
    t->param = param;
    t->status = status;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    t->control = (control & ~TRB_CYCLE) | r->cycle;
    if (++r->enqueue == RING_TRBS - 1) {
        /* Hand the link TRB over too, and go around. */
        struct trb *link = &r->trbs[RING_TRBS - 1];
        link->control = (link->control & ~TRB_CYCLE) | r->cycle;
        r->enqueue = 0;
        r->cycle ^= 1;
    }
    return addr;
}

static void doorbell(struct xhci *hc, int slot, uint32_t target) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wr(hc->db, 4 * (uint32_t)slot, target);
}

/* --- events ------------------------------------------------------------------------------------------------ */

static void handle_event(struct xhci *hc, const struct trb *e) {
    uint32_t type = (e->control >> 10) & 0x3F;
    uint32_t code = e->status >> 24;
    if (type == TRB_COMMAND_COMPLETION) {
        hc->done_cmd = e->param;
        hc->done_cmd_code = code;
        hc->done_cmd_slot = e->control >> 24;
    } else if (type == TRB_PORT_STATUS_CHANGE) {
        uint32_t port = (uint32_t)(e->param >> 24) & 0xFF;
        if (port >= 1 && port <= 32) {
            hc->port_events |= 1u << (port - 1);
        }
    } else if (type == TRB_TRANSFER_EVENT) {
        int slot = (int)(e->control >> 24), dci = (int)((e->control >> 16) & 0x1F);
        struct usb_device *dev = hc->slots[slot];
        if (!dev) {
            return;
        }
        if (dci == 1) {
            /* Endpoint 0: xhci_control is waiting for this. */
            dev->ctrl_event_trb = e->param;
            dev->ctrl_event_code = code;
            dev->ctrl_event_residual = e->status & 0xFFFFFF;
            dev->ctrl_event_seen = true;
        } else if (dev->bulk[dci]) {
            /* xhci_bulk is waiting for this. */
            dev->bulk_event[dci].trb = e->param;
            dev->bulk_event[dci].code = code;
            dev->bulk_event[dci].residual = e->status & 0xFFFFFF;
            dev->bulk_event[dci].seen = true;
        } else {
            usb_interrupt_done(dev, dci, (int)code, e->status & 0xFFFFFF);
        }
    }
}

static void poll_controller(struct xhci *hc) {
    bool any = false;
    for (;;) {
        struct trb *e = &hc->events[hc->event_index];
        if ((e->control & TRB_CYCLE) != hc->event_cycle) {
            break;
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        struct trb copy = *e;
        if (++hc->event_index == RING_TRBS) {
            hc->event_index = 0;
            hc->event_cycle ^= 1;
        }
        any = true;
        handle_event(hc, &copy);
    }
    if (any) {
        wr64(hc->rt, ERDP, (hc->events_phys + (uint64_t)hc->event_index * sizeof(struct trb)) | ERDP_EHB);
    }
}

void xhci_poll(void) {
    for (int i = 0; i < controller_count; i++) {
        poll_controller(&controllers[i]);
    }
}

/* Run a command and wait for its completion. Returns the completion code (0: timeout). */
static uint32_t command(struct xhci *hc, uint64_t param, uint32_t status, uint32_t control, uint32_t *slot) {
    uint64_t addr = ring_push(&hc->cmd, param, status, control);
    hc->done_cmd = 0;
    doorbell(hc, 0, 0);
    for (int i = 0; i < 300; i++) {
        poll_controller(hc);
        if (hc->done_cmd == addr) {
            if (slot) {
                *slot = hc->done_cmd_slot;
            }
            return hc->done_cmd_code;
        }
        if (i < 20) {
            sched_yield();
        } else {
            sched_sleep_ms(10);
        }
    }
    usb_warn("xHCI command %u timed out", (control >> 10) & 0x3F);
    return 0;
}

/* --- contexts ------------------------------------------------------------------------------------------- */

/* Context i of a device: 0 = slot, 1.. = endpoints by DCI. The input context has the control context first. */
static uint32_t *in_ctx(struct usb_device *dev, int i) {
    return (uint32_t *)((uint8_t *)dev->in_ctx + (size_t)(i + 1) * (size_t)dev->hc->ctx_size);
}

static uint32_t *control_ctx(struct usb_device *dev) {
    return (uint32_t *)dev->in_ctx;
}

static void clear_input(struct usb_device *dev) {
    memset(dev->in_ctx, 0, (size_t)dev->hc->ctx_size * 33);
}

static void fill_slot(struct usb_device *dev, int entries) {
    uint32_t *s = in_ctx(dev, 0);
    s[0] = (dev->route & 0xFFFFF) | (uint32_t)dev->speed << 20 | (dev->tt_multi ? 1u << 25 : 0) |
           (dev->is_hub ? 1u << 26 : 0) | (uint32_t)entries << 27;
    s[1] = (uint32_t)dev->root_port << 16 | (dev->is_hub ? (uint32_t)dev->hub_ports << 24 : 0);
    s[2] = (uint32_t)dev->tt_slot | (uint32_t)dev->tt_port << 8;
}

static void fill_endpoint(uint32_t *ep, uint32_t type, uint16_t mps, uint32_t interval, struct ring *r,
                          uint16_t avg_trb, uint16_t esit) {
    ep[0] = interval << 16;
    ep[1] = 3u << 1 | type << 3 | (uint32_t)mps << 16;   /* 3 retries on errors */
    ep[2] = (uint32_t)r->phys | r->cycle;
    ep[3] = (uint32_t)(r->phys >> 32);
    ep[4] = avg_trb | (uint32_t)esit << 16;
}

/* --- devices ---------------------------------------------------------------------------------------------- */

bool xhci_address_device(struct usb_device *dev) {
    struct xhci *hc = dev->hc;
    uint32_t slot = 0;
    uint32_t code = command(hc, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot);
    if (code != CC_SUCCESS || !slot || slot > (uint32_t)hc->max_slots) {
        usb_warn("no free device slot (code %u)", code);
        return false;
    }
    dev->slot = (int)slot;
    void *v = NULL;
    dev->out_ctx_phys = dma_alloc(hc, 1, &v);
    dev->out_ctx = v;
    dev->in_ctx_phys = dma_alloc(hc, 1, &v);
    dev->in_ctx = v;
    dev->buffer_phys = dma_alloc(hc, 1, &v);
    dev->buffer = v;
    struct ring *r = kzalloc(sizeof(struct ring));
    if (!dev->out_ctx_phys || !dev->in_ctx_phys || !dev->buffer_phys || !r || !ring_init(hc, r)) {
        return false;
    }
    dev->rings[1] = r;
    dev->max_dci = 1;
    hc->dcbaa[slot] = dev->out_ctx_phys;
    hc->slots[slot] = dev;

    /* Endpoint 0's packet size until the device tells us: Linux's guesses. */
    dev->mps0 = dev->speed == USB_SPEED_SUPER ? 512 : dev->speed == USB_SPEED_LOW ? 8 : 64;
    clear_input(dev);
    control_ctx(dev)[1] = 1u << 0 | 1u << 1;   /* add: slot, endpoint 0 */
    fill_slot(dev, 1);
    fill_endpoint(in_ctx(dev, 1), 4 /* control */, dev->mps0, 0, r, 8, 0);
    code = command(hc, dev->in_ctx_phys, 0, TRB_TYPE(TRB_ADDRESS_DEVICE) | slot << 24, NULL);
    if (code != CC_SUCCESS) {
        usb_warn("port %d: Address Device failed (code %u)", dev->root_port, code);
        return false;
    }
    sched_sleep_ms(10);   /* SET_ADDRESS recovery time */
    return true;
}

bool xhci_update_mps0(struct usb_device *dev, uint16_t mps) {
    if (mps == dev->mps0) {
        return true;
    }
    dev->mps0 = mps;
    clear_input(dev);
    control_ctx(dev)[1] = 1u << 1;   /* evaluate endpoint 0 */
    fill_endpoint(in_ctx(dev, 1), 4, mps, 0, dev->rings[1], 8, 0);
    /* (Evaluate Context only looks at the packet size; the ring pointer is ignored.) */
    return command(dev->hc, dev->in_ctx_phys, 0, TRB_TYPE(TRB_EVALUATE_CONTEXT) | (uint32_t)dev->slot << 24,
                   NULL) == CC_SUCCESS;
}

bool xhci_set_hub(struct usb_device *dev, int ports, bool multi_tt) {
    dev->is_hub = true;
    dev->hub_ports = ports;
    clear_input(dev);
    control_ctx(dev)[1] = 1u << 0;   /* the slot context only */
    if (multi_tt && dev->speed == USB_SPEED_HIGH) {
        dev->tt_multi = true;
    }
    fill_slot(dev, dev->max_dci);
    return command(dev->hc, dev->in_ctx_phys, 0, TRB_TYPE(TRB_CONFIGURE_EP) | (uint32_t)dev->slot << 24, NULL) ==
           CC_SUCCESS;
}

/* xhci_get_endpoint_interval: the polling period as 2^n 125 us microframes. */
static uint32_t interval_exponent(int speed, uint8_t b) {
    if (speed == USB_SPEED_HIGH || speed == USB_SPEED_SUPER) {
        uint32_t e = (b < 1 ? 1 : b > 16 ? 16 : b) - 1u;   /* bInterval is already an exponent */
        return e;
    }
    /* Low/full speed: bInterval in 1 ms frames; nearest lower power of two, in microframes, 3..10. */
    uint32_t uframes = (uint32_t)(b ? b : 1) * 8, e = 0;
    while ((2u << e) <= uframes) {
        e++;
    }
    return e < 3 ? 3 : e > 10 ? 10 : e;
}

int xhci_add_interrupt_in(struct usb_device *dev, uint8_t address, uint16_t mps, uint8_t interval) {
    int dci = (address & 0x0F) * 2 + 1;
    struct ring *r = kzalloc(sizeof(struct ring));
    if (!r || !ring_init(dev->hc, r)) {
        return -1;
    }
    dev->rings[dci] = r;
    if (dci > dev->max_dci) {
        dev->max_dci = dci;
    }
    clear_input(dev);
    control_ctx(dev)[1] = 1u << 0 | 1u << dci;
    fill_slot(dev, dev->max_dci);
    uint16_t size = mps & 0x7FF;
    uint32_t burst = (mps >> 11) & 3;   /* high-speed high-bandwidth: extra transactions */
    uint32_t *ep = in_ctx(dev, dci);
    fill_endpoint(ep, 7 /* interrupt IN */, size, interval_exponent(dev->speed, interval), r, size,
                  (uint16_t)(size * (burst + 1)));
    ep[1] |= burst << 8;
    uint32_t code = command(dev->hc, dev->in_ctx_phys, 0, TRB_TYPE(TRB_CONFIGURE_EP) | (uint32_t)dev->slot << 24, NULL);
    if (code != CC_SUCCESS) {
        usb_warn("%04x:%04x: Configure Endpoint failed (code %u)", dev->vendor, dev->product, code);
        return -1;
    }
    return dci;
}

bool xhci_queue_interrupt(struct usb_device *dev, int dci, uint64_t buffer_phys, uint32_t length) {
    struct ring *r = dev->rings[dci];
    if (!r) {
        return false;
    }
    ring_push(r, buffer_phys, length, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    doorbell(dev->hc, dev->slot, (uint32_t)dci);
    return true;
}

/* After a stall: reset the endpoint and point it past what's queued (Linux: xhci_handle_halted_endpoint). */
static void recover_endpoint(struct usb_device *dev, int dci) {
    struct ring *r = dev->rings[dci];
    command(dev->hc, 0, 0, TRB_TYPE(TRB_RESET_EP) | (uint32_t)dci << 16 | (uint32_t)dev->slot << 24, NULL);
    uint64_t dequeue = r->phys + (uint64_t)r->enqueue * sizeof(struct trb);
    command(dev->hc, dequeue | r->cycle, 0,
            TRB_TYPE(TRB_SET_TR_DEQUEUE) | (uint32_t)dci << 16 | (uint32_t)dev->slot << 24, NULL);
}

void xhci_reset_endpoint(struct usb_device *dev, int dci) {
    if (dev->rings[dci]) {
        recover_endpoint(dev, dci);
    }
}

uint64_t xhci_dma_alloc(struct usb_device *dev, int pages, void **virt) {
    return dma_alloc(dev->hc, pages, virt);
}

void xhci_dma_free(uint64_t phys, int pages) {
    dma_free(phys, pages);
}

/* --- bulk endpoints (xhci-mem.c xhci_endpoint_init, xhci-ring.c xhci_queue_bulk_tx) ---------------------- */

bool xhci_add_bulk(struct usb_device *dev, uint8_t in_addr, uint16_t in_mps, uint8_t in_burst, uint8_t out_addr,
                   uint16_t out_mps, uint8_t out_burst, int *in_dci, int *out_dci) {
    int in = (in_addr & 0x0F) * 2 + 1, out = (out_addr & 0x0F) * 2;
    struct ring *ri = kzalloc(sizeof(struct ring)), *ro = kzalloc(sizeof(struct ring));
    if (!ri || !ro || !ring_init(dev->hc, ri) || !ring_init(dev->hc, ro)) {
        if (ri && ri->phys) {
            dma_free(ri->phys, 1);
        }
        if (ro && ro->phys) {
            dma_free(ro->phys, 1);
        }
        kfree(ri);
        kfree(ro);
        return false;
    }
    dev->rings[in] = ri;
    dev->rings[out] = ro;
    dev->bulk[in] = dev->bulk[out] = true;
    int top = in > out ? in : out;
    if (top > dev->max_dci) {
        dev->max_dci = top;
    }
    clear_input(dev);
    control_ctx(dev)[1] = 1u << 0 | 1u << in | 1u << out;
    fill_slot(dev, dev->max_dci);
    uint32_t *ep = in_ctx(dev, in);
    fill_endpoint(ep, 6 /* bulk IN */, in_mps & 0x7FF, 0, ri, 3072, 0);
    ep[1] |= (uint32_t)in_burst << 8;
    ep = in_ctx(dev, out);
    fill_endpoint(ep, 2 /* bulk OUT */, out_mps & 0x7FF, 0, ro, 3072, 0);
    ep[1] |= (uint32_t)out_burst << 8;
    uint32_t code = command(dev->hc, dev->in_ctx_phys, 0, TRB_TYPE(TRB_CONFIGURE_EP) | (uint32_t)dev->slot << 24, NULL);
    if (code != CC_SUCCESS) {
        usb_warn("%04x:%04x: Configure Endpoint (bulk) failed (code %u)", dev->vendor, dev->product, code);
        return false;
    }
    *in_dci = in;
    *out_dci = out;
    return true;
}

/* Give up on what's queued: stop the endpoint and move its dequeue pointer past it. */
static void cancel_transfers(struct usb_device *dev, int dci) {
    struct ring *r = dev->rings[dci];
    command(dev->hc, 0, 0, TRB_TYPE(TRB_STOP_EP) | (uint32_t)dci << 16 | (uint32_t)dev->slot << 24, NULL);
    uint64_t dequeue = r->phys + (uint64_t)r->enqueue * sizeof(struct trb);
    command(dev->hc, dequeue | r->cycle, 0,
            TRB_TYPE(TRB_SET_TR_DEQUEUE) | (uint32_t)dci << 16 | (uint32_t)dev->slot << 24, NULL);
}

int xhci_bulk(struct usb_device *dev, int dci, uint64_t buffer_phys, uint32_t length, uint32_t *actual,
              int timeout_ms) {
    struct ring *r = dev->rings[dci];
    *actual = 0;
    if (!r || !dev->bulk[dci]) {
        return -1;
    }
    /* One TRB per piece that doesn't cross a 64 KiB boundary (TRB_MAX_BUFF_SIZE). */
    enum { MAX_PIECES = 16 };
    uint64_t trb_addr[MAX_PIECES];
    uint32_t trb_len[MAX_PIECES];
    int pieces = 0;
    uint32_t mps = 512;   /* only for TD Size, where an estimate is fine */
    uint32_t done = 0;
    do {
        uint64_t at = buffer_phys + done;
        uint32_t piece = (uint32_t)(0x10000 - (at & 0xFFFF));
        if (piece > length - done) {
            piece = length - done;
        }
        if (pieces == MAX_PIECES) {
            return -1;
        }
        bool last = done + piece == length;
        /* TD Size: packets still to come after this TRB (0 on the last one). */
        uint32_t remaining = length - done - piece;
        uint32_t td_size = last ? 0 : (remaining + mps - 1) / mps;
        td_size = td_size > 31 ? 31 : td_size;
        uint32_t control = TRB_TYPE(TRB_NORMAL) | TRB_ISP | (last ? TRB_IOC : TRB_CHAIN);
        trb_addr[pieces] = ring_push(r, at, piece | td_size << 17, control);
        trb_len[pieces++] = piece;
        done += piece;
    } while (done < length);

    dev->bulk_event[dci].seen = false;
    doorbell(dev->hc, dev->slot, (uint32_t)dci);

    /* Poll (yielding, so other threads still run) for the first 20 ms: a disk usually answers well
       within that, and sleeping costs a whole 10 ms timer tick. */
    uint64_t start = uptime_ms(), deadline = start + (uint64_t)timeout_ms;
    for (;;) {
        poll_controller(dev->hc);
        if (dev->bulk_event[dci].seen) {
            dev->bulk_event[dci].seen = false;
            int which = -1;
            for (int k = 0; k < pieces; k++) {
                if (trb_addr[k] == dev->bulk_event[dci].trb) {
                    which = k;
                }
            }
            if (which < 0) {
                continue;   /* a late event from an earlier transfer */
            }
            uint32_t code = dev->bulk_event[dci].code;
            uint32_t moved = 0;
            for (int k = 0; k < which; k++) {
                moved += trb_len[k];
            }
            uint32_t residual = dev->bulk_event[dci].residual;
            moved += trb_len[which] > residual ? trb_len[which] - residual : 0;
            if (code == CC_SUCCESS || code == CC_SHORT_PACKET) {
                /* (Short mid-TD: the controller skips to the TD's end. An event for the last TRB may
                   still come; the next transfer ignores it, as its address isn't in that TD.) */
                *actual = moved;
                return 0;
            }
            *actual = moved;
            if (code == CC_STALL) {
                recover_endpoint(dev, dci);
                return USB_STALL;
            }
            usb_warn("%04x:%04x: bulk transfer failed (code %u)", dev->vendor, dev->product, code);
            recover_endpoint(dev, dci);
            return -1;
        }
        uint64_t now = uptime_ms();
        if (now >= deadline) {
            break;
        }
        if (now - start < 20) {
            sched_yield();
        } else {
            sched_sleep_ms(1);
        }
    }
    usb_warn("%04x:%04x: bulk transfer timed out", dev->vendor, dev->product);
    cancel_transfers(dev, dci);
    return -1;
}

int xhci_control(struct usb_device *dev, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void *data,
                 uint16_t length) {
    if (length > 4096) {
        return -1;
    }
    bool in = type & 0x80;
    if (!in && length) {
        memcpy(dev->buffer, data, length);
    }
    struct ring *r = dev->rings[1];
    uint64_t setup = (uint64_t)type | (uint64_t)request << 8 | (uint64_t)value << 16 | (uint64_t)index << 32 |
                     (uint64_t)length << 48;
    uint32_t trt = length ? (in ? 3u : 2u) : 0u;
    ring_push(r, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | trt << 16);
    uint64_t data_trb = 0;
    if (length) {
        data_trb = ring_push(r, dev->buffer_phys, length, TRB_TYPE(TRB_DATA) | TRB_ISP | (in ? TRB_DIR_IN : 0));
    }
    /* The status stage goes the other way from the data (IN when there's none). */
    uint64_t status_trb = ring_push(r, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (in && length ? 0 : TRB_DIR_IN));
    dev->ctrl_event_seen = false;
    doorbell(dev->hc, dev->slot, 1);

    uint32_t residual = 0;
    for (int i = 0; i < 500; i++) {   /* up to ~5 s */
        poll_controller(dev->hc);
        if (dev->ctrl_event_seen) {
            dev->ctrl_event_seen = false;
            uint32_t code = dev->ctrl_event_code;
            if (dev->ctrl_event_trb == data_trb && code == CC_SHORT_PACKET) {
                residual = dev->ctrl_event_residual;   /* fine: the status stage follows */
                continue;
            }
            if (code != CC_SUCCESS && code != CC_SHORT_PACKET) {
                if (code == CC_STALL) {
                    recover_endpoint(dev, 1);
                }
                return -1;
            }
            if (dev->ctrl_event_trb == status_trb) {
                int got = length - (int)residual;
                if (in && got > 0) {
                    memcpy(data, dev->buffer, (size_t)got);
                }
                return in ? got : 0;
            }
            continue;
        }
        if (i < 20) {
            sched_yield();
        } else {
            sched_sleep_ms(10);
        }
    }
    usb_warn("%04x:%04x: control request %02x timed out", dev->vendor, dev->product, request);
    return -1;
}

void xhci_free_device(struct usb_device *dev) {
    struct xhci *hc = dev->hc;
    if (dev->slot) {
        command(hc, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)dev->slot << 24, NULL);
        hc->dcbaa[dev->slot] = 0;
        hc->slots[dev->slot] = NULL;
    }
    for (int i = 0; i < 32; i++) {
        if (dev->rings[i]) {
            dma_free(dev->rings[i]->phys, 1);
            kfree(dev->rings[i]);
            dev->rings[i] = NULL;
        }
    }
    dma_free(dev->in_ctx_phys, 1);
    dma_free(dev->out_ctx_phys, 1);
    dma_free(dev->buffer_phys, 1);
}

/* --- root ports (xhci-hub.c) --------------------------------------------------------------------------------- */

int xhci_port_count(struct xhci *hc) {
    return hc->max_ports;
}

static uint32_t neutral(uint32_t portsc) {
    return (portsc & PORT_RO) | (portsc & PORT_RWS);
}

bool xhci_port_connected(struct xhci *hc, int port, bool *replugged) {
    uint32_t p = rd(hc->op, PORTSC(port));
    if (p & PORT_CHANGES) {
        wr(hc->op, PORTSC(port), neutral(p) | (p & PORT_CHANGES));   /* acknowledge the changes */
    }
    *replugged = p & PORT_CSC;   /* (resets and link changes don't count) */
    return p & PORT_CCS;
}

bool xhci_port_reset(struct xhci *hc, int port, int *speed) {
    uint32_t p = rd(hc->op, PORTSC(port));
    if (!(p & PORT_CCS)) {
        return false;
    }
    if (hc->port_major[port] == 3) {
        /* USB 3 ports train by themselves; a warm reset if they got stuck. */
        if (!wait_bits(hc->op, PORTSC(port), PORT_PED, PORT_PED, 200)) {
            p = rd(hc->op, PORTSC(port));
            wr(hc->op, PORTSC(port), neutral(p) | PORT_WPR);
            wait_bits(hc->op, PORTSC(port), PORT_PED, PORT_PED, 500);
        }
    } else {
        wr(hc->op, PORTSC(port), neutral(p) | PORT_PR);
        wait_bits(hc->op, PORTSC(port), PORT_PRC, PORT_PRC, 500);
        sched_sleep_ms(10);   /* reset recovery */
    }
    p = rd(hc->op, PORTSC(port));
    wr(hc->op, PORTSC(port), neutral(p) | (p & PORT_CHANGES));
    *speed = (int)PORT_SPEED(p);
    return (p & PORT_CCS) && (p & PORT_PED);
}

/* --- bringing a controller up -------------------------------------------------------------------------------- */

static uint32_t ext_cap(struct xhci *hc, uint32_t from, int id) {
    uint32_t off = from ? from : (rd(hc->cap, HCCPARAMS1) >> 16) << 2;
    if (from) {
        uint32_t next = (rd(hc->cap, from) >> 8) & 0xFF;
        off = next ? from + (next << 2) : 0;
    }
    /* The list lives inside the BAR; a pointer past it is firmware garbage, not a reason to fault. */
    while (off && off + 16 <= hc->mmio_size) {
        uint32_t v = rd(hc->cap, off);
        if ((int)(v & 0xFF) == id) {
            return off;
        }
        uint32_t next = (v >> 8) & 0xFF;
        off = next ? off + (next << 2) : 0;
    }
    return 0;
}

/* quirk_usb_handoff_xhci: ask the BIOS to let go, and stop its SMIs. */
static void bios_handoff(struct xhci *hc) {
    uint32_t off = ext_cap(hc, 0, EXT_LEGACY);
    if (!off) {
        return;
    }
    uint32_t v = rd(hc->cap, off);
    if (v & LEGACY_BIOS_OWNED) {
        wr(hc->cap, off, v | LEGACY_OS_OWNED);
        if (!wait_bits(hc->cap, off, LEGACY_BIOS_OWNED, 0, 1000)) {
            usb_warn("BIOS didn't release the controller; taking it anyway");
            wr(hc->cap, off, rd(hc->cap, off) & ~LEGACY_BIOS_OWNED);
        }
    }
    v = rd(hc->cap, off + 4);
    v &= LEGACY_DISABLE_SMI;
    v |= LEGACY_SMI_EVENTS;
    wr(hc->cap, off + 4, v);
}

/* usb_enable_intel_xhci_ports: on Intel chipsets with EHCI too, route the switchable ports to xHCI. */
static void intel_port_switch(struct xhci *hc) {
    if (hc->pci.vendor != 0x8086) {
        return;
    }
    struct pci_device ehci;
    bool found = false;
    for (int i = 0; pci_find_class(0x0C, 0x03, i, &ehci); i++) {
        if (ehci.prog_if == 0x20 && ehci.vendor == 0x8086) {
            found = true;
        }
    }
    if (!found) {
        return;
    }
    pci_write32(&hc->pci, 0xD8, pci_read32(&hc->pci, 0xDC));   /* USB3_PSSEN = USB3PRM */
    pci_write32(&hc->pci, 0xD0, pci_read32(&hc->pci, 0xD4));   /* XUSB2PR = USB2PRM */
}

static bool start(struct xhci *hc) {
    uint64_t bar = pci_bar_address(&hc->pci, 0);
    if (!bar) {
        return false;
    }
    pci_write16(&hc->pci, PCI_COMMAND, (uint16_t)(pci_read16(&hc->pci, PCI_COMMAND) | PCI_COMMAND_MEMORY |
                                                  PCI_COMMAND_MASTER | PCI_COMMAND_INTX_OFF));
    volatile uint8_t *probe = vmm_map_mmio(bar, 0x1000);
    if (!probe) {
        return false;
    }
    /* Map the whole BAR, as Linux does: the extended capabilities (legacy handoff, protocols) can sit past
     * the doorbells, e.g. at 0x8000 on real Intel controllers. Fall back to the reach of the registers. */
    uint32_t sp1 = rd(probe, HCSPARAMS1), sp2 = rd(probe, HCSPARAMS2), cp1 = rd(probe, HCCPARAMS1);
    uint32_t caplen = rd(probe, CAPLENGTH) & 0xFF, rtsoff = rd(probe, RTSOFF) & ~0x1Fu, dboff = rd(probe, DBOFF) & ~0x3u;
    uint32_t size = caplen + 0x400 + 0x10 * (sp1 >> 24);
    size = rtsoff + 0x40 > size ? rtsoff + 0x40 : size;
    size = dboff + 4 * 256 > size ? dboff + 4 * 256 : size;
    uint64_t bar_size = pci_bar_size(&hc->pci, 0);
    if (bar_size > size && bar_size <= 16u << 20) {
        size = (uint32_t)bar_size;
    }
    hc->mmio_size = size;
    hc->cap = vmm_map_mmio(bar, size);
    if (!hc->cap) {
        return false;
    }
    hc->op = hc->cap + caplen;
    hc->rt = hc->cap + rtsoff;
    hc->db = hc->cap + dboff;
    hc->max_slots = (int)(sp1 & 0xFF);
    hc->max_ports = (int)(sp1 >> 24);
    hc->max_ports = hc->max_ports > MAX_PORTS ? MAX_PORTS : hc->max_ports;
    hc->ctx_size = cp1 & (1u << 2) ? 64 : 32;
    hc->ac64 = cp1 & 1;

    bios_handoff(hc);
    intel_port_switch(hc);

    /* Which ports are USB 2 and which USB 3. */
    for (uint32_t off = ext_cap(hc, 0, EXT_PROTOCOL); off; off = ext_cap(hc, off, EXT_PROTOCOL)) {
        uint32_t major = rd(hc->cap, off) >> 24, ports = rd(hc->cap, off + 8);
        int first = (int)(ports & 0xFF), count = (int)((ports >> 8) & 0xFF);
        for (int p = first; p < first + count && p <= MAX_PORTS; p++) {
            hc->port_major[p] = (uint8_t)major;
        }
    }

    /* Stop, reset (xhci_reset). */
    wait_bits(hc->op, USBSTS, STS_CNR, 0, 1000);
    wr(hc->op, USBCMD, rd(hc->op, USBCMD) & ~(CMD_RUN | CMD_INTE));
    if (!wait_bits(hc->op, USBSTS, STS_HALT, STS_HALT, 100)) {
        usb_warn("controller didn't halt");
    }
    wr(hc->op, USBCMD, CMD_RESET);
    if (!wait_bits(hc->op, USBCMD, CMD_RESET, 0, 1000) || !wait_bits(hc->op, USBSTS, STS_CNR, 0, 1000)) {
        usb_warn("controller didn't come out of reset");
        return false;
    }

    /* Memory (xhci_mem_init): device contexts, scratchpad, command ring, event ring. */
    void *v;
    uint64_t dcbaa_phys = dma_alloc(hc, 1, &v);
    if (!dcbaa_phys) {
        return false;
    }
    hc->dcbaa = v;
    int scratch = (int)(((sp2 >> 21) & 0x1F) << 5 | (sp2 >> 27));
    if (scratch) {
        uint64_t array_phys = dma_alloc(hc, 1, &v);
        uint64_t *array = v;
        if (!array_phys || scratch > 512) {
            usb_warn("can't provide %d scratchpad pages", scratch);
            return false;
        }
        for (int i = 0; i < scratch; i++) {
            void *page;
            array[i] = dma_alloc(hc, 1, &page);
            if (!array[i]) {
                return false;
            }
        }
        hc->dcbaa[0] = array_phys;
    }
    wr(hc->op, CONFIG, (uint32_t)(hc->max_slots > 255 ? 255 : hc->max_slots));
    wr64(hc->op, DCBAAP, dcbaa_phys);
    if (!ring_init(hc, &hc->cmd)) {
        return false;
    }
    wr64(hc->op, CRCR, hc->cmd.phys | 1);

    hc->events_phys = dma_alloc(hc, 1, &v);
    hc->events = v;
    uint64_t erst_phys = dma_alloc(hc, 1, &v);
    uint64_t *erst = v;
    if (!hc->events_phys || !erst_phys) {
        return false;
    }
    erst[0] = hc->events_phys;
    erst[1] = RING_TRBS;
    hc->event_index = 0;
    hc->event_cycle = 1;
    wr(hc->rt, ERSTSZ, 1);
    wr64(hc->rt, ERDP, hc->events_phys);
    wr64(hc->rt, ERSTBA, erst_phys);
    wr(hc->rt, IMAN, rd(hc->rt, IMAN) & ~2u);   /* no interrupts: we poll */

    /* Run (xhci_run). */
    wr(hc->op, USBCMD, CMD_RUN);
    if (!wait_bits(hc->op, USBSTS, STS_HALT, 0, 100)) {
        usb_warn("controller didn't start");
        return false;
    }
    /* Power the ports (if the controller switches power at all), then give devices time to connect. */
    if (cp1 & (1u << 3)) {
        for (int p = 1; p <= hc->max_ports; p++) {
            uint32_t s = rd(hc->op, PORTSC(p));
            if (!(s & PORT_PP)) {
                wr(hc->op, PORTSC(p), neutral(s) | PORT_PP);
            }
        }
    }
    return true;
}

int xhci_init_all(void) {
    pci_init();
    struct pci_device d;
    for (int i = 0; pci_find_class(0x0C, 0x03, i, &d) && controller_count < MAX_HC; i++) {
        if (d.prog_if != 0x30) {
            continue;   /* EHCI/OHCI/UHCI: not ours */
        }
        struct xhci *hc = &controllers[controller_count];
        memset(hc, 0, sizeof(*hc));
        hc->pci = d;
        if (start(hc)) {
            usb_log("xHCI %04x:%04x at %02x:%02x.%x: %d ports, %d slots%s", d.vendor, d.device, d.bus, d.slot,
                    d.function, hc->max_ports, hc->max_slots, hc->ac64 ? "" : ", 32-bit DMA");
            controller_count++;
        } else {
            usb_warn("xHCI %04x:%04x at %02x:%02x.%x: failed to start", d.vendor, d.device, d.bus, d.slot,
                    d.function);
        }
    }
    return controller_count;
}

int xhci_count(void) {
    return controller_count;
}

struct xhci *xhci_at(int i) {
    return &controllers[i];
}
