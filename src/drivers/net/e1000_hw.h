#pragma once

/*
 * Intel gigabit Ethernet: what the e1000 (8254x) and e1000e (8257x, ICH/PCH: I217-I219) drivers
 * share. The register map is linux/drivers/net/intel/e1000e/{regs,defines}.h (e1000's is the same
 * for everything used here), and both use the same legacy descriptor rings, so the data path
 * (e1000_common.c) is one: the chip-specific files only reset and set up the hardware.
 */

#include <stdbool.h>
#include <stdint.h>

#include "drivers/net/nic.h"
#include "drivers/pci.h"

/* Registers */
#define E1000_CTRL        0x00000
#define E1000_STATUS      0x00008
#define E1000_EECD        0x00010
#define E1000_EERD        0x00014
#define E1000_CTRL_EXT    0x00018
#define E1000_MDIC        0x00020
#define E1000_FEXTNVM     0x00028
#define E1000_FEXTNVM3    0x0003C
#define E1000_VET         0x00038
#define E1000_FEXTNVM7    0x000E4
#define E1000_ICR         0x000C0
#define E1000_ITR         0x000C4
#define E1000_IMS         0x000D0
#define E1000_IMC         0x000D8
#define E1000_RCTL        0x00100
#define E1000_TCTL        0x00400
#define E1000_TIPG        0x00410
#define E1000_EXTCNF_CTRL 0x00F00
#define E1000_IOSFPC      0x00F28
#define E1000_PBA         0x01000
#define E1000_PBECCSTS    0x0100C
#define E1000_RDBAL       0x02800
#define E1000_RDBAH       0x02804
#define E1000_RDLEN       0x02808
#define E1000_RDH         0x02810
#define E1000_RDT         0x02818
#define E1000_RDTR        0x02820
#define E1000_RADV        0x0282C
#define E1000_KABGTXD     0x03004
#define E1000_TDBAL       0x03800
#define E1000_TDBAH       0x03804
#define E1000_TDLEN       0x03808
#define E1000_TDH         0x03810
#define E1000_TDT         0x03818
#define E1000_TIDV        0x03820
#define E1000_TXDCTL0     0x03828
#define E1000_TADV        0x0382C
#define E1000_TARC0       0x03840
#define E1000_TXDCTL1     0x03928
#define E1000_TARC1       0x03940
#define E1000_RFCTL       0x05008
#define E1000_MTA         0x05200
#define E1000_RAL0        0x05400
#define E1000_RAH0        0x05404
#define E1000_VFTA        0x05600
#define E1000_MANC        0x05820
#define E1000_GCR         0x05B00
#define E1000_H2ME        0x05B50
#define E1000_FWSM        0x05B54
#define E1000_GCR2        0x05B64

/* CTRL */
#define E1000_CTRL_FD                 0x00000001
#define E1000_CTRL_GIO_MASTER_DISABLE 0x00000004
#define E1000_CTRL_ASDE               0x00000020
#define E1000_CTRL_SLU                0x00000040
#define E1000_CTRL_FRCSPD             0x00000800
#define E1000_CTRL_FRCDPX             0x00001000
#define E1000_CTRL_LANPHYPC_OVERRIDE  0x00010000
#define E1000_CTRL_LANPHYPC_VALUE     0x00020000
#define E1000_CTRL_MEHE               0x00080000
#define E1000_CTRL_RST                0x04000000
#define E1000_CTRL_PHY_RST            0x80000000
/* CTRL_EXT */
#define E1000_CTRL_EXT_LPCD           0x00000004
#define E1000_CTRL_EXT_DPG_EN         0x00000008
#define E1000_CTRL_EXT_FORCE_SMBUS    0x00000800
#define E1000_CTRL_EXT_EE_RST         0x00002000
#define E1000_CTRL_EXT_RO_DIS         0x00020000
#define E1000_CTRL_EXT_DMA_DYN_CLK_EN 0x00080000
#define E1000_CTRL_EXT_PHYPDEN        0x00100000
/* STATUS */
#define E1000_STATUS_FD               0x00000001
#define E1000_STATUS_LU               0x00000002
#define E1000_STATUS_SPEED_100        0x00000040
#define E1000_STATUS_SPEED_1000       0x00000080
#define E1000_STATUS_LAN_INIT_DONE    0x00000200
#define E1000_STATUS_PHYRA            0x00000400
#define E1000_STATUS_GIO_MASTER_ENABLE 0x00080000
/* EECD / EERD */
#define E1000_EECD_AUTO_RD            0x00000200
#define E1000_EERD_START              0x00000001
#define E1000_EERD_DONE               0x00000010
#define E1000_EERD_ADDR_SHIFT         8
#define E1000_EERD_DATA_SHIFT         16
/* MDIC */
#define E1000_MDIC_REG_SHIFT          16
#define E1000_MDIC_PHY_SHIFT          21
#define E1000_MDIC_OP_WRITE           0x04000000
#define E1000_MDIC_OP_READ            0x08000000
#define E1000_MDIC_READY              0x10000000
#define E1000_MDIC_ERROR              0x40000000
/* RCTL */
#define E1000_RCTL_EN                 0x00000002
#define E1000_RCTL_RDMTS_HALF         0x00000000
#define E1000_RCTL_BAM                0x00008000
#define E1000_RCTL_SZ_2048            0x00000000
#define E1000_RCTL_SECRC              0x04000000
#define E1000_RCTL_RDMTS_HEX          0x00010000
/* TCTL */
#define E1000_TCTL_EN                 0x00000002
#define E1000_TCTL_PSP                0x00000008
#define E1000_TCTL_CT                 0x00000FF0
#define E1000_TCTL_COLD               0x003FF000
#define E1000_TCTL_RTLC               0x01000000
#define E1000_TCTL_MULR               0x10000000
#define E1000_CT_SHIFT                4
#define E1000_COLD_SHIFT              12
#define E1000_COLLISION_THRESHOLD     15
/* Descriptors */
#define E1000_TXD_CMD_EOP             0x01
#define E1000_TXD_CMD_IFCS            0x02
#define E1000_TXD_CMD_RS              0x08
#define E1000_TXD_STAT_DD             0x01
#define E1000_RXD_STAT_DD             0x01
#define E1000_RXD_STAT_EOP            0x02
/* Interrupt causes */
#define E1000_ICR_TXDW                0x00000001
#define E1000_ICR_LSC                 0x00000004
#define E1000_ICR_RXSEQ               0x00000008
#define E1000_ICR_RXDMT0              0x00000010
#define E1000_ICR_RXO                 0x00000040
#define E1000_ICR_RXT0                0x00000080
/* TXDCTL */
#define E1000_TXDCTL_PTHRESH          0x0000003F
#define E1000_TXDCTL_WTHRESH          0x003F0000
#define E1000_TXDCTL_FULL_TX_DESC_WB  0x01010000
#define E1000_TXDCTL_MAX_TX_DESC_PREFETCH 0x0100001F
#define E1000_TXDCTL_COUNT_DESC       0x00400000
#define E1000_RAH_AV                  0x80000000

