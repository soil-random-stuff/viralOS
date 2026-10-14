/*
 * Realtek PCI Express Ethernet, after linux/drivers/net/realtek/r8169_main.c and r8169_phy_config.c:
 * the chips on most desktop and laptop boards since about 2013,
 *   - RTL8168G/GU, RTL8411B, RTL8168H/M, RTL8168EP, RTL8168FP/RTL8117 (gigabit; RTL8111x is the same
 *     chip under another name) and their 100 Mbit cousins RTL8106EUS/RTL8107E;
 *   - RTL8125A/B/D/BP/CP and RTL9151A (2.5 gigabit), RTL8126A (5 gigabit).
 * Older chips (RTL8169, RTL8101, RTL8168B-F) aren't ported; they get a message.
 *
 * For each chip: identification by the XID in TxConfig, the MAC init (rtl_hw_init_8168g/_8125),
 * rtl_hw_start's chip-specific sequence with its PCIe PHY (EPHY) table, and the PHY tuning of
 * r8169_hw_phy_config. Not ported: the PHY firmware patches (the rtl_nic firmware files: Linux works without them
 * too, with a warning), ASPM/LTR power saving (left off, as Linux does unless the BIOS vouches for
 * it), Wake-on-LAN, DASH remote management, offloads and jumbo frames. Interrupts by MSI/MSI-X.
 */

#include "drivers/net/nic.h"
#include "arch/x86_64/cpu.h"
#include "drivers/pci.h"
#include "drivers/timer.h"
#include "lib/kprintf.h"
#include "mm/vmm.h"
#include "sched/sched.h"

/* Registers (enum rtl_registers, rtl8168_8101_registers, rtl8168_registers, rtl8125_registers) */
#define MAC0                0x00
#define MAC4                0x04
#define MAR0                0x08
#define TxDescStartAddrLow  0x20
#define TxDescStartAddrHigh 0x24
#define ChipCmd             0x37
#define TxPoll              0x38
#define IntrMask            0x3c
#define IntrStatus          0x3e
#define TxConfig            0x40
#define RxConfig            0x44
#define Cfg9346             0x50
#define Config1             0x52
#define Config2             0x53
#define Config3             0x54
#define Config5             0x56
#define CSIDR               0x64
#define CSIAR               0x68
#define PHYstatus           0x6c
#define PMCH                0x6f
#define ERIDR               0x70
#define ERIAR               0x74
#define EPHYAR              0x80
#define OCPDR               0xb0
#define GPHY_OCP            0xb8
#define DLLPR               0xd0
#define MCU                 0xd3
#define RxMaxSize           0xda
#define CPlusCmd            0xe0
#define IntrMitigate        0xe2
#define RxDescAddrLow       0xe4
#define RxDescAddrHigh      0xe8
#define MaxTxPacketSize     0xec
#define MISC                0xf0
#define MISC_1              0xf2
#define INT_CFG0_8125       0x34
#define IntrMask_8125       0x38
#define IntrStatus_8125     0x3c
#define INT_CFG1_8125       0x7a
#define TxPoll_8125         0x90
#define MAC0_BKP            0x19e0
#define RSS_CTRL_8125       0x4500
#define Q_NUM_CTRL_8125     0x4800
#define EEE_TXIDLE_TIMER_8125 0x6048
#define TX_CONFIG_V2        0x60b0

/* Bits */
#define CmdReset        0x10
#define CmdRxEnb        0x08
#define CmdTxEnb        0x04
#define StopReq         0x80
#define NPQ             0x40
#define Cfg9346_Lock    0x00
#define Cfg9346_Unlock  0xc0
#define AcceptBroadcast 0x08
#define AcceptMulticast 0x04
#define AcceptMyPhys    0x02
#define RX_CONFIG_ACCEPT_MASK 0x3f
#define RX_CONFIG_ACCEPT_OK_MASK 0x0f
#define RX_CONFIG_ACCEPT_ERR_MASK 0x30
#define RX128_INT_EN    (1u << 15)
#define RX_MULTI_EN     (1u << 14)
#define RX_EARLY_OFF    (1u << 11)
#define RX_PAUSE_SLOT_ON (1u << 11)
#define RX_DMA_BURST    (7u << 8)
#define RX_FETCH_DFLT_8125 (8u << 27)
#define RX_VLAN_8125    (3u << 22)
#define TXCFG_AUTO_FIFO (1u << 7)
#define TXCFG_EMPTY     (1u << 11)
#define TX_DMA_BURST    7
#define InterFrameGap   0x03
#define EarlySize       0x27
#define TxPacketMax     (8064 >> 7)
#define RxVlan          (1u << 6)
#define RxChkSum        (1u << 5)
#define CPCMD_MASK      ((1u << 13) | RxVlan | RxChkSum | 3u)
#define ClkReqEn        (1u << 7)
#define ASPM_en         (1u << 0)
#define Rdy_to_L23      (1u << 1)
#define INT_CFG0_CLKREQEN (1u << 3)
#define D3COLD_NO_PLL_DOWN (1u << 7)
#define D3HOT_NO_PLL_DOWN  (1u << 6)
#define PFM_EN          (1u << 6)
#define TX_10M_PS_EN    (1u << 7)
#define PFM_D3COLD_EN   (1u << 6)
#define NOW_IS_OOB      (1u << 7)
#define TX_EMPTY        (1u << 5)
#define RX_EMPTY        (1u << 4)
#define RXTX_EMPTY      (TX_EMPTY | RX_EMPTY)
#define LINK_LIST_RDY   (1u << 1)
#define RXDV_GATED_EN   (1u << 19)
#define ERIAR_FLAG      0x80000000u
#define ERIAR_WRITE_CMD 0x80000000u
#define ERIAR_EXGMAC    0
#define ERIAR_OOB       (0x02u << 16)
#define ERIAR_MASK_0001 (0x1u << 12)
#define ERIAR_MASK_0011 (0x3u << 12)
#define ERIAR_MASK_1111 (0xfu << 12)
#define EPHYAR_FLAG     0x80000000u
#define EPHYAR_WRITE_CMD 0x80000000u
#define CSIAR_FLAG      0x80000000u
#define CSIAR_WRITE_CMD 0x80000000u
#define CSIAR_BYTE_ENABLE 0x0000f000u
#define OCPAR_FLAG      0x80000000u
#define OCP_STD_PHY_BASE 0xa400

/* Interrupts */
#define LinkChg         0x0020
#define TxErr           0x0008
#define TxOK            0x0004
#define RxErr           0x0002
#define RxOK            0x0001

/* PHYstatus */
#define LinkStatus      0x02
#define FullDup         0x01
#define _1000bpsF       0x10
#define _100bps         0x08
#define _2500bpsF       0x400    /* (8125, 16-bit register: the vendor's r8125 driver) */
#define _5000bpsF       0x1000

/* Descriptors */
#define DescOwn         (1u << 31)
#define RingEnd         (1u << 30)
#define FirstFrag       (1u << 29)
#define LastFrag        (1u << 28)
#define RxRES           (1u << 21)

#define MII_BMCR        0x00
#define MII_ADVERTISE   0x04
#define MII_CTRL1000    0x09
#define BMCR_RESET      0x8000
#define BMCR_ANENABLE   0x1000
#define BMCR_ANRESTART  0x0200
#define BMCR_PDOWN      0x0800
#define RTL_ADV_2500FULL (1u << 7)   /* page 0xa5d reg 0x12 (drivers/net/phy/realtek) */

#define RING_SIZE   256
#define BUF_SIZE    2048
#define RX_MAX_SIZE 16384            /* rtl_set_rx_max_size: R8169_RX_BUF_SIZE + 1 */

enum mac_version {
    VER_40, VER_42, VER_43, VER_44, VER_46, VER_48, VER_51, VER_52,
    VER_61, VER_63, VER_64, VER_65, VER_66, VER_70,
    VER_EXTENDED, VER_NONE, VER_OLD,
};

/* rtl_chip_infos, the part that is ported (in the same order: first match wins). */
static const struct {
    uint32_t mask, val;
    enum mac_version ver;
    const char *name;
} chips[] = {
    { 0x7cf, 0x6c9, VER_OLD, "RTL8127A" },   /* (10 gigabit: not ported) */
    { 0x7cf, 0x64a, VER_70, "RTL8126A" },
    { 0x7cf, 0x649, VER_70, "RTL8126A" },
    { 0x7cf, 0x681, VER_66, "RTL8125BP" },
    { 0x7cf, 0x708, VER_65, "RTL8125CP" },
    { 0x7cf, 0x68b, VER_64, "RTL9151A" },
    { 0x7cf, 0x68a, VER_64, "RTL8125K" },
    { 0x7cf, 0x689, VER_64, "RTL8125D" },
    { 0x7cf, 0x688, VER_64, "RTL8125D" },
    { 0x7cf, 0x641, VER_63, "RTL8125B" },
    { 0x7cf, 0x609, VER_61, "RTL8125A" },
    { 0x7cf, 0x54b, VER_52, "RTL8168fp/RTL8117" },
    { 0x7cf, 0x54a, VER_52, "RTL8168fp/RTL8117" },
    { 0x7cf, 0x502, VER_51, "RTL8168ep/8111ep" },
    { 0x7cf, 0x541, VER_46, "RTL8168h/8111h" },
    { 0x7cf, 0x6c0, VER_46, "RTL8168M" },
    { 0x7cf, 0x5c8, VER_44, "RTL8411b" },
    { 0x7cf, 0x509, VER_42, "RTL8168gu/8111gu" },
    { 0x7cf, 0x4c0, VER_40, "RTL8168g/8111g" },
    { 0x7cf, 0x7c8, VER_EXTENDED, NULL },
    { 0x000, 0x000, VER_OLD, NULL },          /* older chips: Linux knows them, we don't yet */
};

struct rtl_desc {
    uint32_t opts1;
    uint32_t opts2;
    uint64_t addr;
};

