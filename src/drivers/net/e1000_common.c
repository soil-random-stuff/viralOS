/*
 * The part of Intel's e1000 and e1000e drivers that is the same for every chip (see e1000_hw.h):
 * register and PHY access, receive addresses, and the data path: legacy descriptor rings of
 * E1000_RING_SIZE entries with a 2 KiB buffer each (e1000_configure_tx/rx, e1000_clean_rx_irq,
 * e1000_xmit_frame, e1000_clean_tx_irq), frames copied in and out.
 */

#include "drivers/net/e1000_hw.h"
#include "arch/x86_64/cpu.h"
#include "drivers/timer.h"
#include "lib/kprintf.h"
#include "mm/vmm.h"
#include "sched/sched.h"

bool e1000_map(struct e1000_card *c, const struct pci_device *d) {
    c->pci = *d;
    uint64_t bar = pci_bar_address(d, 0);
    uint64_t size = pci_bar_size(d, 0);
    if (!bar) {
        return false;
    }
    c->regs = vmm_map_mmio(bar, size ? size : 0x20000);
    if (!c->regs) {
        return false;
    }
    for (int i = 1; i < 6 && !c->io_base; i++) {
        c->io_base = pci_bar_io(d, i);
    }
    pci_write16(d, PCI_COMMAND, (uint16_t)(pci_read16(d, PCI_COMMAND) | PCI_COMMAND_MEMORY |
                                            PCI_COMMAND_MASTER | (c->io_base ? PCI_COMMAND_IO : 0)));
    return true;
}

static bool valid_mac(const uint8_t *m) {
    bool zero = !(m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);
    return !zero && !(m[0] & 1);   /* not all zeroes, not multicast/broadcast */
}

bool e1000_read_mac_rar(struct e1000_card *c) {
    uint32_t lo = er32(c, E1000_RAL0), hi = er32(c, E1000_RAH0);
    uint8_t m[6] = { (uint8_t)lo, (uint8_t)(lo >> 8), (uint8_t)(lo >> 16), (uint8_t)(lo >> 24),
                     (uint8_t)hi, (uint8_t)(hi >> 8) };
    if (!valid_mac(m)) {
        return false;
    }
    memcpy(c->dev.mac, m, 6);
    return true;
}

bool e1000_read_eerd(struct e1000_card *c, uint16_t index, uint16_t *out) {
    ew32(c, E1000_EERD, (uint32_t)index << E1000_EERD_ADDR_SHIFT | E1000_EERD_START);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = er32(c, E1000_EERD);
        if (v & E1000_EERD_DONE) {
            *out = (uint16_t)(v >> E1000_EERD_DATA_SHIFT);
            return true;
        }
        udelay(5);
    }
    return false;
}

/* --- PHY --------------------------------------------------------------------------------------- */

bool e1000_phy_read(struct e1000_card *c, uint32_t reg, uint16_t *out) {
    ew32(c, E1000_MDIC, reg << E1000_MDIC_REG_SHIFT | (uint32_t)c->phy_addr << E1000_MDIC_PHY_SHIFT |
                        E1000_MDIC_OP_READ);
    uint32_t mdic = 0;
    for (int i = 0; i < 640; i++) {   /* E1000_GEN_POLL_TIMEOUT * 3, 50 us apart */
        udelay(50);
        mdic = er32(c, E1000_MDIC);
        if (mdic & E1000_MDIC_READY) {
            break;
        }
    }
    if (!(mdic & E1000_MDIC_READY) || (mdic & E1000_MDIC_ERROR) || ((mdic >> E1000_MDIC_REG_SHIFT) & 0x1F) != reg) {
        return false;
    }
    *out = (uint16_t)mdic;
    return true;
}

bool e1000_phy_write(struct e1000_card *c, uint32_t reg, uint16_t value) {
    ew32(c, E1000_MDIC, value | reg << E1000_MDIC_REG_SHIFT | (uint32_t)c->phy_addr << E1000_MDIC_PHY_SHIFT |
                        E1000_MDIC_OP_WRITE);
    uint32_t mdic = 0;
    for (int i = 0; i < 640; i++) {
        udelay(50);
        mdic = er32(c, E1000_MDIC);
        if (mdic & E1000_MDIC_READY) {
            break;
        }
    }
    return (mdic & E1000_MDIC_READY) && !(mdic & E1000_MDIC_ERROR);
}

