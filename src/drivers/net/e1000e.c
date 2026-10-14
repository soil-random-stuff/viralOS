/*
 * Intel PCI Express gigabit Ethernet, after linux/drivers/net/intel/e1000e:
 *   - 82571/82572/82573/82574/82583 and 80003ES2LAN (82571.c, 80003es2lan.c): QEMU's "e1000e" is an
 *     82574L;
 *   - the controllers built into Intel chipsets (ich8lan.c): ICH8/9/10 with 82566/82567 PHYs, and
 *     PCH with 82577/82578, 82579, I217, I218 and I219 PHYs: the "Intel Ethernet Connection" of most
 *     Intel desktops and laptops since 2009.
 *
 * Each family gets its reset and hardware setup, the I21x ones including the PHY wake-up steps
 * (ULP off, SMBus->PCIe, LANPHYPC power cycle) without which the PHY stays unreachable after
 * Windows or the firmware put it to sleep. Not ported: Wake-on-LAN, EEE, the K1 power-state and
 * jumbo-frame workarounds, the NVM extended PHY configuration (the hardware loads it itself when
 * we don't gate it), flash/NVM writes. The data path is e1000_common.c; interrupts by MSI.
 */

#include "drivers/net/e1000_hw.h"
#include "arch/x86_64/cpu.h"
#include "drivers/timer.h"
#include "lib/kprintf.h"
#include "sched/sched.h"

#define E1000_PBS               0x01008
#define E1000_PBA_ECC           0x01100
#define E1000_FEXTNVM12         0x05BC0
#define E1000_FFLT_DBG          0x05F04
#define E1000_PBA_8K            0x0008
#define E1000_PBS_16K           0x0010
#define E1000_EECD_REQ          0x00000040
#define E1000_EECD_GNT          0x00000080
#define E1000_PBA_ECC_CORR_EN   0x00000001
#define E1000_EXTCNF_CTRL_SWFLAG 0x00000020   /* (= MDIO_SW_OWNERSHIP on the 82573/4) */
#define E1000_EXTCNF_CTRL_GATE_PHY_CFG 0x00000080
#define E1000_ICH_FWSM_RSPCIPHY 0x00000040
#define E1000_ICH_FWSM_FW_VALID 0x00008000
#define E1000_FWSM_ULP_CFG_DONE 0x00000400
#define E1000_H2ME_ULP          0x00000800
#define E1000_H2ME_ENFORCE_SETTINGS 0x00001000
#define E1000_FEXTNVM3_PHY_CFG_COUNTER_MASK  0x0C000000
#define E1000_FEXTNVM3_PHY_CFG_COUNTER_50MSEC 0x08000000
#define E1000_FEXTNVM7_DISABLE_SMB_PERST 0x00000020
#define E1000_FEXTNVM12_PHYPD_CTRL_MASK 0x00C00000
#define E1000_FEXTNVM12_PHYPD_CTRL_P1   0x00800000
#define E1000_FFLT_DBG_DONT_GATE_WAKE_DMA_CLK 0x1000
#define E1000_KABGTXD_BGSQLBIAS 0x00050000
#define E1000_PBECCSTS_ECC_ENABLE 0x00010000
#define E1000_GCR_L1_ACT_WITHOUT_L0S_RX 0x08000000
#define E1000_RFCTL_NFSW_DIS    0x00000040
#define E1000_RFCTL_NFSR_DIS    0x00000080
#define E1000_RFCTL_IPV6_EX_DIS 0x00010000
#define E1000_RFCTL_NEW_IPV6_EXT_DIS 0x00020000
#define E1000_TARC0_CB_MULTIQ_3_REQ 0x30000000
#define E1000_TARC0_CB_MULTIQ_2_REQ 0x20000000
#define PCIE_NO_SNOOP_ALL       0x0000003F   /* GCR: RXD, RXDSCW, RXDSCR, TXD, TXDSCW, TXDSCR */
#define E1000_COLLISION_DISTANCE 63

/* PHY registers by page (ich8lan.h PHY_REG) on the PCH PHYs */
#define PHY_REG(page, reg)      ((page) << 5 | (reg))
#define HV_INTC_FC_PAGE_START   768
#define IGP01E1000_PHY_PAGE_SELECT 0x1F
#define BM_PORT_GEN_CFG         PHY_REG(769, 17)
#define CV_SMB_CTRL             PHY_REG(769, 23)
#define HV_PM_CTRL              PHY_REG(770, 17)
#define I218_ULP_CONFIG1        PHY_REG(779, 16)
#define BM_WUC_HOST_WU_BIT      0x0010
#define CV_SMB_CTRL_FORCE_SMBUS 0x0001
#define HV_PM_CTRL_K1_ENABLE    0x4000
#define I218_ULP_CONFIG1_START  0x0001
#define I218_ULP_CONFIG1_IND    0x0004
#define I218_ULP_CONFIG1_STICKY_ULP 0x0010
#define I218_ULP_CONFIG1_INBAND_EXIT 0x0020
#define I218_ULP_CONFIG1_WOL_HOST 0x0040
#define I218_ULP_CONFIG1_RESET_TO_SMBUS 0x0100
#define I218_ULP_CONFIG1_EN_ULP_LANPHYPC 0x0400
#define I218_ULP_CONFIG1_DIS_CLR_STICKY_ON_PERST 0x0800
#define I218_ULP_CONFIG1_DISABLE_SMB_PERST 0x1000

/* enum e1000_mac_type, in the same order (later = newer) */
enum mac_type {
    B_82571, B_82572, B_82573, B_82574, B_82583, B_80003ES2LAN,
    B_ICH8LAN, B_ICH9LAN, B_ICH10LAN, B_PCHLAN, B_PCH2LAN,
    B_PCH_LPT, B_PCH_SPT, B_PCH_CNP, B_PCH_TGP, B_PCH_ADP, B_PCH_MTP, B_PCH_PTP,
};