struct rtl {
    struct net_device dev;
    struct pci_device pci;
    volatile uint8_t *mmio;
    enum mac_version ver;
    bool gmii;
    uint32_t ocp_base;
    uint16_t cp_cmd;
    uint32_t irq_mask;
    volatile struct rtl_desc *rx, *tx;
    uint64_t rx_phys, tx_phys;
    uint8_t *rx_bufs, *tx_bufs;
    uint64_t rx_bufs_phys, tx_bufs_phys;
    uint32_t cur_rx, cur_tx, dirty_tx;
};

#define MAX_CARDS 4
static struct rtl cards[MAX_CARDS];
static int card_count;

static inline uint8_t R8(struct rtl *tp, uint32_t r) { return *(volatile uint8_t *)(tp->mmio + r); }
static inline uint16_t R16(struct rtl *tp, uint32_t r) { return *(volatile uint16_t *)(tp->mmio + r); }
static inline uint32_t R32(struct rtl *tp, uint32_t r) { return *(volatile uint32_t *)(tp->mmio + r); }
static inline void W8(struct rtl *tp, uint32_t r, uint8_t v) { *(volatile uint8_t *)(tp->mmio + r) = v; }
static inline void W16(struct rtl *tp, uint32_t r, uint16_t v) { *(volatile uint16_t *)(tp->mmio + r) = v; }
static inline void W32(struct rtl *tp, uint32_t r, uint32_t v) { *(volatile uint32_t *)(tp->mmio + r) = v; }

static bool is_8125(struct rtl *tp) { return tp->ver >= VER_61 && tp->ver <= VER_70; }

static void pci_commit(struct rtl *tp) { (void)R8(tp, ChipCmd); }
static void unlock_config(struct rtl *tp) { W8(tp, Cfg9346, Cfg9346_Unlock); }
static void lock_config(struct rtl *tp) { W8(tp, Cfg9346, Cfg9346_Lock); }

/* rtl_loop_wait: poll `cond` until it's `high`, `n` times `us` apart. */
static bool loop_wait(struct rtl *tp, bool (*cond)(struct rtl *), uint32_t us, int n, bool high) {
    for (int i = 0; i < n; i++) {
        if (cond(tp) == high) {
            return true;
        }
        if (us >= 1000) {
            sched_sleep_ms(us / 1000);
        } else {
            udelay(us);
        }
    }
    return false;
}

/* --- ERI, EPHY, CSI and OCP: the chip's indirect register windows --------------------------------- */

static bool eriar_cond(struct rtl *tp) { return R32(tp, ERIAR) & ERIAR_FLAG; }

static void eri_write(struct rtl *tp, uint32_t addr, uint32_t mask, uint32_t val) {
    W32(tp, ERIDR, val);
    W32(tp, ERIAR, ERIAR_WRITE_CMD | ERIAR_EXGMAC | mask | addr);
    loop_wait(tp, eriar_cond, 100, 100, false);
}

static uint32_t eri_read_type(struct rtl *tp, uint32_t addr, uint32_t type) {
    uint32_t cmd = type | ERIAR_MASK_1111 | addr;
    if (type == ERIAR_OOB && tp->ver == VER_52) {
        cmd |= 0xf70u << 18;   /* r8168fp_adjust_ocp_cmd */
    }
    W32(tp, ERIAR, cmd);
    return loop_wait(tp, eriar_cond, 100, 100, true) ? R32(tp, ERIDR) : ~0u;
}

static uint32_t eri_read(struct rtl *tp, uint32_t addr) {
    return eri_read_type(tp, addr, ERIAR_EXGMAC);
}

static void eri_w0w1(struct rtl *tp, uint32_t addr, uint32_t set, uint32_t clear) {
    uint32_t v = eri_read(tp, addr);
    eri_write(tp, addr, ERIAR_MASK_1111, (v & ~clear) | set);
}

static void eri_set_bits(struct rtl *tp, uint32_t addr, uint32_t bits) { eri_w0w1(tp, addr, bits, 0); }
static void eri_clear_bits(struct rtl *tp, uint32_t addr, uint32_t bits) { eri_w0w1(tp, addr, 0, bits); }

static bool ephyar_cond(struct rtl *tp) { return R32(tp, EPHYAR) & EPHYAR_FLAG; }

static void ephy_write(struct rtl *tp, uint32_t reg, uint16_t value) {
    W32(tp, EPHYAR, EPHYAR_WRITE_CMD | value | (reg & 0x1f) << 16);
    loop_wait(tp, ephyar_cond, 10, 100, false);
    udelay(10);
}

static uint16_t ephy_read(struct rtl *tp, uint32_t reg) {
    W32(tp, EPHYAR, (reg & 0x1f) << 16);
    return loop_wait(tp, ephyar_cond, 10, 100, true) ? (uint16_t)R32(tp, EPHYAR) : 0xffff;
}

struct ephy_info {
    uint8_t offset;
    uint16_t mask, bits;
};

static void ephy_init(struct rtl *tp, const struct ephy_info *e, int n) {
    for (; n-- > 0; e++) {
        ephy_write(tp, e->offset, (uint16_t)((ephy_read(tp, e->offset) & ~e->mask) | e->bits));
    }
}
#define EPHY_INIT(tp, a) ephy_init(tp, a, (int)(sizeof(a) / sizeof(a[0])))

static bool csiar_cond(struct rtl *tp) { return R32(tp, CSIAR) & CSIAR_FLAG; }

/* rtl_csi_mod: PCI extended config space through the chip (we only have the first 256 bytes). */
static void csi_mod(struct rtl *tp, uint32_t addr, uint32_t mask, uint32_t set) {
    uint32_t func = (uint32_t)tp->pci.function << 16;
    W32(tp, CSIAR, (addr & 0xfff) | func | CSIAR_BYTE_ENABLE);
    uint32_t v = loop_wait(tp, csiar_cond, 10, 100, true) ? R32(tp, CSIDR) : ~0u;
    W32(tp, CSIDR, (v & ~mask) | set);
    W32(tp, CSIAR, CSIAR_WRITE_CMD | (addr & 0xfff) | CSIAR_BYTE_ENABLE | func);
    loop_wait(tp, csiar_cond, 10, 100, false);
}

static bool ocp_gphy_cond(struct rtl *tp) { return R32(tp, GPHY_OCP) & OCPAR_FLAG; }

static void phy_ocp_write(struct rtl *tp, uint32_t reg, uint16_t data) {
    W32(tp, GPHY_OCP, OCPAR_FLAG | reg << 15 | data);
    loop_wait(tp, ocp_gphy_cond, 25, 10, false);
}

static uint16_t phy_ocp_read(struct rtl *tp, uint32_t reg) {
    W32(tp, GPHY_OCP, reg << 15);
    return loop_wait(tp, ocp_gphy_cond, 25, 10, true) ? (uint16_t)R32(tp, GPHY_OCP) : 0xffff;
}

static void mac_ocp_write(struct rtl *tp, uint32_t reg, uint16_t data) {
    uint64_t irq = irq_save();
    W32(tp, OCPDR, OCPAR_FLAG | reg << 15 | data);
    irq_restore(irq);
}

static uint16_t mac_ocp_read(struct rtl *tp, uint32_t reg) {
    uint64_t irq = irq_save();
    W32(tp, OCPDR, reg << 15);
    uint16_t v = (uint16_t)R32(tp, OCPDR);
    irq_restore(irq);
    return v;
}

static void mac_ocp_modify(struct rtl *tp, uint32_t reg, uint16_t mask, uint16_t set) {
    uint64_t irq = irq_save();
    W32(tp, OCPDR, reg << 15);
    uint16_t v = (uint16_t)R32(tp, OCPDR);
    W32(tp, OCPDR, OCPAR_FLAG | reg << 15 | (uint16_t)((v & ~mask) | set));
    irq_restore(irq);
}

/* --- the PHY: MII registers mapped onto OCP (r8168g_mdio_read/write), pages through reg 0x1f --- */

static void rtl8168g_phy_suspend_quirk(struct rtl *tp, uint16_t value) {
    if (tp->ver == VER_40) {
        if (value & BMCR_RESET || !(value & BMCR_PDOWN)) {
            eri_set_bits(tp, 0x1a8, 0xfc000000);
        } else {
            eri_clear_bits(tp, 0x1a8, 0xfc000000);
        }
    }
}

static void phy_write(struct rtl *tp, uint32_t reg, uint16_t value) {
    if (reg == 0x1f) {
        tp->ocp_base = value ? (uint32_t)value << 4 : OCP_STD_PHY_BASE;
        return;
    }
    if (tp->ocp_base != OCP_STD_PHY_BASE) {
        reg -= 0x10;
    }
    if (tp->ocp_base == OCP_STD_PHY_BASE && reg == MII_BMCR) {
        rtl8168g_phy_suspend_quirk(tp, value);
    }
    phy_ocp_write(tp, tp->ocp_base + reg * 2, value);
}

static uint16_t phy_read(struct rtl *tp, uint32_t reg) {
    if (reg == 0x1f) {
        return tp->ocp_base == OCP_STD_PHY_BASE ? 0 : (uint16_t)(tp->ocp_base >> 4);
    }
    if (tp->ocp_base != OCP_STD_PHY_BASE) {
        reg -= 0x10;
    }
    return phy_ocp_read(tp, tp->ocp_base + reg * 2);
}

static uint16_t phy_read_paged(struct rtl *tp, uint16_t page, uint32_t reg) {
    phy_write(tp, 0x1f, page);
    uint16_t v = phy_read(tp, reg);
    phy_write(tp, 0x1f, 0);
    return v;
}

static void phy_write_paged(struct rtl *tp, uint16_t page, uint32_t reg, uint16_t value) {
    phy_write(tp, 0x1f, page);
    phy_write(tp, reg, value);
    phy_write(tp, 0x1f, 0);
}

static void phy_modify_paged(struct rtl *tp, uint16_t page, uint32_t reg, uint16_t mask, uint16_t set) {
    phy_write(tp, 0x1f, page);
    phy_write(tp, reg, (uint16_t)((phy_read(tp, reg) & ~mask) | set));
    phy_write(tp, 0x1f, 0);
}

