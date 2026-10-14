/*
 * SATA disks on AHCI controllers (PCI class 01:06:01), after linux/drivers/ata/ahci.c, libahci.c and
 * libata-core.c, cut down to what a disk needs: each port with an ATA disk on it gets one command
 * slot, used by polling (no interrupts), with READ/WRITE DMA EXT and FLUSH CACHE EXT. Disks are
 * "sata0", "sata1", ... ATAPI (optical) drives are left alone.
 *
 * Transfers go through a physically contiguous bounce buffer per port (block I/O hands us ordinary
 * heap memory); a mutex keeps one command in flight per port.
 */

#include "drivers/ahci.h"
#include "block/block.h"
#include "drivers/pci.h"
#include "drivers/timer.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include "sched/sync.h"

#define MAX_CONTROLLERS 4
#define MAX_PORTS       32
#define BOUNCE_PAGES    32               /* 128 KiB per command */
#define PRD_ENTRIES     8

/* Host registers (libahci.h: HOST_*) */
#define HOST_CAP        0x00
#define HOST_CTL        0x04
#define HOST_PORTS_IMPL 0x0C
#define HOST_VERSION    0x10
#define HOST_CAP2       0x24
#define HOST_BOHC       0x28
#define HOST_AHCI_EN    (1u << 31)
#define HOST_IRQ_EN     (1u << 1)
#define CAP_S64A        (1u << 31)
#define CAP2_BOH        (1u << 0)
#define BOHC_BOS        (1u << 0)
#define BOHC_OOS        (1u << 1)
#define BOHC_BB         (1u << 4)

/* Port registers, at 0x100 + port * 0x80 (PORT_*) */
#define PORT_LST_ADDR   0x00
#define PORT_FIS_ADDR   0x08
#define PORT_IRQ_STAT   0x10
#define PORT_IRQ_MASK   0x14
#define PORT_CMD        0x18
#define PORT_TFDATA     0x20
#define PORT_SIG        0x24
#define PORT_SCR_STAT   0x28
#define PORT_SCR_CTL    0x2C
#define PORT_SCR_ERR    0x30
#define PORT_CMD_ISSUE  0x38

#define CMD_START       (1u << 0)
#define CMD_SPIN_UP     (1u << 1)
#define CMD_POWER_ON    (1u << 2)
#define CMD_FIS_RX      (1u << 4)
#define CMD_FIS_ON      (1u << 14)
#define CMD_LIST_ON     (1u << 15)
#define CMD_ICC_ACTIVE  (1u << 28)

#define IRQ_TF_ERR      (1u << 30)       /* task file error */
#define IRQ_HBUS_ERR    (1u << 29)
#define IRQ_HBUS_DATA   (1u << 28)
#define IRQ_IF_ERR      (1u << 27)
#define IRQ_ERRORS      (IRQ_TF_ERR | IRQ_HBUS_ERR | IRQ_HBUS_DATA | IRQ_IF_ERR)

#define ATA_BUSY        0x80
#define ATA_DRQ         0x08
#define ATA_ERR         0x01

#define SIG_ATA         0x00000101
#define SIG_ATAPI       0xEB140101

/* ATA commands (ata.h) */
#define ATA_CMD_ID_ATA         0xEC
#define ATA_CMD_READ_EXT       0x25
#define ATA_CMD_WRITE_EXT      0x35
#define ATA_CMD_FLUSH_EXT      0xEA

struct cmd_header {                      /* struct ahci_cmd_hdr */
    uint32_t opts;                       /* FIS length in dwords, W, PRDT length << 16 */
    uint32_t status;                     /* bytes transferred */
    uint64_t tbl_addr;
    uint32_t reserved[4];
};

struct prd {                             /* struct ahci_sg */
    uint64_t addr;
    uint32_t reserved;
    uint32_t flags_size;                 /* bytes - 1 */
};

struct cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct prd prd[PRD_ENTRIES];
};

struct port {
    struct controller *hc;
    int index;
    volatile uint8_t *regs;
    struct cmd_header *list;             /* 32 headers; we use slot 0 */
    struct cmd_table *table;
    uint64_t table_phys;
    uint8_t *bounce;
    uint64_t bounce_phys;
    struct mutex lock;
    bool lba48;
    struct block_device bdev;
};

struct controller {
    struct pci_device pci;
    volatile uint8_t *regs;
    bool s64a;
};

static struct controller controllers[MAX_CONTROLLERS];
static struct port *ports[MAX_CONTROLLERS * MAX_PORTS];
static int controller_count, port_count, disk_number;

