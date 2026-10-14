/*
 * virtio-net: the paravirtual network card of QEMU/KVM and other virtual machines, after
 * linux/drivers/net/virtio_net.c (and the virtio PCI transport and split rings it sits on, virtio
 * spec 1.1 sections 2.6, 4.1 and 5.1), cut down to one receive and one transmit queue:
 *
 *   - "modern" virtio over PCI only (vendor-specific capabilities pointing into the BARs), which
 *     QEMU's virtio-net-pci offers next to the legacy interface;
 *   - features: VERSION_1, MAC, STATUS (link up/down), SPEED_DUPLEX if offered; no offloads and no
 *     mergeable buffers, so every frame is one buffer behind a 12-byte struct virtio_net_hdr_v1;
 *   - receive: add_recvbuf_small's scheme, one fixed buffer per ring slot, put back after use;
 *   - transmit: xmit_skb with a copy into the slot's own buffer, reclaimed like free_old_xmit;
 *   - interrupts by MSI-X (every queue on one vector), otherwise polled.
 */

#include "drivers/net/nic.h"
#include "arch/x86_64/cpu.h"
#include "drivers/pci.h"
#include "lib/kprintf.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "sched/sched.h"

#define VIRTIO_VENDOR          0x1AF4
#define VIRTIO_DEV_NET_LEGACY  0x1000   /* transitional */
#define VIRTIO_DEV_NET_MODERN  0x1041

/* struct virtio_pci_cap cfg_type */
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

/* struct virtio_pci_common_cfg */
#define COMMON_DFSELECT      0x00
#define COMMON_DF            0x04
#define COMMON_GFSELECT      0x08
#define COMMON_GF            0x0C
#define COMMON_MSIX          0x10
#define COMMON_NUMQ          0x12
#define COMMON_STATUS        0x14
#define COMMON_CFGGENERATION 0x15
#define COMMON_Q_SELECT      0x16
#define COMMON_Q_SIZE        0x18
#define COMMON_Q_MSIX        0x1A
#define COMMON_Q_ENABLE      0x1C
#define COMMON_Q_NOFF        0x1E
#define COMMON_Q_DESC        0x20
#define COMMON_Q_AVAIL       0x28
#define COMMON_Q_USED        0x30

#define STATUS_ACKNOWLEDGE 1
#define STATUS_DRIVER      2
#define STATUS_DRIVER_OK   4
#define STATUS_FEATURES_OK 8
#define STATUS_FAILED      128
#define NO_VECTOR          0xFFFF

/* Feature bits */
#define VIRTIO_NET_F_MAC           5
#define VIRTIO_NET_F_STATUS        16
#define VIRTIO_F_VERSION_1         32
#define VIRTIO_NET_F_SPEED_DUPLEX  63
#define VIRTIO_NET_S_LINK_UP       1

/* struct virtio_net_config */
#define NETCFG_MAC     0
#define NETCFG_STATUS  6
#define NETCFG_SPEED   12
#define NETCFG_DUPLEX  16

/* Split virtqueue */
#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2
#define QUEUE_MAX          256

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtio_net_hdr_v1 {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
};

#define BUF_SIZE 2048   /* header + the largest frame, rounded */

struct vq {
    uint16_t index, size;
    volatile struct vring_desc *desc;
    volatile uint16_t *avail;          /* flags, idx, ring[size], used_event */
    volatile uint16_t *used_hdr;       /* flags, idx */
    volatile struct vring_used_elem *used;
    uint16_t last_used;
    volatile uint16_t *notify;
    uint8_t *bufs;                     /* size * BUF_SIZE */
    uint64_t bufs_phys;
    uint16_t free_count;               /* transmit: slots not in flight */
    uint16_t free_list[QUEUE_MAX];
};

struct vnet {
    struct net_device dev;
    struct pci_device pci;
    volatile uint8_t *common, *device;
    volatile uint8_t *notify_base;
    uint32_t notify_mult;
    bool has_status;
    struct vq rx, tx;
};

#define MAX_CARDS 4
static struct vnet cards[MAX_CARDS];
static int card_count;