static const struct {
    uint16_t device;
    enum mac_type type;
    const char *name;
} ids[] = {
    /* (linux/drivers/net/intel/e1000e/netdev.c e1000_pci_tbl) */
    { 0x105e, B_82571, "Intel 82571EB Gigabit Ethernet" },
    { 0x105f, B_82571, "Intel 82571EB Gigabit Ethernet" },
    { 0x10a4, B_82571, "Intel 82571EB Gigabit Ethernet" },
    { 0x10bc, B_82571, "Intel 82571EB Gigabit Ethernet" },
    { 0x10a5, B_82571, "Intel 82571EB Gigabit Ethernet" },
    { 0x1060, B_82571, "Intel 82571EB Gigabit Ethernet" },
    { 0x10d9, B_82571, "Intel 82571EB Gigabit Ethernet" },
    { 0x10da, B_82571, "Intel 82571EB Gigabit Ethernet" },
    { 0x10d5, B_82571, "Intel 82571PT Gigabit Ethernet" },
    { 0x10b9, B_82572, "Intel 82572EI Gigabit Ethernet" },
    { 0x107d, B_82572, "Intel 82572EI Gigabit Ethernet" },
    { 0x107e, B_82572, "Intel 82572EI Gigabit Ethernet" },
    { 0x107f, B_82572, "Intel 82572EI Gigabit Ethernet" },
    { 0x108b, B_82573, "Intel 82573E Gigabit Ethernet" },
    { 0x108c, B_82573, "Intel 82573E Gigabit Ethernet" },
    { 0x109a, B_82573, "Intel 82573L Gigabit Ethernet" },
    { 0x10d3, B_82574, "Intel 82574L Gigabit Ethernet" },
    { 0x10f6, B_82574, "Intel 82574LA Gigabit Ethernet" },
    { 0x150c, B_82583, "Intel 82583V Gigabit Ethernet" },
    { 0x1096, B_80003ES2LAN, "Intel 80003ES2LAN Gigabit Ethernet" },
    { 0x10ba, B_80003ES2LAN, "Intel 80003ES2LAN Gigabit Ethernet" },
    { 0x1098, B_80003ES2LAN, "Intel 80003ES2LAN Gigabit Ethernet" },
    { 0x10bb, B_80003ES2LAN, "Intel 80003ES2LAN Gigabit Ethernet" },
    { 0x104c, B_ICH8LAN, "Intel 82562 Fast Ethernet (ICH8)" },
    { 0x10c5, B_ICH8LAN, "Intel 82562 Fast Ethernet (ICH8)" },
    { 0x10c4, B_ICH8LAN, "Intel 82562 Fast Ethernet (ICH8)" },
    { 0x104a, B_ICH8LAN, "Intel 82566 Gigabit Ethernet (ICH8)" },
    { 0x104b, B_ICH8LAN, "Intel 82566 Gigabit Ethernet (ICH8)" },
    { 0x104d, B_ICH8LAN, "Intel 82566 Gigabit Ethernet (ICH8)" },
    { 0x1049, B_ICH8LAN, "Intel 82566 Gigabit Ethernet (ICH8)" },
    { 0x1501, B_ICH8LAN, "Intel 82567 Gigabit Ethernet (ICH8)" },
    { 0x10c0, B_ICH9LAN, "Intel 82562 Fast Ethernet (ICH9)" },
    { 0x10c2, B_ICH9LAN, "Intel 82562 Fast Ethernet (ICH9)" },
    { 0x10c3, B_ICH9LAN, "Intel 82562 Fast Ethernet (ICH9)" },
    { 0x10bd, B_ICH9LAN, "Intel 82566 Gigabit Ethernet (ICH9)" },
    { 0x294c, B_ICH9LAN, "Intel 82566 Gigabit Ethernet (ICH9)" },
    { 0x10e5, B_ICH9LAN, "Intel 82567 Gigabit Ethernet (ICH9)" },
    { 0x10bf, B_ICH9LAN, "Intel 82566 Gigabit Ethernet (ICH9)" },
    { 0x10f5, B_ICH9LAN, "Intel 82566 Gigabit Ethernet (ICH9)" },
    { 0x10cb, B_ICH9LAN, "Intel 82566 Gigabit Ethernet (ICH9)" },
    { 0x10cc, B_ICH9LAN, "Intel 82567 Gigabit Ethernet (ICH10)" },
    { 0x10cd, B_ICH9LAN, "Intel 82567 Gigabit Ethernet (ICH10)" },
    { 0x10ce, B_ICH9LAN, "Intel 82567 Gigabit Ethernet (ICH10)" },
    { 0x10de, B_ICH10LAN, "Intel 82567 Gigabit Ethernet (ICH10)" },
    { 0x10df, B_ICH10LAN, "Intel 82567 Gigabit Ethernet (ICH10)" },
    { 0x1525, B_ICH10LAN, "Intel 82567 Gigabit Ethernet (ICH10)" },
    { 0x10ea, B_PCHLAN, "Intel 82577 Gigabit Ethernet" },
    { 0x10eb, B_PCHLAN, "Intel 82577 Gigabit Ethernet" },
    { 0x10ef, B_PCHLAN, "Intel 82578 Gigabit Ethernet" },
    { 0x10f0, B_PCHLAN, "Intel 82578 Gigabit Ethernet" },
    { 0x1502, B_PCH2LAN, "Intel 82579LM Gigabit Ethernet" },
    { 0x1503, B_PCH2LAN, "Intel 82579V Gigabit Ethernet" },
    { 0x153a, B_PCH_LPT, "Intel Ethernet Connection I217-LM" },
    { 0x153b, B_PCH_LPT, "Intel Ethernet Connection I217-V" },
    { 0x155a, B_PCH_LPT, "Intel Ethernet Connection I218-LM" },
    { 0x1559, B_PCH_LPT, "Intel Ethernet Connection I218-V" },
    { 0x15a0, B_PCH_LPT, "Intel Ethernet Connection I218-LM" },
    { 0x15a1, B_PCH_LPT, "Intel Ethernet Connection I218-V" },
    { 0x15a2, B_PCH_LPT, "Intel Ethernet Connection I218-LM" },
    { 0x15a3, B_PCH_LPT, "Intel Ethernet Connection I218-V" },
    { 0x156f, B_PCH_SPT, "Intel Ethernet Connection I219-LM" },
    { 0x1570, B_PCH_SPT, "Intel Ethernet Connection I219-V" },
    { 0x15b7, B_PCH_SPT, "Intel Ethernet Connection I219-LM" },
    { 0x15b8, B_PCH_SPT, "Intel Ethernet Connection I219-V" },
    { 0x15b9, B_PCH_SPT, "Intel Ethernet Connection I219-LM" },
    { 0x15d7, B_PCH_SPT, "Intel Ethernet Connection I219-LM" },
    { 0x15d8, B_PCH_SPT, "Intel Ethernet Connection I219-V" },
    { 0x15e3, B_PCH_SPT, "Intel Ethernet Connection I219-LM" },
    { 0x15d6, B_PCH_SPT, "Intel Ethernet Connection I219-V" },
    { 0x15bd, B_PCH_CNP, "Intel Ethernet Connection I219-LM" },
    { 0x15be, B_PCH_CNP, "Intel Ethernet Connection I219-V" },
    { 0x15bb, B_PCH_CNP, "Intel Ethernet Connection I219-LM" },
    { 0x15bc, B_PCH_CNP, "Intel Ethernet Connection I219-V" },
    { 0x15df, B_PCH_CNP, "Intel Ethernet Connection I219-LM" },
    { 0x15e0, B_PCH_CNP, "Intel Ethernet Connection I219-V" },
    { 0x15e1, B_PCH_CNP, "Intel Ethernet Connection I219-LM" },
    { 0x15e2, B_PCH_CNP, "Intel Ethernet Connection I219-V" },
    { 0x0d4e, B_PCH_CNP, "Intel Ethernet Connection I219-LM" },
    { 0x0d4f, B_PCH_CNP, "Intel Ethernet Connection I219-V" },
    { 0x0d4c, B_PCH_CNP, "Intel Ethernet Connection I219-LM" },
    { 0x0d4d, B_PCH_CNP, "Intel Ethernet Connection I219-V" },
    { 0x0d53, B_PCH_SPT, "Intel Ethernet Connection I219-LM" },
    { 0x0d55, B_PCH_SPT, "Intel Ethernet Connection I219-V" },
    { 0x15fb, B_PCH_TGP, "Intel Ethernet Connection I219-LM" },
    { 0x15fc, B_PCH_TGP, "Intel Ethernet Connection I219-V" },
    { 0x15f9, B_PCH_TGP, "Intel Ethernet Connection I219-LM" },
    { 0x15fa, B_PCH_TGP, "Intel Ethernet Connection I219-V" },
    { 0x15f4, B_PCH_TGP, "Intel Ethernet Connection I219-LM" },
    { 0x15f5, B_PCH_TGP, "Intel Ethernet Connection I219-V" },
    { 0x0dc5, B_PCH_ADP, "Intel Ethernet Connection I219-LM" },
    { 0x0dc6, B_PCH_ADP, "Intel Ethernet Connection I219-V" },
    { 0x1a1e, B_PCH_ADP, "Intel Ethernet Connection I219-LM" },
    { 0x1a1f, B_PCH_ADP, "Intel Ethernet Connection I219-V" },
    { 0x1a1c, B_PCH_ADP, "Intel Ethernet Connection I219-LM" },
    { 0x1a1d, B_PCH_ADP, "Intel Ethernet Connection I219-V" },
    { 0x0dc7, B_PCH_ADP, "Intel Ethernet Connection I219-LM" },
    { 0x0dc8, B_PCH_ADP, "Intel Ethernet Connection I219-V" },
    { 0x550c, B_PCH_ADP, "Intel Ethernet Connection I219-LM" },
    { 0x550d, B_PCH_ADP, "Intel Ethernet Connection I219-V" },
    { 0x550a, B_PCH_MTP, "Intel Ethernet Connection I219-LM" },
    { 0x550b, B_PCH_MTP, "Intel Ethernet Connection I219-V" },
    { 0x550e, B_PCH_MTP, "Intel Ethernet Connection I219-LM" },
    { 0x550f, B_PCH_MTP, "Intel Ethernet Connection I219-V" },
    { 0x5510, B_PCH_MTP, "Intel Ethernet Connection I219-LM" },
    { 0x5511, B_PCH_MTP, "Intel Ethernet Connection I219-V" },
    { 0x57a0, B_PCH_MTP, "Intel Ethernet Connection I219-LM" },
    { 0x57a1, B_PCH_MTP, "Intel Ethernet Connection I219-V" },
    { 0x57b3, B_PCH_PTP, "Intel Ethernet Connection I219-LM" },
    { 0x57b4, B_PCH_PTP, "Intel Ethernet Connection I219-V" },
    { 0x57b7, B_PCH_PTP, "Intel Ethernet Connection I219-LM" },
    { 0x57b8, B_PCH_PTP, "Intel Ethernet Connection I219-V" },
    { 0x57b9, B_PCH_PTP, "Intel Ethernet Connection I219-LM" },
    { 0x57ba, B_PCH_PTP, "Intel Ethernet Connection I219-V" },
};