void e1000_phy_autoneg(struct e1000_card *c) {
    uint16_t adv, gig, bmcr;
    if (!e1000_phy_read(c, MII_ADVERTISE, &adv) || !e1000_phy_read(c, MII_CTRL1000, &gig)) {
        return;   /* (no PHY to talk to: the MAC's defaults will have to do) */
    }
    adv = (uint16_t)((adv & ~(ADVERTISE_ALL | ADVERTISE_PAUSE)) | ADVERTISE_ALL | ADVERTISE_PAUSE);
    gig = (uint16_t)((gig & ~ADVERTISE_1000HALF) | ADVERTISE_1000FULL);   /* (1000 half isn't supported) */
    e1000_phy_write(c, MII_ADVERTISE, adv);
    e1000_phy_write(c, MII_CTRL1000, gig);
    if (e1000_phy_read(c, MII_BMCR, &bmcr)) {
        e1000_phy_write(c, MII_BMCR, (uint16_t)(bmcr | BMCR_ANENABLE | BMCR_ANRESTART));
    }
}

void e1000_init_rx_addrs(struct e1000_card *c, int rar_count, int mta_count) {
    const uint8_t *m = c->dev.mac;
    ew32(c, E1000_RAL0, (uint32_t)m[0] | (uint32_t)m[1] << 8 | (uint32_t)m[2] << 16 | (uint32_t)m[3] << 24);
    ew32(c, E1000_RAH0, (uint32_t)m[4] | (uint32_t)m[5] << 8 | E1000_RAH_AV);
    for (int i = 1; i < rar_count; i++) {
        ew32(c, E1000_RAL0 + 8 * (uint32_t)i, 0);
        ew32(c, E1000_RAH0 + 8 * (uint32_t)i, 0);
    }
    for (int i = 0; i < mta_count; i++) {
        ew32(c, E1000_MTA + 4 * (uint32_t)i, 0);
    }
}

/* --- link ---------------------------------------------------------------------------------------- */

static void update_link(struct e1000_card *c, bool report) {
    uint32_t status = er32(c, E1000_STATUS);
    bool up = status & E1000_STATUS_LU;
    if (up == c->dev.link_up && report) {
        return;
    }
    c->dev.link_up = up;
    c->dev.speed_mbps = status & E1000_STATUS_SPEED_1000 ? 1000 : status & E1000_STATUS_SPEED_100 ? 100 : 10;
    c->dev.full_duplex = status & E1000_STATUS_FD;
    if (up) {
        /* e1000e_config_collision_dist: the collision distance for the duplex we got. */
        uint32_t tctl = er32(c, E1000_TCTL) & ~E1000_TCTL_COLD;
        ew32(c, E1000_TCTL, tctl | (c->dev.full_duplex ? c->collision_distance : 0x200) << E1000_COLD_SHIFT);
    }
    if (report) {
        net_link_changed(&c->dev);
    }
}

/* --- the data path ----------------------------------------------------------------------------- */

static int e1000_transmit(struct net_device *dev, const void *frame, size_t len) {
    struct e1000_card *c = dev->priv;
    uint64_t irq = irq_save();
    /* e1000_clean_tx_irq: what the card has finished with. */
    while (c->tx_clean != c->tx_tail && (c->tx[c->tx_clean].status & E1000_TXD_STAT_DD)) {
        c->tx_clean = (c->tx_clean + 1) % E1000_RING_SIZE;
    }
    uint32_t next = (c->tx_tail + 1) % E1000_RING_SIZE;
    if (next == c->tx_clean) {
        irq_restore(irq);
        return -1;   /* ring full */
    }
    volatile struct e1000_tx_desc *d = &c->tx[c->tx_tail];
    memcpy(c->tx_bufs + (uint64_t)c->tx_tail * E1000_BUF_SIZE, frame, len);
    d->addr = c->tx_bufs_phys + (uint64_t)c->tx_tail * E1000_BUF_SIZE;
    d->length = (uint16_t)(len < 60 ? 60 : len);   /* (the card pads short frames, but not with zeroes) */
    if (len < 60) {
        memset(c->tx_bufs + (uint64_t)c->tx_tail * E1000_BUF_SIZE + len, 0, 60 - len);
    }
    d->cso = 0;
    d->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    d->status = 0;
    d->css = 0;
    d->special = 0;
    c->tx_tail = next;
    __asm__ volatile ("sfence" ::: "memory");
    ew32(c, E1000_TDT, c->tx_tail);
    dev->tx_packets++;
    dev->tx_bytes += len;
    irq_restore(irq);
    return 0;
}