static inline uint8_t c8(struct vnet *v, uint32_t off) { return *(volatile uint8_t *)(v->common + off); }
static inline uint16_t c16(struct vnet *v, uint32_t off) { return *(volatile uint16_t *)(v->common + off); }
static inline uint32_t c32(struct vnet *v, uint32_t off) { return *(volatile uint32_t *)(v->common + off); }
static inline void w8(struct vnet *v, uint32_t off, uint8_t x) { *(volatile uint8_t *)(v->common + off) = x; }
static inline void w16(struct vnet *v, uint32_t off, uint16_t x) { *(volatile uint16_t *)(v->common + off) = x; }
static inline void w32(struct vnet *v, uint32_t off, uint32_t x) { *(volatile uint32_t *)(v->common + off) = x; }
static inline void w64(struct vnet *v, uint32_t off, uint64_t x) {
    w32(v, off, (uint32_t)x);
    w32(v, off + 4, (uint32_t)(x >> 32));
}

static inline void barrier(void) {
    __asm__ volatile ("mfence" ::: "memory");
}

/* --- the transport ------------------------------------------------------------------------------- */

static volatile uint8_t *map_cap(struct pci_device *d, uint8_t cap, uint32_t *length) {
    uint8_t bar = pci_read8(d, (uint8_t)(cap + 4));
    uint32_t offset = pci_read32(d, (uint8_t)(cap + 8));
    uint32_t len = pci_read32(d, (uint8_t)(cap + 12));
    uint64_t base = bar < 6 ? pci_bar_address(d, bar) : 0;
    if (!base || !len) {
        return NULL;
    }
    if (length) {
        *length = len;
    }
    return vmm_map_mmio(base + offset, len);
}

static bool find_caps(struct vnet *v) {
    for (uint8_t cap = pci_find_capability(&v->pci, PCI_CAP_VENDOR, 0); cap;
         cap = pci_find_capability(&v->pci, PCI_CAP_VENDOR, cap)) {
        switch (pci_read8(&v->pci, (uint8_t)(cap + 3))) {
        case VIRTIO_PCI_CAP_COMMON_CFG:
            if (!v->common) {
                v->common = map_cap(&v->pci, cap, NULL);
            }
            break;
        case VIRTIO_PCI_CAP_NOTIFY_CFG:
            if (!v->notify_base) {
                v->notify_base = map_cap(&v->pci, cap, NULL);
                v->notify_mult = pci_read32(&v->pci, (uint8_t)(cap + 16));
            }
            break;
        case VIRTIO_PCI_CAP_DEVICE_CFG:
            if (!v->device) {
                v->device = map_cap(&v->pci, cap, NULL);
            }
            break;
        }
    }
    return v->common && v->notify_base && v->device;
}

static bool setup_queue(struct vnet *v, struct vq *q, uint16_t index, bool msix) {
    w16(v, COMMON_Q_SELECT, index);
    uint16_t size = c16(v, COMMON_Q_SIZE);
    if (size == 0) {
        return false;
    }
    if (size > QUEUE_MAX) {
        size = QUEUE_MAX;
        w16(v, COMMON_Q_SIZE, size);
    }
    q->index = index;
    q->size = size;
    /* Descriptors, available ring and used ring in their own pages (alignment 16/2/4 is plenty). */
    void *desc = NULL, *avail = NULL, *used = NULL, *bufs = NULL;
    uint64_t desc_p = nic_dma_alloc((uint64_t)size * 16, false, &desc);
    uint64_t avail_p = nic_dma_alloc(6 + (uint64_t)size * 2, false, &avail);
    uint64_t used_p = nic_dma_alloc(6 + (uint64_t)size * 8, false, &used);
    q->bufs_phys = nic_dma_alloc((uint64_t)size * BUF_SIZE, false, &bufs);
    if (!desc_p || !avail_p || !used_p || !q->bufs_phys) {
        return false;
    }
    q->desc = desc;
    q->avail = avail;
    q->used_hdr = used;
    q->used = (volatile struct vring_used_elem *)((uint8_t *)used + 4);
    q->bufs = bufs;
    w64(v, COMMON_Q_DESC, desc_p);
    w64(v, COMMON_Q_AVAIL, avail_p);
    w64(v, COMMON_Q_USED, used_p);
    if (msix) {
        w16(v, COMMON_Q_MSIX, 0);
    }
    q->notify = (volatile uint16_t *)(v->notify_base + (uint32_t)c16(v, COMMON_Q_NOFF) * v->notify_mult);
    for (uint16_t i = 0; i < size; i++) {
        q->desc[i].addr = q->bufs_phys + (uint64_t)i * BUF_SIZE;
    }
    return true;
}

static void queue_enable(struct vnet *v, struct vq *q) {
    w16(v, COMMON_Q_SELECT, q->index);
    w16(v, COMMON_Q_ENABLE, 1);
}

static void kick(struct vq *q) {
    barrier();
    *q->notify = q->index;
}