#define MAX_CARDS 4
static struct e1000_card cards[MAX_CARDS];
static enum mac_type card_type[MAX_CARDS];
static int card_count;

static bool is_ich(enum mac_type t) { return t >= B_ICH8LAN; }
static bool is_pch(enum mac_type t) { return t >= B_PCHLAN; }

/* --- semaphores ---------------------------------------------------------------------------------- */

/* e1000_acquire_swflag_ich8lan / e1000_get_hw_semaphore_82573: the PHY and NVM are ours. */
static bool acquire_swflag(struct e1000_card *c) {
    int timeout = 100;   /* PHY_CFG_TIMEOUT */
    while (er32(c, E1000_EXTCNF_CTRL) & E1000_EXTCNF_CTRL_SWFLAG && timeout--) {
        sched_sleep_ms(1);
    }
    for (int i = 0; i < 1000; i++) {   /* SW_FLAG_TIMEOUT */
        ew32(c, E1000_EXTCNF_CTRL, er32(c, E1000_EXTCNF_CTRL) | E1000_EXTCNF_CTRL_SWFLAG);
        if (er32(c, E1000_EXTCNF_CTRL) & E1000_EXTCNF_CTRL_SWFLAG) {
            return true;
        }
        sched_sleep_ms(1);
    }
    return false;
}