static void r8168g_phy_param(struct rtl *tp, uint16_t parm, uint16_t mask, uint16_t val) {
    phy_write(tp, 0x1f, 0x0a43);
    phy_write(tp, 0x13, parm);
    phy_write(tp, 0x14, (uint16_t)((phy_read(tp, 0x14) & ~mask) | val));
    phy_write(tp, 0x1f, 0);
}

/* (MDIO_MMD_VEND2 registers are OCP addresses) */
static void rtl8125_phy_param(struct rtl *tp, uint16_t parm, uint16_t mask, uint16_t val) {
    phy_ocp_write(tp, 0xb87c, parm);
    phy_ocp_write(tp, 0xb87e, (uint16_t)((phy_ocp_read(tp, 0xb87e) & ~mask) | val));
}

/* --- PHY tuning per chip (r8169_phy_config.c) ----------------------------------------------------- */

static void rtl8168g_config_eee_phy(struct rtl *tp) {
    phy_modify_paged(tp, 0x0a43, 0x11, 0, 1u << 4);
}

static void rtl8168h_config_eee_phy(struct rtl *tp) {
    rtl8168g_config_eee_phy(tp);
    phy_modify_paged(tp, 0xa4a, 0x11, 0x0000, 0x0200);
    phy_modify_paged(tp, 0xa42, 0x14, 0x0000, 0x0080);
}

static void rtl8125_common_config_eee_phy(struct rtl *tp) {
    phy_modify_paged(tp, 0xa6d, 0x14, 0x0010, 0x0000);
    phy_modify_paged(tp, 0xa42, 0x14, 0x0080, 0x0000);
    phy_modify_paged(tp, 0xa4a, 0x11, 0x0200, 0x0000);
}

static void rtl8125_config_eee_phy(struct rtl *tp) {
    rtl8168g_config_eee_phy(tp);
    rtl8125_common_config_eee_phy(tp);
}

static void rtl8168g_disable_aldps(struct rtl *tp) {
    phy_modify_paged(tp, 0x0a43, 0x10, 1u << 2, 0);
}

static void rtl8168g_enable_gphy_10m(struct rtl *tp) {
    phy_modify_paged(tp, 0x0a44, 0x11, 0, 1u << 11);
}

static void rtl8168g_phy_adjust_10m_aldps(struct rtl *tp) {
    phy_modify_paged(tp, 0x0bcc, 0x14, 1u << 8, 0);
    phy_modify_paged(tp, 0x0a44, 0x11, 0, 1u << 7 | 1u << 6);
    r8168g_phy_param(tp, 0x8084, 0x6000, 0x0000);
    phy_modify_paged(tp, 0x0a43, 0x10, 0x0000, 0x1003);
}

static void improve_swr(struct rtl *tp, uint32_t reg, uint16_t value) {
    phy_write(tp, 0x1f, 0x0bcd);
    phy_write(tp, 0x14, 0x5065);
    phy_write(tp, 0x14, 0xd065);
    phy_write(tp, 0x1f, 0x0bc8);
    phy_write(tp, reg, value);
    phy_write(tp, 0x1f, 0x0bcd);
    phy_write(tp, 0x14, 0x1065);
    phy_write(tp, 0x14, 0x9065);
    phy_write(tp, 0x14, 0x1065);
    phy_write(tp, 0x1f, 0x0000);
}

static void rtl8168g_1_hw_phy_config(struct rtl *tp) {
    if (phy_read_paged(tp, 0x0a46, 0x10) & (1u << 8)) {
        phy_modify_paged(tp, 0x0bcc, 0x12, 1u << 15, 0);
    } else {
        phy_modify_paged(tp, 0x0bcc, 0x12, 0, 1u << 15);
    }
    if (phy_read_paged(tp, 0x0a46, 0x13) & (1u << 8)) {
        phy_modify_paged(tp, 0x0c41, 0x15, 0, 1u << 1);
    } else {
        phy_modify_paged(tp, 0x0c41, 0x15, 1u << 1, 0);
    }
    phy_modify_paged(tp, 0x0a44, 0x11, 0, 1u << 3 | 1u << 2);   /* PHY auto speed down */
    rtl8168g_phy_adjust_10m_aldps(tp);
    phy_modify_paged(tp, 0x0a4b, 0x11, 0, 1u << 2);             /* EEE auto-fallback */
    r8168g_phy_param(tp, 0x8012, 0x0000, 0x8000);               /* UC LPF tune */
    phy_modify_paged(tp, 0x0c42, 0x11, 1u << 13, 1u << 14);
    improve_swr(tp, 0x11, 0x5655);
    rtl8168g_disable_aldps(tp);
    rtl8168g_config_eee_phy(tp);
}

static void rtl8168g_2_hw_phy_config(struct rtl *tp) {
    rtl8168g_config_eee_phy(tp);
}

/* rtl8168h_2_get_adc_bias_ioffset */
static uint16_t adc_bias_ioffset(struct rtl *tp) {
    mac_ocp_write(tp, 0xdd02, 0x807d);
    uint16_t data1 = mac_ocp_read(tp, 0xdd02), data2 = mac_ocp_read(tp, 0xdd00);
    uint16_t ioffset = (uint16_t)(((data2 >> 1) & 0x7ff8) | (data2 & 0x0007));
    if (data1 & (1u << 7)) {
        ioffset |= 1u << 15;
    }
    return ioffset;
}

static void rtl8168h_2_hw_phy_config(struct rtl *tp) {
    r8168g_phy_param(tp, 0x808a, 0x003f, 0x000a);   /* CHIN EST */
    r8168g_phy_param(tp, 0x0811, 0x0000, 0x0800);   /* R-tune & PGA-retune */
    phy_modify_paged(tp, 0x0a42, 0x16, 0x0000, 0x0002);
    rtl8168g_enable_gphy_10m(tp);
    uint16_t ioffset = adc_bias_ioffset(tp);
    if (ioffset != 0xffff) {
        phy_write_paged(tp, 0x0bcf, 0x16, ioffset);
    }
    uint16_t data = phy_read_paged(tp, 0x0bcd, 0x16) & 0x000f, rlen = data > 3 ? (uint16_t)(data - 3) : 0;
    phy_write_paged(tp, 0x0bcd, 0x17, (uint16_t)(rlen | rlen << 4 | rlen << 8 | rlen << 12));
    phy_modify_paged(tp, 0x0a44, 0x11, 1u << 7, 0);   /* no PHY PFM mode */
    phy_modify_paged(tp, 0x0a43, 0x10, 1u << 0, 0);   /* no 10M PLL off */
    rtl8168g_disable_aldps(tp);
    rtl8168g_config_eee_phy(tp);
}

static void rtl8168ep_2_hw_phy_config(struct rtl *tp) {
    static const uint16_t params[][3] = {
        { 0x80f3, 0xff00, 0x8b00 }, { 0x80f0, 0xff00, 0x3a00 }, { 0x80ef, 0xff00, 0x0500 },
        { 0x80f6, 0xff00, 0x6e00 }, { 0x80ec, 0xff00, 0x6800 }, { 0x80ed, 0xff00, 0x7c00 },
        { 0x80f2, 0xff00, 0xf400 }, { 0x80f4, 0xff00, 0x8500 }, { 0x8110, 0xff00, 0xa800 },
        { 0x810f, 0xff00, 0x1d00 }, { 0x8111, 0xff00, 0xf500 }, { 0x8113, 0xff00, 0x6100 },
        { 0x8115, 0xff00, 0x9200 }, { 0x810e, 0xff00, 0x0400 }, { 0x810c, 0xff00, 0x7c00 },
        { 0x810b, 0xff00, 0x5a00 }, { 0x80d1, 0xff00, 0xff00 }, { 0x80cd, 0xff00, 0x9e00 },
        { 0x80d3, 0xff00, 0x0e00 }, { 0x80d5, 0xff00, 0xca00 }, { 0x80d7, 0xff00, 0x8400 },
    };
    rtl8168g_phy_adjust_10m_aldps(tp);
    r8168g_phy_param(tp, 0x8012, 0x0000, 0x8000);
    phy_modify_paged(tp, 0x0c42, 0x11, 1u << 13, 1u << 14);
    for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
        r8168g_phy_param(tp, params[i][0], params[i][1], params[i][2]);
    }
    improve_swr(tp, 0x12, 0x00ed);   /* force PWM mode */
    rtl8168g_disable_aldps(tp);
    rtl8168g_config_eee_phy(tp);
}

static void rtl8117_hw_phy_config(struct rtl *tp) {
    static const uint16_t params[][3] = {
        { 0x808e, 0xff00, 0x4800 }, { 0x8090, 0xff00, 0xcc00 }, { 0x8092, 0xff00, 0xb000 },
        { 0x8088, 0xff00, 0x6000 }, { 0x808b, 0x3f00, 0x0b00 }, { 0x808d, 0x1f00, 0x0600 },
        { 0x808c, 0xff00, 0xb000 }, { 0x80a0, 0xff00, 0x2800 }, { 0x80a2, 0xff00, 0x5000 },
        { 0x809b, 0xf800, 0xb000 }, { 0x809a, 0xff00, 0x4b00 }, { 0x809d, 0x3f00, 0x0800 },
        { 0x80a1, 0xff00, 0x7000 }, { 0x809f, 0x1f00, 0x0300 }, { 0x809e, 0xff00, 0x8800 },
        { 0x80b2, 0xff00, 0x2200 }, { 0x80ad, 0xf800, 0x9800 }, { 0x80af, 0x3f00, 0x0800 },
        { 0x80b3, 0xff00, 0x6f00 }, { 0x80b1, 0x1f00, 0x0300 }, { 0x80b0, 0xff00, 0x9300 },
    };
    for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
        r8168g_phy_param(tp, params[i][0], params[i][1], params[i][2]);
    }
    r8168g_phy_param(tp, 0x8011, 0x0000, 0x0800);
    rtl8168g_enable_gphy_10m(tp);
    r8168g_phy_param(tp, 0x8016, 0x0000, 0x0400);
    rtl8168g_disable_aldps(tp);
    rtl8168h_config_eee_phy(tp);
}