/* Put descriptor `id` on the available ring. */
static void make_available(struct vq *q, uint16_t id) {
    uint16_t idx = q->avail[1];
    q->avail[2 + idx % q->size] = id;
    barrier();
    q->avail[1] = (uint16_t)(idx + 1);
}

/* --- receive ------------------------------------------------------------------------------------- */

static void fill_rx(struct vq *q) {
    for (uint16_t i = 0; i < q->size; i++) {
        q->desc[i].len = BUF_SIZE;
        q->desc[i].flags = VRING_DESC_F_WRITE;
        make_available(q, i);
    }
}

/* --- link ---------------------------------------------------------------------------------------- */

/* virtnet_config_changed_work + virtnet_update_settings */
static void update_link(struct vnet *v, bool report) {
    bool up = true;
    if (v->has_status) {
        up = *(volatile uint16_t *)(v->device + NETCFG_STATUS) & VIRTIO_NET_S_LINK_UP;
    }
    if (up == v->dev.link_up && report) {
        return;
    }
    v->dev.link_up = up;
    uint32_t speed = *(volatile uint32_t *)(v->device + NETCFG_SPEED);
    uint8_t duplex = *(volatile uint8_t *)(v->device + NETCFG_DUPLEX);
    v->dev.speed_mbps = speed == 0xFFFFFFFF ? 0 : speed;
    v->dev.full_duplex = duplex != 0;   /* (0xFF = unknown: assume full) */
    if (report) {
        net_link_changed(&v->dev);
    }
}

/* --- the stack's calls --------------------------------------------------------------------------- */

/* free_old_xmit: slots the device has finished sending go back on the free list. */
static void reclaim_tx(struct vq *q) {
    while (q->last_used != q->used_hdr[1]) {
        uint32_t id = q->used[q->last_used % q->size].id;
        q->last_used++;
        if (id < q->size) {
            q->free_list[q->free_count++] = (uint16_t)id;
        }
    }
}

static int vnet_transmit(struct net_device *dev, const void *frame, size_t len) {
    struct vnet *v = dev->priv;
    struct vq *q = &v->tx;
    uint64_t irq = irq_save();
    reclaim_tx(q);
    if (q->free_count == 0) {
        irq_restore(irq);
        return -1;   /* ring full: dropped (TCP sends it again) */
    }
    uint16_t id = q->free_list[--q->free_count];
    uint8_t *buf = q->bufs + (uint64_t)id * BUF_SIZE;
    memset(buf, 0, sizeof(struct virtio_net_hdr_v1));
    memcpy(buf + sizeof(struct virtio_net_hdr_v1), frame, len);
    q->desc[id].len = (uint32_t)(sizeof(struct virtio_net_hdr_v1) + len);
    q->desc[id].flags = 0;
    make_available(q, id);
    kick(q);
    dev->tx_packets++;
    dev->tx_bytes += len;
    irq_restore(irq);
    return 0;
}

/* virtnet_poll -> receive_buf (small buffers): every used receive slot goes up, then back in. */
static void vnet_poll(struct net_device *dev) {
    struct vnet *v = dev->priv;
    struct vq *q = &v->rx;
    bool returned = false;
    while (q->last_used != q->used_hdr[1]) {
        barrier();
        volatile struct vring_used_elem *e = &q->used[q->last_used % q->size];
        uint32_t id = e->id, len = e->len;
        q->last_used++;
        if (id >= q->size) {
            continue;
        }
        if (len > sizeof(struct virtio_net_hdr_v1)) {
            net_receive(dev, q->bufs + (uint64_t)id * BUF_SIZE + sizeof(struct virtio_net_hdr_v1),
                        len - sizeof(struct virtio_net_hdr_v1));
        } else {
            dev->rx_errors++;
        }
        q->desc[id].len = BUF_SIZE;
        q->desc[id].flags = VRING_DESC_F_WRITE;
        make_available(q, (uint16_t)id);
        returned = true;
    }
    if (returned) {
        kick(q);
    }
    uint64_t irq = irq_save();
    reclaim_tx(&v->tx);
    irq_restore(irq);
    update_link(v, true);
}

static void vnet_interrupt(void *arg) {
    (void)arg;
    net_wake();   /* (MSI-X: nothing to acknowledge) */
}

/* --- probing (virtnet_probe) -------------------------------------------------------------------- */