static void release_swflag(struct e1000_card *c) {
    ew32(c, E1000_EXTCNF_CTRL, er32(c, E1000_EXTCNF_CTRL) & ~E1000_EXTCNF_CTRL_SWFLAG);
}

/* e1000_check_reset_block_ich8lan: the Management Engine may forbid PHY resets. */
static bool reset_blocked(struct e1000_card *c) {
    for (int i = 0; i < 30; i++) {
        if (er32(c, E1000_FWSM) & E1000_ICH_FWSM_RSPCIPHY) {
            return false;
        }
        sched_sleep_ms(10);
    }
    return true;
}

/* --- PCH PHY registers on pages (__e1000_read_phy_reg_hv / write) ------------------------------ */

static bool hv_access(struct e1000_card *c, uint32_t offset, uint16_t *data, bool read) {
    uint32_t page = offset >> 5, reg = offset & 0x1F;
    int saved = c->phy_addr;
    c->phy_addr = page >= HV_INTC_FC_PAGE_START ? 1 : 2;
    bool ok = true;
    if (page == HV_INTC_FC_PAGE_START) {
        page = 0;
    }
    if (reg > 0xF) {   /* MAX_PHY_MULTI_PAGE_REG: set the page first (page x 32) */
        int a = c->phy_addr;
        c->phy_addr = 1;
        ok = e1000_phy_write(c, IGP01E1000_PHY_PAGE_SELECT, (uint16_t)(page << 5));
        c->phy_addr = a;
    }
    if (ok) {
        ok = read ? e1000_phy_read(c, reg, data) : e1000_phy_write(c, reg, *data);
    }
    c->phy_addr = saved;
    return ok;
}

static bool hv_read(struct e1000_card *c, uint32_t offset, uint16_t *data) {
    return hv_access(c, offset, data, true);
}

static bool hv_write(struct e1000_card *c, uint32_t offset, uint16_t data) {
    return hv_access(c, offset, &data, false);
}

/* --- the I21x PHY wake-up (ich8lan.c) ---------------------------------------------------------- */

/* e1000_toggle_lanphypc_pch_lpt: power-cycle the PHY through the LANPHYPC pin. */
static void toggle_lanphypc(struct e1000_card *c, enum mac_type t) {
    uint32_t reg = er32(c, E1000_FEXTNVM3);
    reg &= ~E1000_FEXTNVM3_PHY_CFG_COUNTER_MASK;
    reg |= E1000_FEXTNVM3_PHY_CFG_COUNTER_50MSEC;
    ew32(c, E1000_FEXTNVM3, reg);

    reg = er32(c, E1000_CTRL);
    reg |= E1000_CTRL_LANPHYPC_OVERRIDE;
    reg &= ~E1000_CTRL_LANPHYPC_VALUE;
    ew32(c, E1000_CTRL, reg);
    e1e_flush(c);
    udelay(20);
    reg &= ~E1000_CTRL_LANPHYPC_OVERRIDE;
    ew32(c, E1000_CTRL, reg);
    e1e_flush(c);
    if (t < B_PCH_LPT) {
        sched_sleep_ms(50);
    } else {
        for (int count = 20; !(er32(c, E1000_CTRL_EXT) & E1000_CTRL_EXT_LPCD) && count--;) {
            sched_sleep_ms(5);
        }
        sched_sleep_ms(30);
    }
}

/* e1000e_phy_hw_reset_generic + e1000_get_cfg_done_ich8lan */
static void phy_hw_reset(struct e1000_card *c, enum mac_type t) {
    if (is_ich(t) && reset_blocked(c)) {
        return;
    }
    uint32_t ctrl = er32(c, E1000_CTRL);
    ew32(c, E1000_CTRL, ctrl | E1000_CTRL_PHY_RST);
    e1e_flush(c);
    udelay(100);
    ew32(c, E1000_CTRL, ctrl);
    e1e_flush(c);
    udelay(150);
    sched_sleep_ms(10);
}

/* e1000_phy_is_accessible_pchlan: the PHY answers (and then, on I217+ without the ME, out of SMBus). */
static bool phy_accessible(struct e1000_card *c, enum mac_type t) {
    uint16_t id1 = 0xFFFF, id2 = 0xFFFF;
    bool ok = false;
    c->phy_addr = 2;
    for (int retry = 0; retry < 2 && !ok; retry++) {
        ok = e1000_phy_read(c, MII_PHYSID1, &id1) && id1 != 0xFFFF && e1000_phy_read(c, MII_PHYSID2, &id2) &&
             id2 != 0xFFFF;
    }
    if (!ok) {
        return false;
    }
    if (t >= B_PCH_LPT && !(er32(c, E1000_FWSM) & E1000_ICH_FWSM_FW_VALID)) {
        uint16_t reg;
        if (hv_read(c, CV_SMB_CTRL, &reg)) {
            hv_write(c, CV_SMB_CTRL, reg & ~CV_SMB_CTRL_FORCE_SMBUS);
        }
        ew32(c, E1000_CTRL_EXT, er32(c, E1000_CTRL_EXT) & ~E1000_CTRL_EXT_FORCE_SMBUS);
    }
    return true;
}