static void rtl8125_legacy_force_mode(struct rtl *tp) {
    phy_modify_paged(tp, 0xa5b, 0x12, 1u << 15, 0);
}

static void rtl8125a_2_hw_phy_config(struct rtl *tp) {
    static const uint16_t curve[] = { 0x0043, 0x00A7, 0x00D6, 0x00EC, 0x00F6, 0x00FB, 0x00FD, 0x00FF,
                                      0x00BB, 0x0058, 0x0029, 0x0013, 0x0009, 0x0004, 0x0002 };
    phy_modify_paged(tp, 0xad4, 0x17, 0x0000, 0x0010);
    phy_modify_paged(tp, 0xad1, 0x13, 0x03ff, 0x03ff);
    phy_modify_paged(tp, 0xad3, 0x11, 0x003f, 0x0006);
    phy_modify_paged(tp, 0xac0, 0x14, 0x1100, 0x0000);
    phy_modify_paged(tp, 0xacc, 0x10, 0x0003, 0x0002);
    phy_modify_paged(tp, 0xad4, 0x10, 0x00e7, 0x0044);
    phy_modify_paged(tp, 0xac1, 0x12, 0x0080, 0x0000);
    phy_modify_paged(tp, 0xac8, 0x10, 0x0300, 0x0000);
    phy_modify_paged(tp, 0xac5, 0x17, 0x0007, 0x0002);
    phy_write_paged(tp, 0xad4, 0x16, 0x00a8);
    phy_write_paged(tp, 0xac5, 0x16, 0x01ff);
    phy_modify_paged(tp, 0xac8, 0x15, 0x00f0, 0x0030);
    rtl8125_phy_param(tp, 0x80a2, 0xffff, 0x0153);
    rtl8125_phy_param(tp, 0x809c, 0xffff, 0x0153);
    phy_write(tp, 0x1f, 0x0a43);
    phy_write(tp, 0x13, 0x81B3);
    for (size_t i = 0; i < sizeof(curve) / sizeof(curve[0]); i++) {
        phy_write(tp, 0x14, curve[i]);
    }
    for (int i = 0; i < 25; i++) {
        phy_write(tp, 0x14, 0x0000);
    }
    phy_write(tp, 0x1f, 0x0000);
    r8168g_phy_param(tp, 0x8257, 0xffff, 0x020F);
    r8168g_phy_param(tp, 0x80ea, 0xffff, 0x7843);
    phy_modify_paged(tp, 0xd06, 0x14, 0x0000, 0x2000);
    r8168g_phy_param(tp, 0x81a2, 0x0000, 0x0100);
    phy_modify_paged(tp, 0xb54, 0x16, 0xff00, 0xdb00);
    phy_modify_paged(tp, 0xa45, 0x12, 0x0001, 0x0000);
    phy_modify_paged(tp, 0xa5d, 0x12, 0x0000, 0x0020);
    phy_modify_paged(tp, 0xad4, 0x17, 0x0010, 0x0000);
    phy_modify_paged(tp, 0xa86, 0x15, 0x0001, 0x0000);
    rtl8168g_enable_gphy_10m(tp);
    rtl8168g_disable_aldps(tp);
    rtl8125_config_eee_phy(tp);
}

static void rtl8125b_hw_phy_config(struct rtl *tp) {
    rtl8168g_enable_gphy_10m(tp);
    phy_modify_paged(tp, 0xac4, 0x13, 0x00f0, 0x0090);
    phy_modify_paged(tp, 0xad3, 0x10, 0x0003, 0x0001);
    rtl8125_phy_param(tp, 0x80f5, 0xffff, 0x760e);
    rtl8125_phy_param(tp, 0x8107, 0xffff, 0x360e);
    rtl8125_phy_param(tp, 0x8551, 0xff00, 0x0800);
    phy_modify_paged(tp, 0xbf0, 0x10, 0xe000, 0xa000);
    phy_modify_paged(tp, 0xbf4, 0x13, 0x0f00, 0x0300);
    for (uint16_t p = 0x8044; p <= 0x807a; p += 6) {
        r8168g_phy_param(tp, p, 0xffff, 0x2417);
    }
    phy_modify_paged(tp, 0xa4c, 0x15, 0x0000, 0x0040);
    phy_modify_paged(tp, 0xbf8, 0x12, 0xe000, 0xa000);
    rtl8125_legacy_force_mode(tp);
    rtl8168g_disable_aldps(tp);
    rtl8125_config_eee_phy(tp);
}

static void rtl8125d_hw_phy_config(struct rtl *tp) {
    rtl8168g_enable_gphy_10m(tp);
    rtl8125_legacy_force_mode(tp);
    rtl8168g_disable_aldps(tp);
    rtl8125_config_eee_phy(tp);
}

static void rtl8125cp_hw_phy_config(struct rtl *tp) {
    rtl8168g_enable_gphy_10m(tp);
    phy_modify_paged(tp, 0xad0, 0x17, 0x007f, 0x000b);
    phy_modify_paged(tp, 0xad7, 0x14, 0x0000, 1u << 4);
    rtl8125_phy_param(tp, 0x807f, 0xff00, 0x5300);
    r8168g_phy_param(tp, 0x81b8, 0xffff, 0x00b4);
    r8168g_phy_param(tp, 0x81ba, 0xffff, 0x00e4);
    r8168g_phy_param(tp, 0x81c5, 0xffff, 0x0104);
    r8168g_phy_param(tp, 0x81d0, 0xffff, 0x054d);
    phy_modify_paged(tp, 0x0a43, 0x10, 0x0000, 0x0003);   /* LINK_SPEED_10M_PLL_OFF | ALDPS_PLL_OFF */
    phy_modify_paged(tp, 0x0a44, 0x11, 0x0000, 1u << 7);
    rtl8125_legacy_force_mode(tp);
    rtl8168g_disable_aldps(tp);
    rtl8125_config_eee_phy(tp);
}

static void rtl8125bp_hw_phy_config(struct rtl *tp) {
    rtl8168g_enable_gphy_10m(tp);
    r8168g_phy_param(tp, 0x8010, 0x0800, 0x0000);
    rtl8125_phy_param(tp, 0x8088, 0xff00, 0x9000);
    rtl8125_phy_param(tp, 0x808f, 0xff00, 0x9000);
    r8168g_phy_param(tp, 0x8174, 0x2000, 0x1800);
    rtl8125_legacy_force_mode(tp);
    rtl8168g_disable_aldps(tp);
    rtl8125_config_eee_phy(tp);
}

static void rtl8126a_hw_phy_config(struct rtl *tp) {
    rtl8168g_enable_gphy_10m(tp);
    rtl8125_legacy_force_mode(tp);
    rtl8168g_disable_aldps(tp);
    rtl8125_common_config_eee_phy(tp);
}

static void hw_phy_config(struct rtl *tp) {
    switch (tp->ver) {
    case VER_40: rtl8168g_1_hw_phy_config(tp); break;
    case VER_42:
    case VER_43:
    case VER_44: rtl8168g_2_hw_phy_config(tp); break;
    case VER_46:
    case VER_48: rtl8168h_2_hw_phy_config(tp); break;
    case VER_51: rtl8168ep_2_hw_phy_config(tp); break;
    case VER_52: rtl8117_hw_phy_config(tp); break;
    case VER_61: rtl8125a_2_hw_phy_config(tp); break;
    case VER_63: rtl8125b_hw_phy_config(tp); break;
    case VER_64: rtl8125d_hw_phy_config(tp); break;
    case VER_65: rtl8125cp_hw_phy_config(tp); break;
    case VER_66: rtl8125bp_hw_phy_config(tp); break;
    case VER_70: rtl8126a_hw_phy_config(tp); break;
    default: break;
    }
}

/* rtl8169_init_phy + phy_start: the tuning, a soft reset, and auto-negotiation for every speed. */
static void init_phy(struct rtl *tp) {
    hw_phy_config(tp);
    phy_write(tp, 0x1f, 0);
    phy_write(tp, MII_BMCR, (uint16_t)(phy_read(tp, MII_BMCR) | BMCR_RESET));   /* genphy_soft_reset */
    for (int i = 0; i < 100 && (phy_read(tp, MII_BMCR) & BMCR_RESET); i++) {
        sched_sleep_ms(1);
    }
    phy_write(tp, MII_ADVERTISE, (uint16_t)((phy_read(tp, MII_ADVERTISE) & ~0x0DE0) | 0x01E0 | 0x0C00));
    if (tp->gmii) {
        phy_write(tp, MII_CTRL1000, (uint16_t)((phy_read(tp, MII_CTRL1000) & ~0x0300) | 0x0200));
    }
    if (is_8125(tp)) {
        phy_modify_paged(tp, 0xa5d, 0x12, 0, RTL_ADV_2500FULL);
    }
    phy_write(tp, MII_BMCR, (uint16_t)(phy_read(tp, MII_BMCR) | BMCR_ANENABLE | BMCR_ANRESTART));
}

/* --- MAC bring-up (rtl_hw_initialize, rtl_hw_start...) --------------------------------------------- */

static bool link_list_ready(struct rtl *tp) { return R8(tp, MCU) & LINK_LIST_RDY; }
static bool txcfg_empty(struct rtl *tp) { return R32(tp, TxConfig) & TXCFG_EMPTY; }
static bool rxtx_empty(struct rtl *tp) { return (R8(tp, MCU) & RXTX_EMPTY) == RXTX_EMPTY; }
static bool rxtx_empty_2(struct rtl *tp) { return (R16(tp, IntrMitigate) & 0x0103) == 0x0103; }
static bool chipcmd_reset(struct rtl *tp) { return R8(tp, ChipCmd) & CmdReset; }
static bool mac_ocp_e00e(struct rtl *tp) { return mac_ocp_read(tp, 0xe00e) & (1u << 13); }
static bool ocp_tx_cond(struct rtl *tp) { return R8(tp, 0xfb) & 0x20; }   /* IBISR0 */