static void probe_one(struct pci_device *d) {
    if (card_count == MAX_CARDS) {
        return;
    }
    struct vnet *v = &cards[card_count];
    memset(v, 0, sizeof(*v));
    v->pci = *d;
    pci_write16(d, PCI_COMMAND, (uint16_t)(pci_read16(d, PCI_COMMAND) | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER));
    if (!find_caps(v)) {
        kprintf("[WARN] virtio-net: %02x:%02x.%u has no modern virtio interface\n", d->bus, d->slot, d->function);
        return;
    }

    /* Reset, then ACKNOWLEDGE | DRIVER (virtio_dev_probe). */
    w8(v, COMMON_STATUS, 0);
    for (int i = 0; i < 1000 && c8(v, COMMON_STATUS) != 0; i++) {
        sched_sleep_ms(1);
    }
    w8(v, COMMON_STATUS, STATUS_ACKNOWLEDGE);
    w8(v, COMMON_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    /* Features: ours ∩ the device's. */
    w32(v, COMMON_DFSELECT, 0);
    uint64_t offered = c32(v, COMMON_DF);
    w32(v, COMMON_DFSELECT, 1);
    offered |= (uint64_t)c32(v, COMMON_DF) << 32;
    uint64_t wanted = 1ull << VIRTIO_F_VERSION_1 | 1ull << VIRTIO_NET_F_MAC | 1ull << VIRTIO_NET_F_STATUS |
                      1ull << VIRTIO_NET_F_SPEED_DUPLEX;
    uint64_t features = offered & wanted;
    if (!(features & (1ull << VIRTIO_F_VERSION_1))) {
        kprintf("[WARN] virtio-net: the device doesn't offer virtio 1.0\n");
        w8(v, COMMON_STATUS, STATUS_FAILED);
        return;
    }
    w32(v, COMMON_GFSELECT, 0);
    w32(v, COMMON_GF, (uint32_t)features);
    w32(v, COMMON_GFSELECT, 1);
    w32(v, COMMON_GF, (uint32_t)(features >> 32));
    w8(v, COMMON_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
    if (!(c8(v, COMMON_STATUS) & STATUS_FEATURES_OK)) {
        kprintf("[WARN] virtio-net: features refused\n");
        w8(v, COMMON_STATUS, STATUS_FAILED);
        return;
    }
    v->has_status = features & (1ull << VIRTIO_NET_F_STATUS);

    bool msix = pci_enable_msi(d, vnet_interrupt, v, true);
    if (msix) {
        w16(v, COMMON_MSIX, 0);
        msix = c16(v, COMMON_MSIX) != NO_VECTOR;
    }
    if (!setup_queue(v, &v->rx, 0, msix) || !setup_queue(v, &v->tx, 1, msix)) {
        kprintf("[WARN] virtio-net: can't set up the queues\n");
        w8(v, COMMON_STATUS, STATUS_FAILED);
        return;
    }
    if (msix) {
        w16(v, COMMON_Q_SELECT, 0);
        msix = c16(v, COMMON_Q_MSIX) != NO_VECTOR;
    }
    for (uint16_t i = 0; i < v->tx.size; i++) {
        v->tx.free_list[v->tx.free_count++] = i;
    }
    fill_rx(&v->rx);
    queue_enable(v, &v->rx);
    queue_enable(v, &v->tx);
    w8(v, COMMON_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    kick(&v->rx);

    struct net_device *dev = &v->dev;
    dev->driver = "virtio-net";
    strlcpy(dev->model, "Virtio network device", sizeof(dev->model));
    if (features & (1ull << VIRTIO_NET_F_MAC)) {
        for (int i = 0; i < 6; i++) {
            dev->mac[i] = *(volatile uint8_t *)(v->device + NETCFG_MAC + i);
        }
    } else {
        /* eth_hw_addr_random: a locally administered address. */
        uint32_t lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        uint8_t mac[6] = { 0x02, 0x00, (uint8_t)(hi >> 8), (uint8_t)hi, (uint8_t)(lo >> 8), (uint8_t)lo };
        memcpy(dev->mac, mac, 6);
    }
    dev->has_interrupt = msix;
    dev->priv = v;
    dev->transmit = vnet_transmit;
    dev->poll = vnet_poll;
    update_link(v, false);
    card_count++;
    net_register(dev);
}

void virtio_net_probe(void) {
    struct pci_device d;
    for (int i = 0; pci_find_id(VIRTIO_VENDOR, VIRTIO_DEV_NET_MODERN, i, &d); i++) {
        probe_one(&d);
    }
    for (int i = 0; pci_find_id(VIRTIO_VENDOR, VIRTIO_DEV_NET_LEGACY, i, &d); i++) {
        probe_one(&d);
    }
}