static inline uint32_t hrd(struct controller *c, uint32_t off) {
    return *(volatile uint32_t *)(c->regs + off);
}
static inline void hwr(struct controller *c, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(c->regs + off) = v;
}
static inline uint32_t prd32(struct port *p, uint32_t off) {
    return *(volatile uint32_t *)(p->regs + off);
}
static inline void pwr32(struct port *p, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(p->regs + off) = v;
}

static uint64_t dma_alloc(struct controller *c, int pages, void **virt) {
    uint64_t phys = pmm_alloc_contiguous((uint64_t)pages, c->s64a ? ~0ull : 0x100000000ull);
    if (phys) {
        *virt = phys_to_virt(phys);
        memset(*virt, 0, (size_t)pages * 4096);
    }
    return phys;
}

static bool wait_clear(struct port *p, uint32_t off, uint32_t mask, uint32_t ms) {
    uint64_t end = uptime_ms() + ms;
    while (prd32(p, off) & mask) {
        if (uptime_ms() > end) {
            return false;
        }
        sched_sleep_ms(1);
    }
    return true;
}

/* ahci_stop_engine + stopping FIS reception. */
static bool port_stop(struct port *p) {
    uint32_t cmd = prd32(p, PORT_CMD);
    if (cmd & (CMD_START | CMD_LIST_ON)) {
        pwr32(p, PORT_CMD, cmd & ~CMD_START);
        if (!wait_clear(p, PORT_CMD, CMD_LIST_ON, 500)) {
            return false;
        }
    }
    cmd = prd32(p, PORT_CMD);
    if (cmd & (CMD_FIS_RX | CMD_FIS_ON)) {
        pwr32(p, PORT_CMD, cmd & ~CMD_FIS_RX);
        if (!wait_clear(p, PORT_CMD, CMD_FIS_ON, 500)) {
            return false;
        }
    }
    return true;
}

/* ahci_start_fis_rx + ahci_start_engine. */
static void port_start(struct port *p) {
    pwr32(p, PORT_SCR_ERR, 0xFFFFFFFF);
    pwr32(p, PORT_IRQ_STAT, 0xFFFFFFFF);
    pwr32(p, PORT_CMD, prd32(p, PORT_CMD) | CMD_FIS_RX);
    pwr32(p, PORT_CMD, prd32(p, PORT_CMD) | CMD_START);
}

/* After an error: restart the command engine (ahci_kick_engine, without the port reset). */
static void port_recover(struct port *p) {
    port_stop(p);
    port_start(p);
}

/* --- commands ------------------------------------------------------------------------------------------- */

/* Run an ATA command in slot 0: `bytes` of data to/from the bounce buffer. */
static int command(struct port *p, uint8_t cmd, uint64_t lba, uint32_t count, uint32_t bytes, bool write) {
    struct cmd_table *t = p->table;
    memset(t, 0, sizeof(*t));
    uint8_t *f = t->cfis;                /* register host-to-device FIS (ata_tf_to_fis) */
    f[0] = 0x27;
    f[1] = 0x80;                         /* a command */
    f[2] = cmd;
    f[4] = (uint8_t)lba;
    f[5] = (uint8_t)(lba >> 8);
    f[6] = (uint8_t)(lba >> 16);
    f[7] = 0x40;                         /* LBA mode */
    f[8] = (uint8_t)(lba >> 24);
    f[9] = (uint8_t)(lba >> 32);
    f[10] = (uint8_t)(lba >> 40);
    f[12] = (uint8_t)count;
    f[13] = (uint8_t)(count >> 8);
    int prds = 0;
    for (uint32_t done = 0; done < bytes && prds < PRD_ENTRIES; prds++) {
        uint32_t n = bytes - done < 0x400000 ? bytes - done : 0x400000;
        t->prd[prds].addr = p->bounce_phys + done;
        t->prd[prds].flags_size = n - 1;
        done += n;
    }
    struct cmd_header *h = &p->list[0];
    h->opts = 5u | (write ? 1u << 6 : 0) | (uint32_t)prds << 16;
    h->status = 0;
    h->tbl_addr = p->table_phys;
    __asm__ volatile("mfence" ::: "memory");

    if (!wait_clear(p, PORT_TFDATA, ATA_BUSY | ATA_DRQ, 1000)) {
        port_recover(p);
    }
    pwr32(p, PORT_IRQ_STAT, 0xFFFFFFFF);
    pwr32(p, PORT_CMD_ISSUE, 1);
    uint64_t start = uptime_ms();
    for (uint64_t spins = 0; prd32(p, PORT_CMD_ISSUE) & 1; spins++) {
        if (prd32(p, PORT_IRQ_STAT) & IRQ_ERRORS) {
            break;
        }
        if (spins > 2000) {
            if (uptime_ms() - start > 30000) {
                kprintf("[WARN] ahci: %s: command 0x%x timed out\n", p->bdev.name, cmd);
                port_recover(p);
                return -EIO;
            }
            sched_yield();
        } else {
            __asm__ volatile("pause");
        }
    }
    uint32_t is = prd32(p, PORT_IRQ_STAT), tfd = prd32(p, PORT_TFDATA);
    if ((is & IRQ_ERRORS) || (tfd & ATA_ERR)) {
        kprintf("[WARN] ahci: %s: command 0x%x at %lu failed (status 0x%x, error 0x%x)\n", p->bdev.name, cmd, lba,
                tfd & 0xFF, (tfd >> 8) & 0xFF);
        port_recover(p);
        return -EIO;
    }
    return 0;
}

