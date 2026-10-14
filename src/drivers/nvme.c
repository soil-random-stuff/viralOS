/*
 * NVMe SSDs (PCI class 01:08:02), after linux/drivers/nvme/host/pci.c and core.c, cut down to what
 * a disk needs: one admin queue and one I/O queue pair per controller, polled (no interrupts), and
 * every namespace registered as a disk ("nvme0", "nvme1", ...).
 *
 * Transfers go through a physically contiguous bounce buffer per controller (block I/O hands us
 * ordinary heap memory), whose PRP list is built once; a mutex keeps one command in flight.
 */

#include "drivers/nvme.h"
#include "block/block.h"
#include "drivers/pci.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "drivers/timer.h"
#include "sched/sched.h"
#include "sched/sync.h"

#define MAX_CONTROLLERS 4
#define MAX_NAMESPACES  4
#define QUEUE_DEPTH     32
#define BOUNCE_PAGES    32                     /* 128 KiB per command at most */

/* Controller registers (nvme.h: NVME_REG_*) */
#define REG_CAP   0x00
#define REG_VS    0x08
#define REG_INTMS 0x0C
#define REG_CC    0x14
#define REG_CSTS  0x1C
#define REG_AQA   0x24
#define REG_ASQ   0x28
#define REG_ACQ   0x30
#define REG_DBS   0x1000

#define CC_ENABLE      1u
#define CC_CSS_NVM     (0u << 4)
#define CC_MPS_4K      (0u << 7)
#define CC_AMS_RR      (0u << 11)
#define CC_SHN_NORMAL  (1u << 14)
#define CC_IOSQES      (6u << 16)               /* 64-byte submission entries */
#define CC_IOCQES      (4u << 20)               /* 16-byte completion entries */
#define CSTS_RDY       1u
#define CSTS_CFS       2u
#define CSTS_SHST_MASK (3u << 2)
#define CSTS_SHST_DONE (2u << 2)

/* Opcodes */
#define ADMIN_CREATE_SQ 0x01
#define ADMIN_CREATE_CQ 0x05
#define ADMIN_IDENTIFY  0x06
#define CMD_FLUSH       0x00
#define CMD_WRITE       0x01
#define CMD_READ        0x02

struct sqe {                                   /* struct nvme_common_command */
    uint8_t opcode, flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsvd;
    uint64_t metadata;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};

struct cqe {                                   /* struct nvme_completion */
    uint32_t result, rsvd;
    uint16_t sq_head, sq_id;
    uint16_t cid;
    uint16_t status;                           /* bit 0: phase; 15:1 the status */
};

struct queue {
    struct sqe *sq;
    volatile struct cqe *cq;
    uint64_t sq_phys, cq_phys;
    uint16_t qid, sq_tail, cq_head;
    uint8_t phase;
    uint16_t cid;
};

struct controller {
    struct pci_device pci;
    volatile uint8_t *regs;
    uint32_t stride;                           /* doorbell stride in bytes */
    uint32_t timeout_ms;                       /* CAP.TO */
    struct queue admin, io;
    struct mutex lock;
    uint8_t *bounce;
    uint64_t bounce_phys;
    uint64_t *prp_list;                        /* entries for bounce pages 1.. */
    uint64_t prp_list_phys;
    uint32_t max_bytes;                        /* per command (MDTS, the bounce buffer) */
    char model[48], serial[40];
    bool dead;
};

struct namespace {
    struct controller *c;
    uint32_t nsid;
    struct block_device bdev;
};

static struct controller controllers[MAX_CONTROLLERS];
static int controller_count;
static int disk_number;

static inline uint32_t rd32(struct controller *c, uint32_t off) {
    return *(volatile uint32_t *)(c->regs + off);
}
static inline void wr32(struct controller *c, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(c->regs + off) = v;
}
static inline uint64_t rd64(struct controller *c, uint32_t off) {
    return rd32(c, off) | (uint64_t)rd32(c, off + 4) << 32;
}
static inline void wr64(struct controller *c, uint32_t off, uint64_t v) {
    wr32(c, off, (uint32_t)v);
    wr32(c, off + 4, (uint32_t)(v >> 32));
}

static uint64_t dma_alloc(int pages, void **virt) {
    uint64_t phys = pmm_alloc_contiguous((uint64_t)pages, ~0ull);
    if (phys) {
        *virt = phys_to_virt(phys);
        memset(*virt, 0, (size_t)pages * 4096);
    }
    return phys;
}