static void wait_txrx_fifo_empty(struct rtl *tp) {
    if (tp->ver <= VER_52) {
        loop_wait(tp, txcfg_empty, 100, 42, true);
        loop_wait(tp, rxtx_empty, 100, 42, true);
    } else if (tp->ver == VER_61) {
        loop_wait(tp, rxtx_empty, 100, 42, true);
    } else {
        W8(tp, ChipCmd, R8(tp, ChipCmd) | StopReq);
        loop_wait(tp, rxtx_empty, 100, 42, true);
        loop_wait(tp, rxtx_empty_2, 100, 42, true);
    }
}

static void enable_rxdvgate(struct rtl *tp) {
    W32(tp, MISC, R32(tp, MISC) | RXDV_GATED_EN);
    sched_sleep_ms(2);
    wait_txrx_fifo_empty(tp);
}

static void disable_rxdvgate(struct rtl *tp) {
    W32(tp, MISC, R32(tp, MISC) & ~RXDV_GATED_EN);
}

/* rtl8168ep_stop_cmac: the management controller lets go of the MAC. */
static void stop_cmac(struct rtl *tp) {
    W8(tp, 0xf9, R8(tp, 0xf9) & ~0x01);   /* IBCR2 */
    loop_wait(tp, ocp_tx_cond, 50000, 40, true);
    W8(tp, 0xfb, R8(tp, 0xfb) | 0x20);    /* IBISR0 */
    W8(tp, 0xf8, R8(tp, 0xf8) & ~0x01);   /* IBCR0 */
}

static void hw_initialize(struct rtl *tp) {
    if (tp->ver == VER_51 || tp->ver == VER_52) {
        stop_cmac(tp);
    }
    enable_rxdvgate(tp);
    W8(tp, ChipCmd, R8(tp, ChipCmd) & ~(CmdTxEnb | CmdRxEnb));
    sched_sleep_ms(1);
    W8(tp, MCU, R8(tp, MCU) & ~NOW_IS_OOB);
    mac_ocp_modify(tp, 0xe8de, 1u << 14, 0);
    loop_wait(tp, link_list_ready, 100, 42, true);
    if (is_8125(tp)) {
        mac_ocp_write(tp, 0xc0aa, 0x07d0);
        mac_ocp_write(tp, 0xc0a6, 0x0150);
        mac_ocp_write(tp, 0xc01e, 0x5555);
    } else {
        mac_ocp_modify(tp, 0xe8de, 0, 1u << 15);
    }
    loop_wait(tp, link_list_ready, 100, 42, true);
}

static void hw_reset(struct rtl *tp) {
    W8(tp, ChipCmd, CmdReset);
    loop_wait(tp, chipcmd_reset, 100, 100, false);
}

static void set_def_aspm_entry_latency(struct rtl *tp) {
    csi_mod(tp, 0x070c, 0xff000000, 0x27u << 24);   /* L0 7us, L1 16us */
}

static void set_fifo_size(struct rtl *tp, uint16_t rx_stat, uint16_t tx_stat, uint16_t rx_dyn, uint16_t tx_dyn) {
    eri_write(tp, 0xc8, ERIAR_MASK_1111, (uint32_t)rx_stat << 16 | rx_dyn);
    eri_write(tp, 0xe8, ERIAR_MASK_1111, (uint32_t)tx_stat << 16 | tx_dyn);
}

static void set_pause_thresholds(struct rtl *tp, uint8_t low, uint8_t high) {
    eri_write(tp, 0xcc, ERIAR_MASK_0001, low);
    eri_write(tp, 0xd0, ERIAR_MASK_0001, high);
}

static void reset_packet_filter(struct rtl *tp) {
    eri_clear_bits(tp, 0xdc, 1u << 0);
    eri_set_bits(tp, 0xdc, 1u << 0);
}

static void pcie_state_l2l3_disable(struct rtl *tp) {
    W8(tp, Config3, R8(tp, Config3) & ~Rdy_to_L23);
}

static void rtl_hw_start_8168g(struct rtl *tp) {
    set_fifo_size(tp, 0x08, 0x10, 0x02, 0x06);
    set_pause_thresholds(tp, 0x38, 0x48);
    set_def_aspm_entry_latency(tp);
    reset_packet_filter(tp);
    eri_write(tp, 0x2f8, ERIAR_MASK_0011, 0x1d8f);
    disable_rxdvgate(tp);
    eri_write(tp, 0xc0, ERIAR_MASK_0011, 0x0000);
    eri_write(tp, 0xb8, ERIAR_MASK_0011, 0x0000);
    eri_w0w1(tp, 0x2fc, 0x01, 0x06);
    eri_clear_bits(tp, 0x1b0, 1u << 12);
    pcie_state_l2l3_disable(tp);
}

static void rtl_hw_start_8168g_1(struct rtl *tp) {
    static const struct ephy_info e[] = { { 0x00, 0x0008, 0x0000 }, { 0x0c, 0x3ff0, 0x0820 },
                                          { 0x1e, 0x0000, 0x0001 }, { 0x19, 0x8000, 0x0000 } };
    rtl_hw_start_8168g(tp);
    EPHY_INIT(tp, e);
}

static void rtl_hw_start_8168g_2(struct rtl *tp) {
    static const struct ephy_info e[] = {
        { 0x00, 0x0008, 0x0000 }, { 0x0c, 0x3ff0, 0x0820 }, { 0x19, 0xffff, 0x7c00 },
        { 0x1e, 0xffff, 0x20eb }, { 0x0d, 0xffff, 0x1666 }, { 0x00, 0xffff, 0x10a3 },
        { 0x06, 0xffff, 0xf050 }, { 0x04, 0x0000, 0x0010 }, { 0x1d, 0x4000, 0x0000 },
    };
    rtl_hw_start_8168g(tp);
    EPHY_INIT(tp, e);
}

static void rtl8411b_fix_phy_down(struct rtl *tp) {
    static const uint16_t fix_data[] = {
        0xe008, 0xe00a, 0xe00c, 0xe00e, 0xe027, 0xe04f, 0xe05e, 0xe065, 0xc602, 0xbe00, 0x0000, 0xc502,
        0xbd00, 0x074c, 0xc302, 0xbb00, 0x080a, 0x6420, 0x48c2, 0x8c20, 0xc516, 0x64a4, 0x49c0, 0xf009,
        0x74a2, 0x8ca5, 0x74a0, 0xc50e, 0x9ca2, 0x1c11, 0x9ca0, 0xe006, 0x74f8, 0x48c4, 0x8cf8, 0xc404,
        0xbc00, 0xc403, 0xbc00, 0x0bf2, 0x0c0a, 0xe434, 0xd3c0, 0x49d9, 0xf01f, 0xc526, 0x64a5, 0x1400,
        0xf007, 0x0c01, 0x8ca5, 0x1c15, 0xc51b, 0x9ca0, 0xe013, 0xc519, 0x74a0, 0x48c4, 0x8ca0, 0xc516,
        0x74a4, 0x48c8, 0x48ca, 0x9ca4, 0xc512, 0x1b00, 0x9ba0, 0x1b1c, 0x483f, 0x9ba2, 0x1b04, 0xc508,
        0x9ba0, 0xc505, 0xbd00, 0xc502, 0xbd00, 0x0300, 0x051e, 0xe434, 0xe018, 0xe092, 0xde20, 0xd3c0,
        0xc50f, 0x76a4, 0x49e3, 0xf007, 0x49c0, 0xf103, 0xc607, 0xbe00, 0xc606, 0xbe00, 0xc602, 0xbe00,
        0x0c4c, 0x0c28, 0x0c2c, 0xdc00, 0xc707, 0x1d00, 0x8de2, 0x48c1, 0xc502, 0xbd00, 0x00aa, 0xe0c0,
        0xc502, 0xbd00, 0x0132,
    };
    for (size_t i = 0; i < sizeof(fix_data) / sizeof(fix_data[0]); i++) {
        mac_ocp_write(tp, 0xf800 + 2 * (uint32_t)i, fix_data[i]);
    }
}

static void rtl_hw_start_8411_2(struct rtl *tp) {
    static const struct ephy_info e[] = {
        { 0x00, 0x0008, 0x0000 }, { 0x0c, 0x37d0, 0x0820 }, { 0x1e, 0x0000, 0x0001 }, { 0x19, 0x8021, 0x0000 },
        { 0x1e, 0x0000, 0x2000 }, { 0x0d, 0x0100, 0x0200 }, { 0x00, 0x0000, 0x0080 }, { 0x06, 0x0000, 0x0010 },
        { 0x04, 0x0000, 0x0010 }, { 0x1d, 0x0000, 0x4000 },
    };
    rtl_hw_start_8168g(tp);
    EPHY_INIT(tp, e);
    /* Realtek's fix for the receiver getting confused after the PHY was powered down. */
    for (uint32_t r = 0xFC28; r <= 0xFC36; r += 2) {
        mac_ocp_write(tp, r, 0x0000);
    }
    sched_sleep_ms(3);
    mac_ocp_write(tp, 0xFC26, 0x0000);
    rtl8411b_fix_phy_down(tp);
    mac_ocp_write(tp, 0xFC26, 0x8000);
    mac_ocp_write(tp, 0xFC2A, 0x0743);
    mac_ocp_write(tp, 0xFC2C, 0x0801);
    mac_ocp_write(tp, 0xFC2E, 0x0BE9);
    mac_ocp_write(tp, 0xFC30, 0x02FD);
    mac_ocp_write(tp, 0xFC32, 0x0C25);
    mac_ocp_write(tp, 0xFC34, 0x00A9);
    mac_ocp_write(tp, 0xFC36, 0x012D);
}