/* e1000_disable_ulp_lpt_lp (force): bring the PHY out of Ultra Low Power, where Windows or the
   firmware may have left it. */
static void disable_ulp(struct e1000_card *c, enum mac_type t) {
    uint16_t dev = c->pci.device;
    if (t < B_PCH_LPT || dev == 0x153A || dev == 0x153B || dev == 0x15A0 || dev == 0x15A1) {
        return;   /* (I217-LM/V, I218-LM2/V2 have no ULP) */
    }
    if (er32(c, E1000_FWSM) & E1000_ICH_FWSM_FW_VALID) {
        /* The Management Engine does it on request. */
        ew32(c, E1000_H2ME, (er32(c, E1000_H2ME) & ~E1000_H2ME_ULP) | E1000_H2ME_ENFORCE_SETTINGS);
        for (int i = 0; er32(c, E1000_FWSM) & E1000_FWSM_ULP_CFG_DONE && i < 250; i++) {
            sched_sleep_ms(10);
        }
        ew32(c, E1000_H2ME, er32(c, E1000_H2ME) & ~E1000_H2ME_ENFORCE_SETTINGS);
        return;
    }
    if (!acquire_swflag(c)) {
        return;
    }
    toggle_lanphypc(c, t);
    uint16_t reg;
    if (!hv_read(c, CV_SMB_CTRL, &reg)) {
        /* The MAC may be in PCIe mode: force SMBus to reach the PHY. */
        ew32(c, E1000_CTRL_EXT, er32(c, E1000_CTRL_EXT) | E1000_CTRL_EXT_FORCE_SMBUS);
        sched_sleep_ms(50);
        if (!hv_read(c, CV_SMB_CTRL, &reg)) {
            release_swflag(c);
            phy_hw_reset(c, t);
            sched_sleep_ms(50);
            return;
        }
    }
    hv_write(c, CV_SMB_CTRL, reg & ~CV_SMB_CTRL_FORCE_SMBUS);
    ew32(c, E1000_CTRL_EXT, er32(c, E1000_CTRL_EXT) & ~E1000_CTRL_EXT_FORCE_SMBUS);
    if (hv_read(c, HV_PM_CTRL, &reg)) {
        hv_write(c, HV_PM_CTRL, reg | HV_PM_CTRL_K1_ENABLE);   /* K1 back on (ULP turned it off) */
    }
    if (hv_read(c, I218_ULP_CONFIG1, &reg)) {
        reg &= ~(I218_ULP_CONFIG1_IND | I218_ULP_CONFIG1_STICKY_ULP | I218_ULP_CONFIG1_RESET_TO_SMBUS |
                 I218_ULP_CONFIG1_WOL_HOST | I218_ULP_CONFIG1_INBAND_EXIT | I218_ULP_CONFIG1_EN_ULP_LANPHYPC |
                 I218_ULP_CONFIG1_DIS_CLR_STICKY_ON_PERST | I218_ULP_CONFIG1_DISABLE_SMB_PERST);
        hv_write(c, I218_ULP_CONFIG1, reg);
        hv_write(c, I218_ULP_CONFIG1, reg | I218_ULP_CONFIG1_START);   /* commit */
    }
    ew32(c, E1000_FEXTNVM7, er32(c, E1000_FEXTNVM7) & ~E1000_FEXTNVM7_DISABLE_SMB_PERST);
    release_swflag(c);
    phy_hw_reset(c, t);
    sched_sleep_ms(50);
}

/* e1000_gate_hw_phy_config_ich8lan (82579 only here: see the top) */
static void gate_phy_config(struct e1000_card *c, enum mac_type t, bool gate) {
    if (t != B_PCH2LAN) {
        return;
    }
    uint32_t r = er32(c, E1000_EXTCNF_CTRL);
    ew32(c, E1000_EXTCNF_CTRL, gate ? r | E1000_EXTCNF_CTRL_GATE_PHY_CFG : r & ~E1000_EXTCNF_CTRL_GATE_PHY_CFG);
}

/* e1000_init_phy_workarounds_pchlan */
static void init_phy_workarounds(struct e1000_card *c, enum mac_type t) {
    uint32_t fwsm = er32(c, E1000_FWSM);
    gate_phy_config(c, t, true);
    disable_ulp(c, t);
    if (!acquire_swflag(c)) {
        return;
    }
    bool ok = false;
    switch (t) {
    case B_PCH_MTP:
    case B_PCH_PTP: {
        uint32_t f12 = er32(c, E1000_FEXTNVM12);   /* e1000_reconfigure_k1_params: K1 power down P0s -> P1 */
        ew32(c, E1000_FEXTNVM12, (f12 & ~E1000_FEXTNVM12_PHYPD_CTRL_MASK) | E1000_FEXTNVM12_PHYPD_CTRL_P1);
        udelay(1000);
    }
        /* fall through */
    case B_PCH_LPT:
    case B_PCH_SPT:
    case B_PCH_CNP:
    case B_PCH_TGP:
    case B_PCH_ADP:
        if ((ok = phy_accessible(c, t))) {
            break;
        }
        /* Before toggling LANPHYPC, try with the MAC forced to SMBus. */
        ew32(c, E1000_CTRL_EXT, er32(c, E1000_CTRL_EXT) | E1000_CTRL_EXT_FORCE_SMBUS);
        sched_sleep_ms(50);
        /* fall through */
    case B_PCH2LAN:
        if ((ok = phy_accessible(c, t))) {
            break;
        }
        /* fall through */
    case B_PCHLAN:
        if (t == B_PCHLAN && (fwsm & E1000_ICH_FWSM_FW_VALID)) {
            ok = true;
            break;
        }
        if (reset_blocked(c)) {
            kprintf("[WARN] e1000e: the Management Engine blocks the PHY reset\n");
            break;
        }
        toggle_lanphypc(c, t);
        if (t >= B_PCH_LPT) {
            if ((ok = phy_accessible(c, t))) {
                break;
            }
            ew32(c, E1000_CTRL_EXT, er32(c, E1000_CTRL_EXT) & ~E1000_CTRL_EXT_FORCE_SMBUS);
            ok = phy_accessible(c, t);
        } else {
            ok = true;
        }
        break;
    default:
        ok = true;
        break;
    }
    release_swflag(c);
    if (!ok) {
        kprintf("[WARN] e1000e: the PHY doesn't answer\n");
        return;
    }
    phy_hw_reset(c, t);   /* a known state before anything else touches it */
}