/* Wait for CSTS.RDY to become `ready` (nvme_wait_ready). */
static bool wait_ready(struct controller *c, bool ready) {
    uint64_t end = uptime_ms() + c->timeout_ms;
    for (;;) {
        uint32_t csts = rd32(c, REG_CSTS);
        if (csts == 0xFFFFFFFF) {
            return false;   /* gone from the bus */
        }
        if (((csts & CSTS_RDY) != 0) == ready) {
            return true;
        }
        if (uptime_ms() > end) {
            return false;
        }
        sched_sleep_ms(1);
    }
}

/* --- commands -------------------------------------------------------------------------------------------- */

/* Submit one command and poll for its completion; returns the status field (0 = success) or -errno. */
static int submit(struct controller *c, struct queue *q, struct sqe *cmd, uint32_t *result) {
    if (c->dead) {
        return -EIO;
    }
    cmd->cid = q->cid++;
    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (uint16_t)((q->sq_tail + 1) % QUEUE_DEPTH);
    __asm__ volatile("mfence" ::: "memory");
    wr32(c, REG_DBS + (2 * q->qid) * c->stride, q->sq_tail);

    /* Completions come in microseconds: spin a little, then let others run while we wait. */
    uint64_t start = uptime_ms();
    for (uint64_t spins = 0;; spins++) {
        volatile struct cqe *e = &q->cq[q->cq_head];
        if ((e->status & 1) == q->phase) {
            uint16_t status = e->status >> 1;
            if (result) {
                *result = e->result;
            }
            q->cq_head = (uint16_t)((q->cq_head + 1) % QUEUE_DEPTH);
            if (q->cq_head == 0) {
                q->phase ^= 1;
            }
            wr32(c, REG_DBS + (2 * q->qid + 1) * c->stride, q->cq_head);
            return status & 0x7FF;   /* status code type + code */
        }
        if (spins > 2000) {
            if (rd32(c, REG_CSTS) & CSTS_CFS || rd32(c, REG_CSTS) == 0xFFFFFFFF ||
                uptime_ms() - start > 30000) {
                kprintf("[WARN] nvme: a command timed out (controller %s)\n",
                        rd32(c, REG_CSTS) & CSTS_CFS ? "failed" : "stopped answering");
                c->dead = true;
                return -EIO;
            }
            sched_yield();
        } else {
            __asm__ volatile("pause");
        }
    }
}

static bool queue_alloc(struct queue *q, uint16_t qid) {
    void *sq = NULL, *cq = NULL;
    q->sq_phys = dma_alloc(1, &sq);
    q->cq_phys = dma_alloc(1, &cq);
    if (!q->sq_phys || !q->cq_phys) {
        return false;
    }
    q->sq = sq;
    q->cq = cq;
    q->qid = qid;
    q->phase = 1;
    return true;
}

/* An admin command with one page of data at `phys`. */
static int admin(struct controller *c, uint8_t opcode, uint32_t nsid, uint64_t phys, uint32_t cdw10, uint32_t cdw11,
                 uint32_t *result) {
    struct sqe cmd = { .opcode = opcode, .nsid = nsid, .prp1 = phys, .cdw10 = cdw10, .cdw11 = cdw11 };
    return submit(c, &c->admin, &cmd, result);
}

/* Read/write `count` sectors at `lba` through the bounce buffer. */
static int io(struct controller *c, uint32_t nsid, uint8_t opcode, uint64_t lba, uint32_t count, uint32_t bytes) {
    struct sqe cmd = { .opcode = opcode, .nsid = nsid, .prp1 = c->bounce_phys };
    uint32_t pages = (bytes + 4095) / 4096;
    if (pages == 2) {
        cmd.prp2 = c->bounce_phys + 4096;
    } else if (pages > 2) {
        cmd.prp2 = c->prp_list_phys;   /* entries for pages 1.. of the bounce buffer */
    }
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = count - 1;
    int status = submit(c, &c->io, &cmd, NULL);
    if (status > 0) {
        kprintf("[WARN] nvme: %s of %u sectors at %lu failed (status 0x%x)\n", opcode == CMD_READ ? "read" : "write",
                count, lba, status);
        return -EIO;
    }
    return status;
}

/* --- the block device -------------------------------------------------------------------------------------- */

static int rw(struct block_device *d, uint64_t lba, uint32_t count, void *buf, bool write) {
    struct namespace *ns = d->priv;
    struct controller *c = ns->c;
    uint32_t per = c->max_bytes / d->sector_size;
    mutex_lock(&c->lock);
    int err = 0;
    while (count && !err) {
        uint32_t n = count < per ? count : per;
        uint32_t bytes = n * d->sector_size;
        if (write) {
            memcpy(c->bounce, buf, bytes);
        }
        err = io(c, ns->nsid, write ? CMD_WRITE : CMD_READ, lba, n, bytes);
        if (!err && !write) {
            memcpy(buf, c->bounce, bytes);
        }
        lba += n;
        count -= n;
        buf = (uint8_t *)buf + bytes;
    }
    mutex_unlock(&c->lock);
    return err;
}