static int rw(struct block_device *d, uint64_t lba, uint32_t count, void *buf, bool write) {
    struct port *p = d->priv;
    uint32_t per = BOUNCE_PAGES * 4096 / d->sector_size;
    if (per > 65536) {
        per = 65536;
    }
    mutex_lock(&p->lock);
    int err = 0;
    while (count && !err) {
        uint32_t n = count < per ? count : per;
        uint32_t bytes = n * d->sector_size;
        if (write) {
            memcpy(p->bounce, buf, bytes);
        }
        err = command(p, write ? ATA_CMD_WRITE_EXT : ATA_CMD_READ_EXT, lba, n == 65536 ? 0 : n, bytes, write);
        if (!err && !write) {
            memcpy(buf, p->bounce, bytes);
        }
        lba += n;
        count -= n;
        buf = (uint8_t *)buf + bytes;
    }
    mutex_unlock(&p->lock);
    return err;
}

static int ahci_read(struct block_device *d, uint64_t lba, uint32_t count, void *buf) {
    return rw(d, lba, count, buf, false);
}

static int ahci_write(struct block_device *d, uint64_t lba, uint32_t count, const void *buf) {
    return rw(d, lba, count, (void *)buf, true);
}

static int ahci_flush(struct block_device *d) {
    struct port *p = d->priv;
    mutex_lock(&p->lock);
    int err = command(p, ATA_CMD_FLUSH_EXT, 0, 0, 0, false);
    mutex_unlock(&p->lock);
    return err;
}

/* --- finding the disks ------------------------------------------------------------------------------------- */

/* ata_id_string: IDENTIFY strings are 16-bit words with the bytes swapped. */
static void id_string(char *out, size_t size, const uint16_t *id, int word, int words) {
    char raw[96];
    int n = 0;
    for (int i = 0; i < words && n + 2 < (int)sizeof(raw); i++) {
        raw[n++] = (char)(id[word + i] >> 8);
        raw[n++] = (char)(id[word + i] & 0xFF);
    }
    while (n && (raw[n - 1] == ' ' || raw[n - 1] == 0)) {
        n--;
    }
    int s = 0;
    while (s < n && raw[s] == ' ') {
        s++;
    }
    int o = 0;
    for (int i = s; i < n && (size_t)o + 1 < size; i++) {
        out[o++] = raw[i] >= 0x20 && raw[i] < 0x7F ? raw[i] : '?';
    }
    out[o] = '\0';
}