/* e1000_post_phy_reset_ich8lan */
static void post_phy_reset(struct e1000_card *c, enum mac_type t) {
    if (reset_blocked(c)) {
        return;
    }
    sched_sleep_ms(10);
    if (is_pch(t)) {
        uint16_t reg;
        if (hv_read(c, BM_PORT_GEN_CFG, &reg)) {
            hv_write(c, BM_PORT_GEN_CFG, reg & ~BM_WUC_HOST_WU_BIT);   /* clear the host wake-up bit */
        }
    }
    if (t == B_PCH2LAN && !(er32(c, E1000_FWSM) & E1000_ICH_FWSM_FW_VALID)) {
        sched_sleep_ms(10);
        gate_phy_config(c, t, false);
    }
}

/* --- reset and setup ----------------------------------------------------------------------------- */

/* e1000e_disable_pcie_master: no DMA in flight when the MAC resets. */
static void disable_pcie_master(struct e1000_card *c) {
    ew32(c, E1000_CTRL, er32(c, E1000_CTRL) | E1000_CTRL_GIO_MASTER_DISABLE);
    for (int i = 0; i < 800 && (er32(c, E1000_STATUS) & E1000_STATUS_GIO_MASTER_ENABLE); i++) {
        udelay(100);
    }
}

/* e1000e_get_auto_rd_done */
static void auto_rd_done(struct e1000_card *c) {
    for (int i = 0; i < 10 && !(er32(c, E1000_EECD) & E1000_EECD_AUTO_RD); i++) {
        sched_sleep_ms(1);
    }
}

/* e1000_reset_hw_82571 */
static void reset_hw_82571(struct e1000_card *c, enum mac_type t) {
    disable_pcie_master(c);
    ew32(c, E1000_IMC, 0xFFFFFFFF);
    ew32(c, E1000_RCTL, 0);
    ew32(c, E1000_TCTL, er32(c, E1000_TCTL) & ~E1000_TCTL_EN);
    e1e_flush(c);
    sched_sleep_ms(10);
    /* The MDIO must be ours across the reset on the 82573/82574/82583. */
    bool owned = (t == B_82573 || t == B_82574 || t == B_82583) && acquire_swflag(c);
    ew32(c, E1000_CTRL, er32(c, E1000_CTRL) | E1000_CTRL_RST);
    if (owned) {
        release_swflag(c);
    }
    sched_sleep_ms(1);
    auto_rd_done(c);
    if (t == B_82571 || t == B_82572) {
        ew32(c, E1000_EECD, er32(c, E1000_EECD) & ~(E1000_EECD_REQ | E1000_EECD_GNT));
    } else if (t != B_80003ES2LAN) {
        sched_sleep_ms(25);
    }
    ew32(c, E1000_IMC, 0xFFFFFFFF);
    er32(c, E1000_ICR);
}