static int nvme_read(struct block_device *d, uint64_t lba, uint32_t count, void *buf) {
    return rw(d, lba, count, buf, false);
}

static int nvme_write(struct block_device *d, uint64_t lba, uint32_t count, const void *buf) {
    return rw(d, lba, count, (void *)buf, true);
}

static int nvme_flush(struct block_device *d) {
    struct namespace *ns = d->priv;
    mutex_lock(&ns->c->lock);
    struct sqe cmd = { .opcode = CMD_FLUSH, .nsid = ns->nsid };
    int status = submit(ns->c, &ns->c->io, &cmd, NULL);
    mutex_unlock(&ns->c->lock);
    return status ? -EIO : 0;
}

/* --- bringing a controller up (nvme_pci_enable, nvme_pci_configure_admin_queue, nvme_setup_io_queues) --- */

static void ascii_copy(char *out, size_t size, const uint8_t *in, int len) {
    while (len && (in[len - 1] == ' ' || in[len - 1] == 0)) {
        len--;
    }
    int o = 0;
    for (int i = 0; i < len && (size_t)o + 1 < size; i++) {
        out[o++] = in[i] >= 0x20 && in[i] < 0x7F ? (char)in[i] : '?';
    }
    out[o] = '\0';
}

static bool start(struct controller *c) {
    uint64_t bar = pci_bar_address(&c->pci, 0);
    if (!bar) {
        return false;
    }
    pci_write16(&c->pci, PCI_COMMAND, (uint16_t)(pci_read16(&c->pci, PCI_COMMAND) | PCI_COMMAND_MEMORY |
                                                 PCI_COMMAND_MASTER | PCI_COMMAND_INTX_OFF));
    c->regs = vmm_map_mmio(bar, 0x2000);
    if (!c->regs) {
        return false;
    }
    uint64_t cap = rd64(c, REG_CAP);
    if (cap == ~0ull) {
        return false;
    }
    c->stride = 4u << ((cap >> 32) & 0xF);
    c->timeout_ms = (uint32_t)(((cap >> 24) & 0xFF) + 1) * 500;
    uint32_t mqes = (uint32_t)(cap & 0xFFFF) + 1;
    if (mqes < QUEUE_DEPTH || ((cap >> 48) & 0xF) > 0) {   /* tiny queues, or 4 KiB pages unsupported */
        kprintf("[WARN] nvme: controller needs queues/pages we don't do\n");
        return false;
    }
    if (REG_DBS + 4 * c->stride > 0x2000) {
        c->regs = vmm_map_mmio(bar, REG_DBS + 4 * c->stride);
    }

    /* Disable, set up the admin queue, enable. */
    if (rd32(c, REG_CC) & CC_ENABLE) {
        wr32(c, REG_CC, rd32(c, REG_CC) & ~CC_ENABLE);
    }
    if (!wait_ready(c, false)) {
        kprintf("[WARN] nvme: controller didn't stop\n");
        return false;
    }
    if (!queue_alloc(&c->admin, 0) || !queue_alloc(&c->io, 1)) {
        return false;
    }
    wr32(c, REG_AQA, (QUEUE_DEPTH - 1) << 16 | (QUEUE_DEPTH - 1));
    wr64(c, REG_ASQ, c->admin.sq_phys);
    wr64(c, REG_ACQ, c->admin.cq_phys);
    wr32(c, REG_INTMS, 0xFFFFFFFF);   /* we poll */
    wr32(c, REG_CC, CC_CSS_NVM | CC_MPS_4K | CC_AMS_RR | CC_IOSQES | CC_IOCQES | CC_ENABLE);
    if (!wait_ready(c, true)) {
        kprintf("[WARN] nvme: controller didn't start (CSTS 0x%x)\n", rd32(c, REG_CSTS));
        return false;
    }

    /* Identify the controller: names, the transfer limit, how many namespaces. */
    void *page = NULL;
    uint64_t page_phys = dma_alloc(1, &page);
    if (!page_phys || admin(c, ADMIN_IDENTIFY, 0, page_phys, 1, 0, NULL) != 0) {
        kprintf("[WARN] nvme: identify failed\n");
        return false;
    }
    const uint8_t *id = page;
    ascii_copy(c->serial, sizeof(c->serial), id + 4, 20);
    ascii_copy(c->model, sizeof(c->model), id + 24, 40);
    uint8_t mdts = id[77];
    uint32_t nn = *(const uint32_t *)(id + 516);
    c->max_bytes = BOUNCE_PAGES * 4096;
    if (mdts && mdts < 16 && (4096u << mdts) < c->max_bytes) {
        c->max_bytes = 4096u << mdts;
    }

    /* The bounce buffer and its PRP list. */
    void *bounce = NULL, *list = NULL;
    c->bounce_phys = dma_alloc(BOUNCE_PAGES, &bounce);
    c->prp_list_phys = dma_alloc(1, &list);
    if (!c->bounce_phys || !c->prp_list_phys) {
        return false;
    }
    c->bounce = bounce;
    c->prp_list = list;
    for (int i = 1; i < BOUNCE_PAGES; i++) {
        c->prp_list[i - 1] = c->bounce_phys + (uint64_t)i * 4096;
    }

    /* One I/O queue pair: completion queue first (physically contiguous, no interrupts). */
    if (admin(c, ADMIN_CREATE_CQ, 0, c->io.cq_phys, (QUEUE_DEPTH - 1) << 16 | 1, 1, NULL) != 0 ||
        admin(c, ADMIN_CREATE_SQ, 0, c->io.sq_phys, (QUEUE_DEPTH - 1) << 16 | 1, 1u << 16 | 1, NULL) != 0) {
        kprintf("[WARN] nvme: couldn't create the I/O queues\n");
        return false;
    }

    /* Each namespace with blocks is a disk. */
    int found = 0;
    for (uint32_t nsid = 1; nsid <= nn && nsid <= MAX_NAMESPACES; nsid++) {
        memset(page, 0, 4096);
        if (admin(c, ADMIN_IDENTIFY, nsid, page_phys, 0, 0, NULL) != 0) {
            continue;
        }
        const uint8_t *ns_id = page;
        uint64_t size = *(const uint64_t *)ns_id;          /* NSZE */
        uint8_t flbas = ns_id[26] & 0xF;
        uint32_t lbaf = *(const uint32_t *)(ns_id + 128 + 4 * flbas);
        uint32_t lbads = (lbaf >> 16) & 0xFF;
        uint16_t ms = (uint16_t)(lbaf & 0xFFFF);
        if (!size || lbads < 9 || lbads > 12 || ms) {
            continue;   /* empty, odd sector size, or metadata we'd have to handle */
        }
        struct namespace *ns = kzalloc(sizeof(*ns));
        if (!ns) {
            break;
        }
        ns->c = c;
        ns->nsid = nsid;
        struct block_device *d = &ns->bdev;
        ksnprintf(d->name, sizeof(d->name), "nvme%d", disk_number++);
        strlcpy(d->model, c->model, sizeof(d->model));
        strlcpy(d->serial, c->serial, sizeof(d->serial));
        d->sector_size = 1u << lbads;
        d->sectors = size;
        d->read = nvme_read;
        d->write = nvme_write;
        d->flush = nvme_flush;
        d->priv = ns;
        block_add_disk(d);
        found++;
    }
    pmm_free_page(page_phys);
    return found > 0;
}