/* e1000_clean_rx_irq: hand every finished receive buffer up, then give it back to the card. */
static void e1000_poll(struct net_device *dev) {
    struct e1000_card *c = dev->priv;
    uint32_t done = 0;
    for (;;) {
        volatile struct e1000_rx_desc *d = &c->rx[c->rx_next];
        uint8_t status = d->status;
        if (!(status & E1000_RXD_STAT_DD)) {
            break;
        }
        __asm__ volatile ("lfence" ::: "memory");
        uint16_t len = d->length;
        if ((status & E1000_RXD_STAT_EOP) && !d->errors && len) {
            net_receive(dev, c->rx_bufs + (uint64_t)c->rx_next * E1000_BUF_SIZE, len);
        } else {
            dev->rx_errors++;   /* (a frame bigger than one buffer, or a bad one) */
        }
        d->status = 0;
        d->addr = c->rx_bufs_phys + (uint64_t)c->rx_next * E1000_BUF_SIZE;
        c->rx_next = (c->rx_next + 1) % E1000_RING_SIZE;
        done++;
    }
    if (done) {
        /* The tail is the last buffer the card may fill: the one just before the next to look at. */
        ew32(c, E1000_RDT, (c->rx_next + E1000_RING_SIZE - 1) % E1000_RING_SIZE);
    }
    if (!c->dev.has_interrupt) {
        er32(c, E1000_ICR);   /* (polled: clear the causes ourselves) */
    }
    update_link(c, true);
}

static void e1000_interrupt(void *arg) {
    struct e1000_card *c = arg;
    er32(c, E1000_ICR);   /* reading acknowledges every cause */
    net_wake();
}

bool e1000_start(struct e1000_card *c, const char *driver, bool msix_ok) {
    void *rx = NULL, *tx = NULL, *rxb = NULL, *txb = NULL;
    uint64_t rx_phys = nic_dma_alloc(E1000_RING_SIZE * sizeof(struct e1000_rx_desc), false, &rx);
    uint64_t tx_phys = nic_dma_alloc(E1000_RING_SIZE * sizeof(struct e1000_tx_desc), false, &tx);
    c->rx_bufs_phys = nic_dma_alloc((uint64_t)E1000_RING_SIZE * E1000_BUF_SIZE, false, &rxb);
    c->tx_bufs_phys = nic_dma_alloc((uint64_t)E1000_RING_SIZE * E1000_BUF_SIZE, false, &txb);
    if (!rx_phys || !tx_phys || !c->rx_bufs_phys || !c->tx_bufs_phys) {
        kprintf("[WARN] %s: out of memory for the rings\n", driver);
        return false;
    }
    c->rx = rx;
    c->tx = tx;
    c->rx_bufs = rxb;
    c->tx_bufs = txb;

    /* e1000_configure_tx */
    ew32(c, E1000_TDBAL, (uint32_t)tx_phys);
    ew32(c, E1000_TDBAH, (uint32_t)(tx_phys >> 32));
    ew32(c, E1000_TDLEN, E1000_RING_SIZE * sizeof(struct e1000_tx_desc));
    ew32(c, E1000_TDH, 0);
    ew32(c, E1000_TDT, 0);
    ew32(c, E1000_TIDV, 0);
    ew32(c, E1000_TADV, 0);
    uint32_t tctl = er32(c, E1000_TCTL) & ~(E1000_TCTL_CT | E1000_TCTL_COLD);
    tctl |= E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_RTLC | E1000_COLLISION_THRESHOLD << E1000_CT_SHIFT |
            c->collision_distance << E1000_COLD_SHIFT;
    ew32(c, E1000_TCTL, tctl);

    /* e1000_configure_rx + e1000_setup_rctl: every buffer handed to the card. */
    for (uint32_t i = 0; i < E1000_RING_SIZE; i++) {
        c->rx[i].addr = c->rx_bufs_phys + (uint64_t)i * E1000_BUF_SIZE;
        c->rx[i].status = 0;
    }
    ew32(c, E1000_RDTR, 0);
    ew32(c, E1000_RADV, 0);
    ew32(c, E1000_ITR, 0);   /* no interrupt throttling: latency over throughput at our traffic */
    ew32(c, E1000_RDBAL, (uint32_t)rx_phys);
    ew32(c, E1000_RDBAH, (uint32_t)(rx_phys >> 32));
    ew32(c, E1000_RDLEN, E1000_RING_SIZE * sizeof(struct e1000_rx_desc));
    ew32(c, E1000_RDH, 0);
    ew32(c, E1000_RDT, E1000_RING_SIZE - 1);
    ew32(c, E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SZ_2048 | E1000_RCTL_SECRC |
                        E1000_RCTL_RDMTS_HALF);

    struct net_device *dev = &c->dev;
    dev->driver = driver;
    dev->priv = c;
    dev->transmit = e1000_transmit;
    dev->poll = e1000_poll;
    strlcpy(dev->model, c->name, sizeof(dev->model));
    dev->has_interrupt = pci_enable_msi(&c->pci, e1000_interrupt, c, msix_ok);
    er32(c, E1000_ICR);
    if (dev->has_interrupt) {
        ew32(c, E1000_IMS, E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_RXO | E1000_ICR_LSC | E1000_ICR_TXDW |
                           E1000_ICR_RXSEQ);
    }
    update_link(c, false);
    net_register(dev);
    return true;
}