static bool port_setup(struct controller *c, int index) {
    volatile uint8_t *regs = c->regs + 0x100 + index * 0x80;
    uint32_t ssts = *(volatile uint32_t *)(regs + PORT_SCR_STAT);
    if ((ssts & 0xF) != 3) {
        return false;                    /* nothing there (or no link) */
    }
    uint32_t sig = *(volatile uint32_t *)(regs + PORT_SIG);
    if (sig != SIG_ATA) {
        return false;                    /* ATAPI, port multiplier, enclosure... */
    }
    struct port *p = kzalloc(sizeof(*p));
    if (!p) {
        return false;
    }
    p->hc = c;
    p->index = index;
    p->regs = regs;
    p->lock = (struct mutex)MUTEX_INIT;
    if (!port_stop(p)) {
        kprintf("[WARN] ahci: port %d won't stop\n", index);
        kfree(p);
        return false;
    }
    /* Command list (1 KiB) and received FISes (256 bytes) share a page; the table gets its own. */
    void *page = NULL, *table = NULL, *bounce = NULL;
    uint64_t page_phys = dma_alloc(c, 1, &page);
    p->table_phys = dma_alloc(c, 1, &table);
    p->bounce_phys = dma_alloc(c, BOUNCE_PAGES, &bounce);
    if (!page_phys || !p->table_phys || !p->bounce_phys) {
        kfree(p);
        return false;
    }
    p->list = page;
    p->table = table;
    p->bounce = bounce;
    pwr32(p, PORT_LST_ADDR, (uint32_t)page_phys);
    pwr32(p, PORT_LST_ADDR + 4, (uint32_t)(page_phys >> 32));
    pwr32(p, PORT_FIS_ADDR, (uint32_t)(page_phys + 1024));
    pwr32(p, PORT_FIS_ADDR + 4, (uint32_t)((page_phys + 1024) >> 32));
    pwr32(p, PORT_IRQ_MASK, 0);          /* we poll */
    pwr32(p, PORT_CMD, prd32(p, PORT_CMD) | CMD_SPIN_UP | CMD_POWER_ON);
    port_start(p);

    /* IDENTIFY DEVICE: 512 bytes of words. */
    if (command(p, ATA_CMD_ID_ATA, 0, 0, 512, false) != 0) {
        kprintf("[WARN] ahci: port %d: IDENTIFY failed\n", index);
        return false;
    }
    const uint16_t *id = (const uint16_t *)p->bounce;
    struct block_device *d = &p->bdev;
    id_string(d->model, sizeof(d->model), id, 27, 20);
    id_string(d->serial, sizeof(d->serial), id, 10, 10);
    p->lba48 = id[83] & (1u << 10);
    uint64_t sectors = p->lba48 ? (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 |
                                      (uint64_t)id[103] << 48
                                : (uint64_t)id[60] | (uint64_t)id[61] << 16;
    uint32_t sector_size = 512;
    /* ata_id_logical_sector_size: words 117-118 when word 106 says the logical sector is bigger. */
    if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1u << 12))) {
        sector_size = ((uint32_t)id[117] | (uint32_t)id[118] << 16) * 2;
    }
    if (!p->lba48 || !sectors || (sector_size != 512 && sector_size != 4096)) {
        kprintf("[WARN] ahci: port %d: %s isn't a disk we can use\n", index, d->model);
        return false;
    }
    ksnprintf(d->name, sizeof(d->name), "sata%d", disk_number++);
    d->sector_size = sector_size;
    d->sectors = sectors;
    d->read = ahci_read;
    d->write = ahci_write;
    d->flush = ahci_flush;
    d->priv = p;
    ports[port_count++] = p;
    block_add_disk(d);
    return true;
}

/* bios_handoff (ahci_platform/libahci: BOHC). */
static void take_from_firmware(struct controller *c) {
    if (!(hrd(c, HOST_CAP2) & CAP2_BOH)) {
        return;
    }
    hwr(c, HOST_BOHC, hrd(c, HOST_BOHC) | BOHC_OOS);
    uint64_t end = uptime_ms() + 2000;
    while ((hrd(c, HOST_BOHC) & BOHC_BOS) && uptime_ms() < end) {
        sched_sleep_ms(10);
    }
    if (hrd(c, HOST_BOHC) & BOHC_BB) {
        sched_sleep_ms(2000);            /* the firmware said it's busy: give it the time the spec says */
    }
}

static void start_controller(struct controller *c) {
    uint64_t bar = pci_bar_address(&c->pci, 5);
    if (!bar) {
        return;
    }
    pci_write16(&c->pci, PCI_COMMAND, (uint16_t)(pci_read16(&c->pci, PCI_COMMAND) | PCI_COMMAND_MEMORY |
                                                 PCI_COMMAND_MASTER | PCI_COMMAND_INTX_OFF));
    c->regs = vmm_map_mmio(bar, 0x1100);
    if (!c->regs) {
        return;
    }
    take_from_firmware(c);
    hwr(c, HOST_CTL, (hrd(c, HOST_CTL) | HOST_AHCI_EN) & ~HOST_IRQ_EN);
    uint32_t cap = hrd(c, HOST_CAP), implemented = hrd(c, HOST_PORTS_IMPL);
    c->s64a = cap & CAP_S64A;
    uint32_t version = hrd(c, HOST_VERSION);
    kprintf("[ OK ] AHCI %04x:%04x at %02x:%02x.%x: version %x.%x, %d ports\n", c->pci.vendor, c->pci.device,
            c->pci.bus, c->pci.slot, c->pci.function, version >> 16, (version >> 8) & 0xFF,
            (int)(cap & 0x1F) + 1);
    for (int i = 0; i < MAX_PORTS; i++) {
        if (implemented & (1u << i)) {
            port_setup(c, i);
        }
    }
}

void ahci_init(void) {
    pci_init();
    struct pci_device d;
    for (int i = 0; pci_find_class(0x01, 0x06, i, &d) && controller_count < MAX_CONTROLLERS; i++) {
        if (d.prog_if != 0x01) {
            continue;
        }
        struct controller *c = &controllers[controller_count++];
        memset(c, 0, sizeof(*c));
        c->pci = d;
        start_controller(c);
    }
}

void ahci_shutdown(void) {
    for (int i = 0; i < port_count; i++) {
        ahci_flush(&ports[i]->bdev);
    }
}