void nvme_init(void) {
    pci_init();
    struct pci_device d;
    for (int i = 0; pci_find_class(0x01, 0x08, i, &d) && controller_count < MAX_CONTROLLERS; i++) {
        if (d.prog_if != 0x02) {
            continue;
        }
        struct controller *c = &controllers[controller_count];
        memset(c, 0, sizeof(*c));
        c->pci = d;
        c->lock = (struct mutex)MUTEX_INIT;
        if (start(c)) {
            controller_count++;
        } else {
            kprintf("[WARN] nvme %04x:%04x at %02x:%02x.%x: not usable\n", d.vendor, d.device, d.bus, d.slot,
                    d.function);
        }
    }
}

/* Tell the controllers we're powering off, so they save what's in flight (nvme_disable_ctrl, shutdown). */
void nvme_shutdown(void) {
    for (int i = 0; i < controller_count; i++) {
        struct controller *c = &controllers[i];
        if (c->dead) {
            continue;
        }
        mutex_lock(&c->lock);
        wr32(c, REG_CC, (rd32(c, REG_CC) & ~(3u << 14)) | CC_SHN_NORMAL);
        uint64_t end = uptime_ms() + 5000;
        while ((rd32(c, REG_CSTS) & CSTS_SHST_MASK) != CSTS_SHST_DONE && uptime_ms() < end) {
            __asm__ volatile("pause");
        }
        mutex_unlock(&c->lock);
    }
}