/* e1000_initialize_hw_bits_82571 + e1000_init_hw_82571 (+ the copper link setup) */
static void init_hw_82571(struct e1000_card *c, enum mac_type t) {
    uint32_t reg = er32(c, E1000_TXDCTL0);
    ew32(c, E1000_TXDCTL0, reg | 1u << 22);
    reg = er32(c, E1000_TXDCTL1);
    ew32(c, E1000_TXDCTL1, reg | 1u << 22);
    reg = er32(c, E1000_TARC0) & ~(0xFu << 27);
    if (t == B_82571 || t == B_82572) {
        reg |= 1u << 23 | 1u << 24 | 1u << 25 | 1u << 26;
    } else if (t == B_82574 || t == B_82583) {
        reg |= 1u << 26;
    }
    ew32(c, E1000_TARC0, reg);
    if (t == B_82571 || t == B_82572) {
        reg = er32(c, E1000_TARC1);
        reg &= ~(1u << 29 | 1u << 30);
        reg |= 1u << 22 | 1u << 24 | 1u << 25 | 1u << 26;
        reg = er32(c, E1000_TCTL) & E1000_TCTL_MULR ? reg & ~(1u << 28) : reg | 1u << 28;
        ew32(c, E1000_TARC1, reg);
        ew32(c, E1000_CTRL_EXT, er32(c, E1000_CTRL_EXT) & ~E1000_CTRL_EXT_DMA_DYN_CLK_EN);
    }
    if (t == B_82573 || t == B_82574 || t == B_82583) {
        ew32(c, E1000_CTRL, er32(c, E1000_CTRL) & ~(1u << 29));
        ew32(c, E1000_CTRL_EXT, (er32(c, E1000_CTRL_EXT) & ~(1u << 23)) | 1u << 22);
    }
    if (t == B_82571) {
        ew32(c, E1000_PBA_ECC, er32(c, E1000_PBA_ECC) | E1000_PBA_ECC_CORR_EN);
    }
    if (t <= B_82573) {
        ew32(c, E1000_RFCTL, er32(c, E1000_RFCTL) | E1000_RFCTL_IPV6_EX_DIS | E1000_RFCTL_NEW_IPV6_EXT_DIS);
    }
    if (t == B_82574 || t == B_82583) {
        ew32(c, E1000_GCR, er32(c, E1000_GCR) | 1u << 22);
        ew32(c, E1000_GCR2, er32(c, E1000_GCR2) | 1);   /* PCIe completion errata */
    }

    for (uint32_t i = 0; i < 128; i++) {
        ew32(c, E1000_VFTA + 4 * i, 0);
    }
    e1000_init_rx_addrs(c, 15, 128);

    /* e1000_setup_copper_link_82571 -> e1000e_setup_copper_link */
    uint32_t ctrl = er32(c, E1000_CTRL);
    ctrl |= E1000_CTRL_SLU;
    ctrl &= ~(E1000_CTRL_FRCSPD | E1000_CTRL_FRCDPX);
    ew32(c, E1000_CTRL, ctrl);
    bool owned = (t == B_82573 || t == B_82574 || t == B_82583) && acquire_swflag(c);
    e1000_phy_autoneg(c);
    if (owned) {
        release_swflag(c);
    }

    reg = er32(c, E1000_TXDCTL0);
    ew32(c, E1000_TXDCTL0, (reg & ~E1000_TXDCTL_WTHRESH) | E1000_TXDCTL_FULL_TX_DESC_WB | E1000_TXDCTL_COUNT_DESC);
    if (t == B_82573 || t == B_82574 || t == B_82583) {
        ew32(c, E1000_GCR, er32(c, E1000_GCR) | E1000_GCR_L1_ACT_WITHOUT_L0S_RX);
    } else {
        reg = er32(c, E1000_TXDCTL1);
        ew32(c, E1000_TXDCTL1, (reg & ~E1000_TXDCTL_WTHRESH) | E1000_TXDCTL_FULL_TX_DESC_WB | E1000_TXDCTL_COUNT_DESC);
    }
}

/* e1000_reset_hw_ich8lan */
static void reset_hw_ich8lan(struct e1000_card *c, enum mac_type t) {
    disable_pcie_master(c);
    ew32(c, E1000_IMC, 0xFFFFFFFF);
    ew32(c, E1000_RCTL, 0);
    ew32(c, E1000_TCTL, E1000_TCTL_PSP);
    e1e_flush(c);
    sched_sleep_ms(10);
    if (t == B_ICH8LAN) {   /* FIFO corruption workaround */
        ew32(c, E1000_PBA, E1000_PBA_8K);
        ew32(c, E1000_PBS, E1000_PBS_16K);
    }
    uint32_t ctrl = er32(c, E1000_CTRL);
    if (!reset_blocked(c)) {
        ctrl |= E1000_CTRL_PHY_RST;   /* the MAC and the PHY together */
        if (t == B_PCH2LAN && !(er32(c, E1000_FWSM) & E1000_ICH_FWSM_FW_VALID)) {
            gate_phy_config(c, t, true);
        }
    }
    acquire_swflag(c);
    ew32(c, E1000_CTRL, ctrl | E1000_CTRL_RST);
    sched_sleep_ms(20);   /* (no flush: it would hang the hardware) */
    if (t == B_PCH2LAN) {
        uint32_t reg = er32(c, E1000_FEXTNVM3);
        ew32(c, E1000_FEXTNVM3, (reg & ~E1000_FEXTNVM3_PHY_CFG_COUNTER_MASK) | E1000_FEXTNVM3_PHY_CFG_COUNTER_50MSEC);
    }
    if (ctrl & E1000_CTRL_PHY_RST) {
        /* e1000_get_cfg_done_ich8lan: then wait for the LAN to finish configuring itself. */
        sched_sleep_ms(10);
        if (t >= B_ICH10LAN) {
            for (int loop = 1500; loop-- && !(er32(c, E1000_STATUS) & E1000_STATUS_LAN_INIT_DONE);) {
                udelay(150);
            }
            ew32(c, E1000_STATUS, er32(c, E1000_STATUS) & ~E1000_STATUS_LAN_INIT_DONE);
        } else {
            auto_rd_done(c);
        }
        if (er32(c, E1000_STATUS) & E1000_STATUS_PHYRA) {
            ew32(c, E1000_STATUS, er32(c, E1000_STATUS) & ~E1000_STATUS_PHYRA);
        }
        post_phy_reset(c, t);
    }
    ew32(c, E1000_IMC, 0xFFFFFFFF);
    er32(c, E1000_ICR);
    ew32(c, E1000_KABGTXD, er32(c, E1000_KABGTXD) | E1000_KABGTXD_BGSQLBIAS);
    if (t >= B_PCH_PTP) {
        ew32(c, E1000_CTRL_EXT, er32(c, E1000_CTRL_EXT) & ~E1000_CTRL_EXT_DPG_EN);
    }
}