static void sw_cnt_1ms(struct rtl *tp) {
    int rg_saw_cnt = phy_read_paged(tp, 0x0c42, 0x13) & 0x3fff;
    if (rg_saw_cnt > 0) {
        mac_ocp_modify(tp, 0xd412, 0x0fff, (uint16_t)((16000000 / rg_saw_cnt) & 0x0fff));
    }
}

static void rtl_hw_start_8168h_1(struct rtl *tp) {
    static const struct ephy_info e[] = {
        { 0x1e, 0x0800, 0x0001 }, { 0x1d, 0x0000, 0x0800 }, { 0x05, 0xffff, 0x2089 },
        { 0x06, 0xffff, 0x5881 }, { 0x04, 0xffff, 0x854a }, { 0x01, 0xffff, 0x068b },
    };
    EPHY_INIT(tp, e);
    set_fifo_size(tp, 0x08, 0x10, 0x02, 0x06);
    set_pause_thresholds(tp, 0x38, 0x48);
    set_def_aspm_entry_latency(tp);
    reset_packet_filter(tp);
    eri_set_bits(tp, 0xdc, 0x001c);
    eri_write(tp, 0x5f0, ERIAR_MASK_0011, 0x4f87);
    disable_rxdvgate(tp);
    eri_write(tp, 0xc0, ERIAR_MASK_0011, 0x0000);
    eri_write(tp, 0xb8, ERIAR_MASK_0011, 0x0000);
    W8(tp, DLLPR, R8(tp, DLLPR) & ~PFM_EN);
    W8(tp, MISC_1, R8(tp, MISC_1) & ~PFM_D3COLD_EN);
    W8(tp, DLLPR, R8(tp, DLLPR) & ~TX_10M_PS_EN);
    eri_clear_bits(tp, 0x1b0, 1u << 12);
    pcie_state_l2l3_disable(tp);
    sw_cnt_1ms(tp);
    mac_ocp_modify(tp, 0xe056, 0x00f0, 0x0000);
    mac_ocp_modify(tp, 0xe052, 0x6000, 0x8008);
    mac_ocp_modify(tp, 0xe0d6, 0x01ff, 0x017f);
    mac_ocp_modify(tp, 0xd420, 0x0fff, 0x047f);
    mac_ocp_write(tp, 0xe63e, 0x0001);
    mac_ocp_write(tp, 0xe63e, 0x0000);
    mac_ocp_write(tp, 0xc094, 0x0000);
    mac_ocp_write(tp, 0xc09e, 0x0000);
}

static void rtl_hw_start_8168ep(struct rtl *tp) {
    stop_cmac(tp);
    set_fifo_size(tp, 0x08, 0x10, 0x02, 0x06);
    set_pause_thresholds(tp, 0x2f, 0x5f);
    set_def_aspm_entry_latency(tp);
    reset_packet_filter(tp);
    eri_write(tp, 0x5f0, ERIAR_MASK_0011, 0x4f87);
    disable_rxdvgate(tp);
    eri_write(tp, 0xc0, ERIAR_MASK_0011, 0x0000);
    eri_write(tp, 0xb8, ERIAR_MASK_0011, 0x0000);
    eri_w0w1(tp, 0x2fc, 0x01, 0x06);
    W8(tp, DLLPR, R8(tp, DLLPR) & ~TX_10M_PS_EN);
    pcie_state_l2l3_disable(tp);
}

static void rtl_hw_start_8168ep_3(struct rtl *tp) {
    static const struct ephy_info e[] = { { 0x00, 0x0000, 0x0080 }, { 0x0d, 0x0100, 0x0200 },
                                          { 0x19, 0x8021, 0x0000 }, { 0x1e, 0x0000, 0x2000 } };
    EPHY_INIT(tp, e);
    rtl_hw_start_8168ep(tp);
    W8(tp, DLLPR, R8(tp, DLLPR) & ~PFM_EN);
    W8(tp, MISC_1, R8(tp, MISC_1) & ~PFM_D3COLD_EN);
    mac_ocp_modify(tp, 0xd3e2, 0x0fff, 0x0271);
    mac_ocp_modify(tp, 0xd3e4, 0x00ff, 0x0000);
    mac_ocp_modify(tp, 0xe860, 0x0000, 0x0080);
}

static void rtl_hw_start_8117(struct rtl *tp) {
    static const struct ephy_info e[] = { { 0x19, 0x0040, 0x1100 }, { 0x59, 0x0040, 0x1100 } };
    stop_cmac(tp);
    EPHY_INIT(tp, e);
    set_fifo_size(tp, 0x08, 0x10, 0x02, 0x06);
    set_pause_thresholds(tp, 0x2f, 0x5f);
    set_def_aspm_entry_latency(tp);
    reset_packet_filter(tp);
    eri_set_bits(tp, 0xd4, 0x0010);
    eri_write(tp, 0x5f0, ERIAR_MASK_0011, 0x4f87);
    disable_rxdvgate(tp);
    eri_write(tp, 0xc0, ERIAR_MASK_0011, 0x0000);
    eri_write(tp, 0xb8, ERIAR_MASK_0011, 0x0000);
    W8(tp, DLLPR, R8(tp, DLLPR) & ~PFM_EN);
    W8(tp, MISC_1, R8(tp, MISC_1) & ~PFM_D3COLD_EN);
    W8(tp, DLLPR, R8(tp, DLLPR) & ~TX_10M_PS_EN);
    eri_clear_bits(tp, 0x1b0, 1u << 12);
    pcie_state_l2l3_disable(tp);
    sw_cnt_1ms(tp);
    mac_ocp_modify(tp, 0xe056, 0x00f0, 0x0000);
    mac_ocp_write(tp, 0xea80, 0x0003);
    mac_ocp_modify(tp, 0xe052, 0x0000, 0x0009);
    mac_ocp_modify(tp, 0xd420, 0x0fff, 0x047f);
    mac_ocp_write(tp, 0xe63e, 0x0001);
    mac_ocp_write(tp, 0xe63e, 0x0000);
    mac_ocp_write(tp, 0xc094, 0x0000);
    mac_ocp_write(tp, 0xc09e, 0x0000);
}

static void rtl_hw_start_8125_common(struct rtl *tp) {
    pcie_state_l2l3_disable(tp);
    W16(tp, 0x382, 0x221b);
    W32(tp, RSS_CTRL_8125, 0);
    W16(tp, Q_NUM_CTRL_8125, 0);
    mac_ocp_modify(tp, 0xd40a, 0x0010, 0x0000);   /* no UPS */
    W8(tp, Config1, R8(tp, Config1) & ~0x10);
    mac_ocp_write(tp, 0xc140, 0xffff);
    mac_ocp_write(tp, 0xc142, 0xffff);
    mac_ocp_modify(tp, 0xd3e2, 0x0fff, 0x03a9);
    mac_ocp_modify(tp, 0xd3e4, 0x00ff, 0x0000);
    mac_ocp_modify(tp, 0xe860, 0x0000, 0x0080);
    mac_ocp_modify(tp, 0xeb58, 0x0001, 0x0000);   /* the old transmit descriptor format */
    if (tp->ver == VER_70) {
        W8(tp, 0xD8, R8(tp, 0xD8) & ~0x02);
        mac_ocp_modify(tp, 0xe614, 0x0700, 0x0400);
    } else if (tp->ver == VER_63) {
        mac_ocp_modify(tp, 0xe614, 0x0700, 0x0200);
    } else {
        mac_ocp_modify(tp, 0xe614, 0x0700, 0x0300);
    }
    mac_ocp_modify(tp, 0xe63e, 0x0c30, tp->ver == VER_63 ? 0x0000 : 0x0020);
    mac_ocp_modify(tp, 0xc0b4, 0x0000, 0x000c);
    mac_ocp_modify(tp, 0xeb6a, 0x00ff, 0x0033);
    mac_ocp_modify(tp, 0xeb50, 0x03e0, 0x0040);
    mac_ocp_modify(tp, 0xe056, 0x00f0, 0x0000);
    mac_ocp_modify(tp, 0xe040, 0x1000, 0x0000);
    mac_ocp_modify(tp, 0xea1c, 0x0003, 0x0001);
    if (tp->ver == VER_70) {
        mac_ocp_modify(tp, 0xea1c, 0x0300, 0x0000);
    } else {
        mac_ocp_modify(tp, 0xea1c, 0x0004, 0x0000);
    }
    mac_ocp_modify(tp, 0xe0c0, 0x4f0f, 0x4403);
    mac_ocp_modify(tp, 0xe052, 0x0080, 0x0068);
    mac_ocp_modify(tp, 0xd430, 0x0fff, 0x047f);
    mac_ocp_modify(tp, 0xea1c, 0x0004, 0x0000);
    mac_ocp_modify(tp, 0xeb54, 0x0000, 0x0001);
    udelay(1);
    mac_ocp_modify(tp, 0xeb54, 0x0001, 0x0000);
    W16(tp, 0x1880, R16(tp, 0x1880) & ~0x0030);
    mac_ocp_write(tp, 0xe098, 0xc302);
    loop_wait(tp, mac_ocp_e00e, 1000, 10, false);
    disable_rxdvgate(tp);
}

static void rtl_hw_start_8125a_2(struct rtl *tp) {
    static const struct ephy_info e[] = {
        { 0x04, 0xffff, 0xd000 }, { 0x0a, 0xffff, 0x8653 }, { 0x23, 0xffff, 0xab66 },
        { 0x20, 0xffff, 0x9455 }, { 0x21, 0xffff, 0x99ff }, { 0x29, 0xffff, 0xfe04 },
        { 0x44, 0xffff, 0xd000 }, { 0x4a, 0xffff, 0x8653 }, { 0x63, 0xffff, 0xab66 },
        { 0x60, 0xffff, 0x9455 }, { 0x61, 0xffff, 0x99ff }, { 0x69, 0xffff, 0xfe04 },
    };
    set_def_aspm_entry_latency(tp);
    EPHY_INIT(tp, e);
    rtl_hw_start_8125_common(tp);
}

