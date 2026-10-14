/*
 * Intel 8254x gigabit Ethernet (82540/82541/82543/82544/82545/82546/82547: QEMU's "e1000", and the
 * PCI/PCI-X cards of the 2000s), after linux/drivers/net/intel/e1000: e1000_reset_hw, e1000_init_hw
 * and the copper link setup, for copper cards with the internal PHY. The data path is shared with
 * e1000e (e1000_common.c). These chips have no MSI: they're polled every tick.
 */

#include "drivers/net/e1000_hw.h"
#include "drivers/timer.h"
#include "arch/x86_64/cpu.h"
#include "lib/kprintf.h"
#include "sched/sched.h"

#define E1000_CTRL_DUP              0x00004   /* shadow of CTRL, for resets on 82545/82546 rev 3 */
#define E1000_MANC_ARP_EN           0x00002000
#define E1000_TXDCTL                E1000_TXDCTL0
#define M88E1000_PHY_SPEC_CTRL      0x10
#define M88E1000_PSCR_POLARITY_REVERSAL 0x0002
#define M88E1000_PSCR_AUTO_X_MODE   0x0060
#define E1000_COLLISION_DISTANCE    0x40

enum mac_type { M_82543, M_82544, M_82540, M_82545, M_82545_REV3, M_82546, M_82546_REV3, M_82541, M_82547 };

static const struct {
    uint16_t device;
    enum mac_type type;
    const char *name;
} ids[] = {
    { 0x1001, M_82543, "Intel 82543GC Gigabit (fiber)" },
    { 0x1004, M_82543, "Intel 82543GC Gigabit" },
    { 0x1008, M_82544, "Intel 82544EI Gigabit" },
    { 0x1009, M_82544, "Intel 82544EI Gigabit (fiber)" },
    { 0x100C, M_82544, "Intel 82544GC Gigabit" },
    { 0x100D, M_82544, "Intel 82544GC Gigabit (LOM)" },
    { 0x100E, M_82540, "Intel 82540EM Gigabit" },
    { 0x1015, M_82540, "Intel 82540EM Gigabit (LOM)" },
    { 0x1016, M_82540, "Intel 82540EP Gigabit (LOM)" },
    { 0x1017, M_82540, "Intel 82540EP Gigabit" },
    { 0x101E, M_82540, "Intel 82540EP Gigabit (mobile)" },
    { 0x100F, M_82545, "Intel 82545EM Gigabit" },
    { 0x1011, M_82545, "Intel 82545EM Gigabit (fiber)" },
    { 0x1026, M_82545_REV3, "Intel 82545GM Gigabit" },
    { 0x1027, M_82545_REV3, "Intel 82545GM Gigabit (fiber)" },
    { 0x1028, M_82545_REV3, "Intel 82545GM Gigabit (SerDes)" },
    { 0x1010, M_82546, "Intel 82546EB Gigabit" },
    { 0x1012, M_82546, "Intel 82546EB Gigabit (fiber)" },
    { 0x101D, M_82546, "Intel 82546EB Gigabit (quad)" },
    { 0x1079, M_82546_REV3, "Intel 82546GB Gigabit" },
    { 0x107A, M_82546_REV3, "Intel 82546GB Gigabit (fiber)" },
    { 0x107B, M_82546_REV3, "Intel 82546GB Gigabit (SerDes)" },
    { 0x108A, M_82546_REV3, "Intel 82546GB Gigabit (PCIe)" },
    { 0x1099, M_82546_REV3, "Intel 82546GB Gigabit (quad)" },
    { 0x10B5, M_82546_REV3, "Intel 82546GB Gigabit (quad)" },
    { 0x1013, M_82541, "Intel 82541EI Gigabit" },
    { 0x1018, M_82541, "Intel 82541EI Gigabit (mobile)" },
    { 0x1014, M_82541, "Intel 82541ER Gigabit (LOM)" },
    { 0x1078, M_82541, "Intel 82541ER Gigabit" },
    { 0x1076, M_82541, "Intel 82541GI Gigabit" },
    { 0x1077, M_82541, "Intel 82541GI Gigabit (mobile)" },
    { 0x107C, M_82541, "Intel 82541GI Gigabit (LF)" },
    { 0x1019, M_82547, "Intel 82547EI Gigabit" },
    { 0x101A, M_82547, "Intel 82547EI Gigabit (mobile)" },
    { 0x1075, M_82547, "Intel 82547GI Gigabit" },
};

#define MAX_CARDS 4
static struct e1000_card cards[MAX_CARDS];
static int card_count;

/* Some of these chips can't acknowledge the reset write over MMIO: go through the I/O window. */
static void write_ctrl_io(struct e1000_card *c, uint32_t value) {
    if (c->io_base) {
        outl_port(c->io_base, E1000_CTRL);         /* IOADDR */
        outl_port((uint16_t)(c->io_base + 4), value);  /* IODATA */
    } else {
        ew32(c, E1000_CTRL, value);
    }
}