/* e1000_initialize_hw_bits_ich8lan + e1000_init_hw_ich8lan + e1000_setup_copper_link_pch_lpt */
static void init_hw_ich8lan(struct e1000_card *c, enum mac_type t) {
    uint32_t reg = er32(c, E1000_CTRL_EXT) | 1u << 22;
    if (is_pch(t)) {
        reg |= E1000_CTRL_EXT_PHYPDEN;
    }
    ew32(c, E1000_CTRL_EXT, reg);
    ew32(c, E1000_TXDCTL0, er32(c, E1000_TXDCTL0) | 1u << 22);
    ew32(c, E1000_TXDCTL1, er32(c, E1000_TXDCTL1) | 1u << 22);
    reg = er32(c, E1000_TARC0);
    if (t == B_ICH8LAN) {
        reg |= 1u << 28 | 1u << 29;
    }
    ew32(c, E1000_TARC0, reg | 1u << 23 | 1u << 24 | 1u << 26 | 1u << 27);
    reg = er32(c, E1000_TARC1);
    reg = er32(c, E1000_TCTL) & E1000_TCTL_MULR ? reg & ~(1u << 28) : reg | 1u << 28;
    ew32(c, E1000_TARC1, reg | 1u << 24 | 1u << 26 | 1u << 30);
    if (t == B_ICH8LAN) {
        ew32(c, E1000_STATUS, er32(c, E1000_STATUS) & ~(1u << 31));
    }
    reg = er32(c, E1000_RFCTL) | E1000_RFCTL_NFSW_DIS | E1000_RFCTL_NFSR_DIS;
    if (t == B_ICH8LAN) {
        reg |= E1000_RFCTL_IPV6_EX_DIS | E1000_RFCTL_NEW_IPV6_EXT_DIS;
    }
    ew32(c, E1000_RFCTL, reg);
    if (t >= B_PCH_LPT) {
        ew32(c, E1000_PBECCSTS, er32(c, E1000_PBECCSTS) | E1000_PBECCSTS_ECC_ENABLE);
        ew32(c, E1000_CTRL, er32(c, E1000_CTRL) | E1000_CTRL_MEHE);
    }

    /* Receive address 0 only: the shared ones above it may be locked by the ME. */
    e1000_init_rx_addrs(c, 1, 32);

    if (!reset_blocked(c)) {
        uint32_t ctrl = er32(c, E1000_CTRL);
        ctrl |= E1000_CTRL_SLU;
        ctrl &= ~(E1000_CTRL_FRCSPD | E1000_CTRL_FRCDPX);
        ew32(c, E1000_CTRL, ctrl);
        if (acquire_swflag(c)) {
            c->phy_addr = is_pch(t) ? 2 : 1;
            e1000_phy_autoneg(c);
            release_swflag(c);
        }
    }

    reg = er32(c, E1000_TXDCTL0);
    reg = (reg & ~E1000_TXDCTL_WTHRESH) | E1000_TXDCTL_FULL_TX_DESC_WB;
    reg = (reg & ~E1000_TXDCTL_PTHRESH) | E1000_TXDCTL_MAX_TX_DESC_PREFETCH;
    ew32(c, E1000_TXDCTL0, reg);
    ew32(c, E1000_TXDCTL1, reg);
    /* Snoop (cache-coherent DMA): ICH8 has the bits the other way round. */
    reg = er32(c, E1000_GCR) & ~PCIE_NO_SNOOP_ALL;
    if (t == B_ICH8LAN) {
        reg |= PCIE_NO_SNOOP_ALL;
    }
    ew32(c, E1000_GCR, reg);
    if (t >= B_PCH_TGP) {
        ew32(c, E1000_FFLT_DBG, er32(c, E1000_FFLT_DBG) | E1000_FFLT_DBG_DONT_GATE_WAKE_DMA_CLK);
    }
    ew32(c, E1000_CTRL_EXT, er32(c, E1000_CTRL_EXT) | E1000_CTRL_EXT_RO_DIS);
    if (t == B_PCH_SPT) {
        /* e1000_configure_tx: SPT/KBL errata (data corruption, Tx hang). */
        ew32(c, E1000_IOSFPC, er32(c, E1000_IOSFPC) | E1000_RCTL_RDMTS_HEX);
        reg = er32(c, E1000_TARC0) & ~E1000_TARC0_CB_MULTIQ_3_REQ;
        ew32(c, E1000_TARC0, reg | E1000_TARC0_CB_MULTIQ_2_REQ);
    }
}

static void probe_one(const struct pci_device *d, enum mac_type t, const char *name) {
    if (card_count == MAX_CARDS) {
        return;
    }
    struct e1000_card *c = &cards[card_count];
    memset(c, 0, sizeof(*c));
    if (!e1000_map(c, d)) {
        kprintf("[WARN] e1000e: %02x:%02x.%u has no register window\n", d->bus, d->slot, d->function);
        return;
    }
    c->name = name;
    c->phy_addr = 1;
    c->collision_distance = E1000_COLLISION_DISTANCE;
    card_type[card_count] = t;

    if (is_pch(t)) {
        init_phy_workarounds(c, t);
    }
    if (is_ich(t)) {
        reset_hw_ich8lan(c, t);
    } else {
        reset_hw_82571(c, t);
    }
    /* e1000_read_mac_addr_generic: the hardware has loaded receive address 0 from the NVM. */
    if (!e1000_read_mac_rar(c)) {
        uint16_t w[3];
        if (!is_ich(t) && e1000_read_eerd(c, 0, &w[0]) && e1000_read_eerd(c, 1, &w[1]) && e1000_read_eerd(c, 2, &w[2])) {
            uint8_t m[6] = { (uint8_t)w[0], (uint8_t)(w[0] >> 8), (uint8_t)w[1], (uint8_t)(w[1] >> 8),
                             (uint8_t)w[2], (uint8_t)(w[2] >> 8) };
            memcpy(c->dev.mac, m, 6);
        } else {
            kprintf("[WARN] e1000e: %s has no MAC address\n", name);
            return;
        }
    }
    if (is_ich(t)) {
        init_hw_ich8lan(c, t);
    } else {
        init_hw_82571(c, t);
    }
    card_count++;
    /* MSI, not MSI-X: with MSI-X the 82574 routes causes through IVAR, which we don't set up. */
    e1000_start(c, "e1000e", false);
}

void e1000e_probe(void) {
    struct pci_device d;
    for (size_t k = 0; k < sizeof(ids) / sizeof(ids[0]); k++) {
        for (int i = 0; pci_find_id(0x8086, ids[k].device, i, &d); i++) {
            probe_one(&d, ids[k].type, ids[k].name);
        }
    }
}