static void rtl_hw_start_8125b(struct rtl *tp) {
    static const struct ephy_info e[] = {
        { 0x0b, 0xffff, 0xa908 }, { 0x1e, 0xffff, 0x20eb }, { 0x4b, 0xffff, 0xa908 },
        { 0x5e, 0xffff, 0x20eb }, { 0x22, 0x0030, 0x0020 }, { 0x62, 0x0030, 0x0020 },
    };
    set_def_aspm_entry_latency(tp);
    EPHY_INIT(tp, e);
    rtl_hw_start_8125_common(tp);
}

static void rtl_hw_start_8125d(struct rtl *tp) {
    set_def_aspm_entry_latency(tp);
    rtl_hw_start_8125_common(tp);
}

static void rtl_hw_start_8126a(struct rtl *tp) {
    csi_mod(tp, 0x0890, 0x1, 0);   /* rtl_disable_zrxdc_timeout */
    set_def_aspm_entry_latency(tp);
    rtl_hw_start_8125_common(tp);
}

static void hw_config(struct rtl *tp) {
    switch (tp->ver) {
    case VER_40: rtl_hw_start_8168g_1(tp); break;
    case VER_42:
    case VER_43: rtl_hw_start_8168g_2(tp); break;
    case VER_44: rtl_hw_start_8411_2(tp); break;
    case VER_46:
    case VER_48: rtl_hw_start_8168h_1(tp); break;
    case VER_51: rtl_hw_start_8168ep_3(tp); break;
    case VER_52: rtl_hw_start_8117(tp); break;
    case VER_61: rtl_hw_start_8125a_2(tp); break;
    case VER_63: rtl_hw_start_8125b(tp); break;
    case VER_64:
    case VER_65:
    case VER_66: rtl_hw_start_8125d(tp); break;
    case VER_70: rtl_hw_start_8126a(tp); break;
    default: break;
    }
}

/* rtl_hw_aspm_clkreq_enable(tp, false): power saving stays off. */
static void aspm_clkreq_disable(struct rtl *tp) {
    if (tp->ver == VER_46 || tp->ver == VER_48 || is_8125(tp)) {
        mac_ocp_modify(tp, 0xe092, 0x00ff, 0);
    }
    if (tp->ver == VER_70) {
        W8(tp, INT_CFG0_8125, R8(tp, INT_CFG0_8125) & ~INT_CFG0_CLKREQEN);
    } else {
        W8(tp, Config2, R8(tp, Config2) & ~ClkReqEn);
    }
    W8(tp, Config5, R8(tp, Config5) & ~ASPM_en);
}

static void init_rxcfg(struct rtl *tp) {
    if (tp->ver <= VER_52) {
        W32(tp, RxConfig, RX128_INT_EN | RX_MULTI_EN | RX_DMA_BURST | RX_EARLY_OFF);
    } else if (tp->ver == VER_61) {
        W32(tp, RxConfig, RX_FETCH_DFLT_8125 | RX_DMA_BURST);
    } else {
        W32(tp, RxConfig, RX_FETCH_DFLT_8125 | RX_DMA_BURST | RX_PAUSE_SLOT_ON);
    }
}

static void irq_mask_and_ack(struct rtl *tp) {
    if (is_8125(tp)) {
        W32(tp, IntrMask_8125, 0);
        W32(tp, IntrStatus_8125, 0xffffffff);
    } else {
        W16(tp, IntrMask, 0);
        W16(tp, IntrStatus, 0xffff);
    }
    pci_commit(tp);
}

static void irq_enable(struct rtl *tp) {
    if (is_8125(tp)) {
        W32(tp, IntrMask_8125, tp->irq_mask);
    } else {
        W16(tp, IntrMask, (uint16_t)tp->irq_mask);
    }
}

/* rtl_reset_work -> rtl8169_cleanup + rtl_hw_start */
static void hw_start(struct rtl *tp) {
    irq_mask_and_ack(tp);
    W32(tp, RxConfig, R32(tp, RxConfig) & ~RX_CONFIG_ACCEPT_MASK);   /* rtl_rx_close */
    enable_rxdvgate(tp);
    sched_sleep_ms(2);
    hw_reset(tp);
    tp->cur_rx = tp->cur_tx = tp->dirty_tx = 0;
    for (uint32_t i = 0; i < RING_SIZE; i++) {
        tp->rx[i].addr = tp->rx_bufs_phys + (uint64_t)i * BUF_SIZE;
        tp->rx[i].opts2 = 0;
        tp->rx[i].opts1 = DescOwn | (i == RING_SIZE - 1 ? RingEnd : 0) | BUF_SIZE;
    }

    unlock_config(tp);
    aspm_clkreq_disable(tp);
    W16(tp, CPlusCmd, tp->cp_cmd);
    /* rtl_set_eee_txidle_timer */
    uint16_t timer = NET_MTU + 14 + 0x20;
    if (tp->ver == VER_46 || tp->ver == VER_48) {
        mac_ocp_write(tp, 0xe048, timer);
    } else if (is_8125(tp)) {
        W16(tp, EEE_TXIDLE_TIMER_8125, timer);
    }
    if (is_8125(tp)) {
        /* rtl_hw_start_8125: interrupt coalescing off, the extended tally counter on */
        W8(tp, INT_CFG0_8125, 0x00);
        uint32_t end = tp->ver == VER_63 || tp->ver == VER_70 ? 0xa80 : 0xb00;
        for (uint32_t i = 0xa00; i < end; i += 4) {
            W32(tp, i, 0);
        }
        if (tp->ver == VER_63 || tp->ver == VER_70) {
            W16(tp, INT_CFG1_8125, 0x0000);
        }
        mac_ocp_modify(tp, 0xea84, 0, 1u << 1 | 1u << 0);
        hw_config(tp);
    } else {
        /* rtl_hw_start_8168 */
        W8(tp, MaxTxPacketSize, EarlySize);
        hw_config(tp);
        W16(tp, IntrMitigate, 0x0000);
    }
    /* rtl_enable_exit_l1 */
    mac_ocp_modify(tp, 0xc0ac, 0, 0x1f80);
    W16(tp, RxMaxSize, RX_MAX_SIZE);
    W32(tp, TxDescStartAddrHigh, (uint32_t)(tp->tx_phys >> 32));
    W32(tp, TxDescStartAddrLow, (uint32_t)tp->tx_phys);
    W32(tp, RxDescAddrHigh, (uint32_t)(tp->rx_phys >> 32));
    W32(tp, RxDescAddrLow, (uint32_t)tp->rx_phys);
    lock_config(tp);
    pci_commit(tp);

    W8(tp, ChipCmd, CmdTxEnb | CmdRxEnb);
    init_rxcfg(tp);
    uint32_t txcfg = TX_DMA_BURST << 8 | InterFrameGap << 24;
    if (tp->ver <= VER_52) {
        txcfg |= TXCFG_AUTO_FIFO;   /* rtl_is_8168evl_up */
    }
    W32(tp, TxConfig, txcfg);
    uint32_t rx = R32(tp, RxConfig) & ~RX_CONFIG_ACCEPT_ERR_MASK;
    if (is_8125(tp)) {
        rx &= ~RX_VLAN_8125;
    }
    W32(tp, RxConfig, rx);
    /* rtl_set_rx_mode: our address, broadcasts, all multicasts */
    W32(tp, MAR0 + 4, 0xffffffff);
    W32(tp, MAR0 + 0, 0xffffffff);
    W32(tp, RxConfig, (R32(tp, RxConfig) & ~RX_CONFIG_ACCEPT_OK_MASK) | AcceptBroadcast | AcceptMyPhys | AcceptMulticast);
    irq_enable(tp);
}

/* --- link and data path -------------------------------------------------------------------------- */

static void update_link(struct rtl *tp, bool report) {
    uint16_t s = is_8125(tp) ? R16(tp, PHYstatus) : R8(tp, PHYstatus);
    bool up = s & LinkStatus;
    if (up == tp->dev.link_up && report) {
        return;
    }
    tp->dev.link_up = up;
    tp->dev.speed_mbps = s & _5000bpsF && tp->ver == VER_70 ? 5000 : s & _2500bpsF && is_8125(tp) ? 2500 :
                         s & _1000bpsF ? 1000 : s & _100bps ? 100 : 10;
    tp->dev.full_duplex = (s & FullDup) || tp->dev.speed_mbps >= 1000;
    if (report) {
        net_link_changed(&tp->dev);
    }
}

static void doorbell(struct rtl *tp) {
    if (is_8125(tp)) {
        W16(tp, TxPoll_8125, 1u << 0);
    } else {
        W8(tp, TxPoll, NPQ);
    }
}

static void reclaim_tx(struct rtl *tp) {
    while (tp->dirty_tx != tp->cur_tx && !(tp->tx[tp->dirty_tx % RING_SIZE].opts1 & DescOwn)) {
        tp->dirty_tx++;
    }
}

static int rtl_transmit(struct net_device *dev, const void *frame, size_t len) {
    struct rtl *tp = dev->priv;
    uint64_t irq = irq_save();
    reclaim_tx(tp);
    if (tp->cur_tx - tp->dirty_tx >= RING_SIZE - 1) {
        irq_restore(irq);
        return -1;
    }
    uint32_t entry = tp->cur_tx % RING_SIZE;
    uint8_t *buf = tp->tx_bufs + (uint64_t)entry * BUF_SIZE;
    memcpy(buf, frame, len);
    if (len < 60) {   /* rtl_quirk_packet_padto: ETH_ZLEN */
        memset(buf + len, 0, 60 - len);
        len = 60;
    }
    volatile struct rtl_desc *d = &tp->tx[entry];
    d->addr = tp->tx_bufs_phys + (uint64_t)entry * BUF_SIZE;
    d->opts2 = 0;
    __asm__ volatile ("sfence" ::: "memory");
    d->opts1 = DescOwn | FirstFrag | LastFrag | (entry == RING_SIZE - 1 ? RingEnd : 0) | (uint32_t)len;
    tp->cur_tx++;
    __asm__ volatile ("sfence" ::: "memory");
    doorbell(tp);
    dev->tx_packets++;
    dev->tx_bytes += len;
    irq_restore(irq);
    return 0;
}