/* e1000_reset_hw */
static void reset_hw(struct e1000_card *c, enum mac_type t) {
    ew32(c, E1000_IMC, 0xFFFFFFFF);
    ew32(c, E1000_RCTL, 0);
    ew32(c, E1000_TCTL, E1000_TCTL_PSP);
    e1e_flush(c);
    sched_sleep_ms(10);

    uint32_t ctrl = er32(c, E1000_CTRL);
    if (t == M_82541 || t == M_82547) {   /* the PHY before the MAC */
        ew32(c, E1000_CTRL, ctrl | E1000_CTRL_PHY_RST);
        e1e_flush(c);
        sched_sleep_ms(5);
    }
    switch (t) {
    case M_82544:
    case M_82540:
    case M_82545:
    case M_82546:
    case M_82541:
        write_ctrl_io(c, ctrl | E1000_CTRL_RST);
        break;
    case M_82545_REV3:
    case M_82546_REV3:
        ew32(c, E1000_CTRL_DUP, ctrl | E1000_CTRL_RST);
        break;
    default:
        ew32(c, E1000_CTRL, ctrl | E1000_CTRL_RST);
        break;
    }
    /* Then the EEPROM reloads the power-on settings. */
    switch (t) {
    case M_82543:
    case M_82544:
        udelay(10);
        ew32(c, E1000_CTRL_EXT, er32(c, E1000_CTRL_EXT) | E1000_CTRL_EXT_EE_RST);
        e1e_flush(c);
        sched_sleep_ms(2);
        break;
    case M_82541:
    case M_82547:
        sched_sleep_ms(20);
        break;
    default:
        sched_sleep_ms(5);   /* e1000_get_auto_rd_done */
        break;
    }
    if (t >= M_82540) {
        ew32(c, E1000_MANC, er32(c, E1000_MANC) & ~E1000_MANC_ARP_EN);   /* no ARP answers by the card itself */
    }
    ew32(c, E1000_IMC, 0xFFFFFFFF);
    er32(c, E1000_ICR);
}

/* e1000_init_hw + e1000_setup_copper_link (the M88/IGP internal PHYs: auto MDI-X, auto-negotiation). */
static void init_hw(struct e1000_card *c, enum mac_type t) {
    ew32(c, E1000_VET, 0);
    for (uint32_t i = 0; i < 128; i++) {
        ew32(c, E1000_VFTA + 4 * i, 0);
    }
    e1000_init_rx_addrs(c, 15, 128);

    uint32_t ctrl = er32(c, E1000_CTRL);
    ctrl |= E1000_CTRL_SLU;
    ctrl &= ~(E1000_CTRL_FRCSPD | E1000_CTRL_FRCDPX);
    ew32(c, E1000_CTRL, ctrl);

    if (t == M_82540 || t == M_82545 || t == M_82545_REV3 || t == M_82546 || t == M_82546_REV3 || t == M_82544) {
        /* e1000_copper_link_mgp_setup (M88 PHYs): automatic MDI/MDI-X crossover, polarity correction;
           a PHY reset commits it. */
        uint16_t pscr;
        if (e1000_phy_read(c, M88E1000_PHY_SPEC_CTRL, &pscr)) {
            pscr |= M88E1000_PSCR_AUTO_X_MODE;
            pscr &= ~M88E1000_PSCR_POLARITY_REVERSAL;
            e1000_phy_write(c, M88E1000_PHY_SPEC_CTRL, pscr);
            uint16_t bmcr;
            if (e1000_phy_read(c, MII_BMCR, &bmcr)) {
                e1000_phy_write(c, MII_BMCR, (uint16_t)(bmcr | BMCR_RESET));
                udelay(1);
            }
        }
    }
    if (t != M_82543) {
        e1000_phy_autoneg(c);
    }

    if (t > M_82544) {
        uint32_t txdctl = er32(c, E1000_TXDCTL);
        ew32(c, E1000_TXDCTL, (txdctl & ~E1000_TXDCTL_WTHRESH) | E1000_TXDCTL_FULL_TX_DESC_WB);
    }
    ew32(c, E1000_TIPG, 8 | 8 << 10 | 6 << 20);   /* copper: IPGT 8, IPGR1 8, IPGR2 6 */
}

static void probe_one(const struct pci_device *d, enum mac_type t, const char *name) {
    if (card_count == MAX_CARDS) {
        return;
    }
    struct e1000_card *c = &cards[card_count];
    memset(c, 0, sizeof(*c));
    if (!e1000_map(c, d)) {
        kprintf("[WARN] e1000: %02x:%02x.%u has no register window\n", d->bus, d->slot, d->function);
        return;
    }
    c->name = name;
    c->phy_addr = 1;
    c->collision_distance = E1000_COLLISION_DISTANCE;
    reset_hw(c, t);
    if (!e1000_read_mac_rar(c)) {
        uint16_t w[3];
        if ((t == M_82541 || t == M_82547) && e1000_read_eerd(c, 0, &w[0]) && e1000_read_eerd(c, 1, &w[1]) &&
            e1000_read_eerd(c, 2, &w[2])) {
            uint8_t m[6] = { (uint8_t)w[0], (uint8_t)(w[0] >> 8), (uint8_t)w[1], (uint8_t)(w[1] >> 8),
                             (uint8_t)w[2], (uint8_t)(w[2] >> 8) };
            memcpy(c->dev.mac, m, 6);
        } else {
            kprintf("[WARN] e1000: no MAC address in the EEPROM\n");
            return;
        }
    }
    init_hw(c, t);
    card_count++;
    e1000_start(c, "e1000", false);
}

void e1000_probe(void) {
    struct pci_device d;
    for (size_t k = 0; k < sizeof(ids) / sizeof(ids[0]); k++) {
        for (int i = 0; pci_find_id(0x8086, ids[k].device, i, &d); i++) {
            probe_one(&d, ids[k].type, ids[k].name);
        }
    }
}