/* PHY (MII) registers and bits */
#define MII_BMCR          0x00
#define MII_BMSR          0x01
#define MII_PHYSID1       0x02
#define MII_PHYSID2       0x03
#define MII_ADVERTISE     0x04
#define MII_CTRL1000      0x09
#define BMCR_ANRESTART    0x0200
#define BMCR_ANENABLE     0x1000
#define BMCR_RESET        0x8000
#define ADVERTISE_ALL     0x01E0   /* 10/100, half and full */
#define ADVERTISE_PAUSE   0x0C00   /* symmetric + asymmetric pause */
#define ADVERTISE_1000FULL 0x0200
#define ADVERTISE_1000HALF 0x0100

#define E1000_RING_SIZE  256
#define E1000_BUF_SIZE   2048

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
};

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
};

/* One card (either driver). */
struct e1000_card {
    struct net_device dev;
    struct pci_device pci;
    volatile uint8_t *regs;
    uint16_t io_base;                  /* I/O BAR (8254x resets go through it), 0 if none */
    int phy_addr;
    uint32_t collision_distance;
    const char *name;                  /* the chip, for the model string */

    volatile struct e1000_rx_desc *rx;
    volatile struct e1000_tx_desc *tx;
    uint8_t *rx_bufs, *tx_bufs;
    uint64_t rx_bufs_phys, tx_bufs_phys;
    uint32_t rx_next;                  /* next descriptor to look at */
    uint32_t tx_tail, tx_clean;        /* next free / oldest in flight */
};

static inline uint32_t er32(struct e1000_card *c, uint32_t reg) {
    return *(volatile uint32_t *)(c->regs + reg);
}
static inline void ew32(struct e1000_card *c, uint32_t reg, uint32_t v) {
    *(volatile uint32_t *)(c->regs + reg) = v;
}
static inline void e1e_flush(struct e1000_card *c) {
    (void)er32(c, E1000_STATUS);
}

/* e1000_common.c */
/* Map BAR 0, turn on memory decoding and bus mastering. False if it can't. */
bool e1000_map(struct e1000_card *c, const struct pci_device *d);
/* The MAC address the firmware/EEPROM put in receive address 0; false if there's none. */
bool e1000_read_mac_rar(struct e1000_card *c);
/* Word `index` of the EEPROM through EERD (8254x from 82541, and e1000e's 8257x). */
bool e1000_read_eerd(struct e1000_card *c, uint16_t index, uint16_t *out);
/* PHY registers through MDIC (e1000e_read_phy_reg_mdic): at the card's PHY address. */
bool e1000_phy_read(struct e1000_card *c, uint32_t reg, uint16_t *out);
bool e1000_phy_write(struct e1000_card *c, uint32_t reg, uint16_t value);
/* Advertise every speed and restart auto-negotiation (e1000e_phy_setup_autoneg + restart). */
void e1000_phy_autoneg(struct e1000_card *c);
/* Receive address 0 = our MAC, the rest and the multicast table empty (e1000e_init_rx_addrs). */
void e1000_init_rx_addrs(struct e1000_card *c, int rar_count, int mta_count);
/* Rings, receive/transmit on, interrupts (MSI if the card has it, else polled), register with the
   network stack (e1000_configure_tx/rx, e1000_setup_rctl). */
bool e1000_start(struct e1000_card *c, const char *driver, bool msix_ok);