/* rtl_rx: every descriptor the chip gave back. */
static void rtl_poll(struct net_device *dev) {
    struct rtl *tp = dev->priv;
    for (int count = 0; count < RING_SIZE; count++) {
        uint32_t entry = tp->cur_rx % RING_SIZE;
        volatile struct rtl_desc *d = &tp->rx[entry];
        uint32_t status = d->opts1;
        if (status & DescOwn) {
            break;
        }
        __asm__ volatile ("lfence" ::: "memory");
        uint32_t size = status & 0x3fff;
        if (status & RxRES) {
            dev->rx_errors++;
        } else if ((status & (FirstFrag | LastFrag)) != (FirstFrag | LastFrag) || size < 4 + 14) {
            dev->rx_dropped++;   /* (frames bigger than one buffer) */
        } else {
            net_receive(dev, tp->rx_bufs + (uint64_t)entry * BUF_SIZE, size - 4);   /* minus the FCS */
        }
        d->opts2 = 0;
        __asm__ volatile ("sfence" ::: "memory");
        d->opts1 = DescOwn | (status & RingEnd) | BUF_SIZE;   /* rtl8169_mark_to_asic */
        tp->cur_rx++;
    }
    uint64_t irq = irq_save();
    reclaim_tx(tp);
    irq_restore(irq);
    if (!dev->has_interrupt) {   /* (polled: clear the events ourselves) */
        if (is_8125(tp)) {
            W32(tp, IntrStatus_8125, R32(tp, IntrStatus_8125));
        } else {
            W16(tp, IntrStatus, R16(tp, IntrStatus));
        }
    }
    update_link(tp, true);
}

static void rtl_interrupt(void *arg) {
    struct rtl *tp = arg;
    uint32_t status = is_8125(tp) ? R32(tp, IntrStatus_8125) : R16(tp, IntrStatus);
    if ((status & 0xffff) == 0xffff || !(status & tp->irq_mask)) {
        return;
    }
    if (is_8125(tp)) {
        W32(tp, IntrStatus_8125, status);
    } else {
        W16(tp, IntrStatus, (uint16_t)status);
    }
    net_wake();
}

/* --- probing (rtl_init_one, rtl_open) ------------------------------------------------------------- */

static void probe_one(const struct pci_device *d, bool gmii) {
    if (card_count == MAX_CARDS) {
        return;
    }
    struct rtl *tp = &cards[card_count];
    memset(tp, 0, sizeof(*tp));
    tp->pci = *d;
    tp->gmii = gmii;
    tp->ocp_base = OCP_STD_PHY_BASE;

    /* The first memory BAR (the I/O one comes first on most of them). */
    uint64_t bar = 0, size = 0;
    for (int i = 0; i < 6 && !bar; i++) {
        bar = pci_bar_address(d, i);
        size = bar ? pci_bar_size(d, i) : 0;
    }
    if (!bar) {
        kprintf("[WARN] r8169: %02x:%02x.%u has no register window\n", d->bus, d->slot, d->function);
        return;
    }
    pci_write16(d, PCI_COMMAND, (uint16_t)(pci_read16(d, PCI_COMMAND) | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER));
    tp->mmio = vmm_map_mmio(bar, size ? size : 0x10000);
    if (!tp->mmio) {
        return;
    }
    uint32_t txconfig = R32(tp, TxConfig);
    if (txconfig == ~0u) {
        kprintf("[WARN] r8169: PCI read failed\n");
        return;
    }
    uint32_t xid = (txconfig >> 20) & 0xfcf;
    const char *name = NULL;
    size_t k = 0;
    while ((xid & chips[k].mask) != chips[k].val) {
        k++;
    }
    tp->ver = chips[k].ver;
    name = chips[k].name;
    if (tp->ver == VER_EXTENDED) {
        xid = R32(tp, TX_CONFIG_V2);
        tp->ver = (xid & 0x7fffffff) == 0 ? VER_64 : VER_NONE;
        name = "RTL9151AS";
    }
    if (tp->ver == VER_42 && !gmii) {
        tp->ver = VER_43;
        name = "RTL8106eus";
    } else if (tp->ver == VER_46 && !gmii) {
        tp->ver = VER_48;
        name = "RTL8107e";
    }
    if (tp->ver == VER_OLD || tp->ver == VER_NONE) {
        kprintf("[WARN] r8169: Realtek %s (XID %03x) isn't supported yet: the ported chips are the RTL8168G "
                "and newer\n", name ? name : "chip", xid);
        return;
    }

    if (is_8125(tp) && (mac_ocp_read(tp, 0xd006) & 0xff) == 0x07) {
        kprintf("[WARN] r8169: %s in SFP mode isn't supported\n", name);
        return;
    }
    tp->cp_cmd = (uint16_t)(R16(tp, CPlusCmd) & CPCMD_MASK & ~(RxVlan | RxChkSum));
    init_rxcfg(tp);
    irq_mask_and_ack(tp);
    hw_initialize(tp);
    hw_reset(tp);

    /* rtl_init_mac_address: the copy in the chip's ERI/backup registers, else MAC0. */
    uint8_t mac[6] = { 0 };
    if (tp->ver <= VER_52) {
        uint32_t lo = eri_read(tp, 0xe0), hi = eri_read(tp, 0xe4);
        uint8_t m[6] = { (uint8_t)lo, (uint8_t)(lo >> 8), (uint8_t)(lo >> 16), (uint8_t)(lo >> 24), (uint8_t)hi,
                         (uint8_t)(hi >> 8) };
        memcpy(mac, m, 6);
    } else {
        for (int i = 0; i < 6; i++) {
            mac[i] = R8(tp, MAC0_BKP + (uint32_t)i);
        }
    }
    bool valid = (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) && !(mac[0] & 1);
    if (!valid) {
        for (int i = 0; i < 6; i++) {
            mac[i] = R8(tp, MAC0 + (uint32_t)i);
        }
        valid = (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) && !(mac[0] & 1);
    }
    if (!valid) {
        kprintf("[WARN] r8169: can't read the MAC address\n");
        return;
    }
    unlock_config(tp);
    W32(tp, MAC4, (uint32_t)mac[4] | (uint32_t)mac[5] << 8);
    pci_commit(tp);
    W32(tp, MAC0, (uint32_t)mac[0] | (uint32_t)mac[1] << 8 | (uint32_t)mac[2] << 16 | (uint32_t)mac[3] << 24);
    pci_commit(tp);
    lock_config(tp);
    /* rtl_set_d3_pll_down(tp, true) */
    W8(tp, PMCH, R8(tp, PMCH) | D3HOT_NO_PLL_DOWN);
    W8(tp, PMCH, R8(tp, PMCH) & ~D3COLD_NO_PLL_DOWN);

    void *rx = NULL, *tx = NULL, *rxb = NULL, *txb = NULL;
    tp->rx_phys = nic_dma_alloc(RING_SIZE * sizeof(struct rtl_desc), false, &rx);
    tp->tx_phys = nic_dma_alloc(RING_SIZE * sizeof(struct rtl_desc), false, &tx);
    tp->rx_bufs_phys = nic_dma_alloc((uint64_t)RING_SIZE * BUF_SIZE, false, &rxb);
    tp->tx_bufs_phys = nic_dma_alloc((uint64_t)RING_SIZE * BUF_SIZE, false, &txb);
    if (!tp->rx_phys || !tp->tx_phys || !tp->rx_bufs_phys || !tp->tx_bufs_phys) {
        kprintf("[WARN] r8169: out of memory for the rings\n");
        return;
    }
    tp->rx = rx;
    tp->tx = tx;
    tp->rx_bufs = rxb;
    tp->tx_bufs = txb;
    tp->irq_mask = RxOK | RxErr | TxOK | TxErr | LinkChg;

    /* rtl8169_up: the PHY, then the MAC. */
    init_phy(tp);
    hw_start(tp);

    struct net_device *dev = &tp->dev;
    memcpy(dev->mac, mac, 6);
    dev->driver = "r8169";
    strlcpy(dev->model, "Realtek ", sizeof(dev->model));
    strlcpy(dev->model + 8, name, sizeof(dev->model) - 8);
    dev->priv = tp;
    dev->transmit = rtl_transmit;
    dev->poll = rtl_poll;
    dev->has_interrupt = pci_enable_msi(&tp->pci, rtl_interrupt, tp, true);
    update_link(tp, false);
    card_count++;
    net_register(dev);
}

void r8169_probe(void) {
    static const struct {
        uint16_t vendor, device;
    } ids[] = {
        { 0x10ec, 0x2502 }, { 0x10ec, 0x2600 }, { 0x10ec, 0x8129 }, { 0x10ec, 0x8136 }, { 0x10ec, 0x8161 },
        { 0x10ec, 0x8162 }, { 0x10ec, 0x8167 }, { 0x10ec, 0x8168 }, { 0x10ff, 0x8168 }, { 0x10ec, 0x8169 },
        { 0x1186, 0x4300 }, { 0x1186, 0x4302 }, { 0x1259, 0xc107 }, { 0x16ec, 0x0116 }, { 0x1737, 0x1032 },
        { 0x0001, 0x8168 }, { 0x10ec, 0x8125 }, { 0x10ec, 0x8126 }, { 0x10ec, 0x8127 }, { 0x10ec, 0x3000 },
        { 0x10ec, 0x5000 }, { 0x10ec, 0x0e10 },
    };
    struct pci_device d;
    for (size_t k = 0; k < sizeof(ids) / sizeof(ids[0]); k++) {
        for (int i = 0; pci_find_id(ids[k].vendor, ids[k].device, i, &d); i++) {
            probe_one(&d, !(ids[k].vendor == 0x10ec && ids[k].device == 0x8136));
        }
    }
}
