/*
 * PCI configuration space through I/O ports 0xCF8 (address) and 0xCFC
 * (data), and a scan of every bus/slot/function at boot, as Linux's
 * arch/x86/pci/direct.c and drivers/pci/probe.c do (minus bridges'
 * resource assignment: the firmware has already placed everything).
 */

#include "drivers/pci.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/smp.h"
#include "lib/kprintf.h"
#include "mm/vmm.h"

#define CONFIG_ADDRESS 0xCF8
#define CONFIG_DATA    0xCFC
#define MAX_DEVICES    64

static struct pci_device devices[MAX_DEVICES];
static int device_count;
static bool scanned;

static inline void outl(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port) : "memory");
}

static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}

static void select(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    outl(CONFIG_ADDRESS, 0x80000000u | (uint32_t)bus << 16 | (uint32_t)(slot & 31) << 11 |
                             (uint32_t)(function & 7) << 8 | (offset & 0xFC));
}

static uint32_t raw_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    uint64_t flags = irq_save();
    select(bus, slot, function, offset);
    uint32_t v = inl(CONFIG_DATA);
    irq_restore(flags);
    return v;
}

uint32_t pci_read32(const struct pci_device *d, uint8_t offset) {
    return raw_read32(d->bus, d->slot, d->function, offset);
}

uint16_t pci_read16(const struct pci_device *d, uint8_t offset) {
    return (uint16_t)(pci_read32(d, offset) >> ((offset & 2) * 8));
}

uint8_t pci_read8(const struct pci_device *d, uint8_t offset) {
    return (uint8_t)(pci_read32(d, offset) >> ((offset & 3) * 8));
}

void pci_write32(const struct pci_device *d, uint8_t offset, uint32_t value) {
    uint64_t flags = irq_save();
    select(d->bus, d->slot, d->function, offset);
    outl(CONFIG_DATA, value);
    irq_restore(flags);
}

/* Narrower writes: read-modify-write of the containing dword. */
void pci_write16(const struct pci_device *d, uint8_t offset, uint16_t value) {
    int shift = (offset & 2) * 8;
    uint32_t v = pci_read32(d, offset);
    v = (v & ~(0xFFFFu << shift)) | (uint32_t)value << shift;
    pci_write32(d, offset, v);
}

void pci_write8(const struct pci_device *d, uint8_t offset, uint8_t value) {
    int shift = (offset & 3) * 8;
    uint32_t v = pci_read32(d, offset);
    v = (v & ~(0xFFu << shift)) | (uint32_t)value << shift;
    pci_write32(d, offset, v);
}

static void add(uint8_t bus, uint8_t slot, uint8_t function) {
    if (device_count == MAX_DEVICES) {
        return;
    }
    uint32_t id = raw_read32(bus, slot, function, 0x00);
    uint32_t class = raw_read32(bus, slot, function, 0x08);
    struct pci_device *d = &devices[device_count++];
    d->bus = bus;
    d->slot = slot;
    d->function = function;
    d->vendor = (uint16_t)id;
    d->device = (uint16_t)(id >> 16);
    d->class_code = (uint8_t)(class >> 24);
    d->subclass = (uint8_t)(class >> 16);
    d->prog_if = (uint8_t)(class >> 8);
}

void pci_init(void) {
    if (scanned) {
        return;
    }
    scanned = true;
    /* Brute force over all 256 buses: simple, and quick enough at boot. */
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            if ((raw_read32((uint8_t)bus, (uint8_t)slot, 0, 0) & 0xFFFF) == 0xFFFF) {
                continue;   /* nothing here */
            }
            bool multi = (raw_read32((uint8_t)bus, (uint8_t)slot, 0, 0x0C) >> 16) & 0x80;
            for (int fn = 0; fn < (multi ? 8 : 1); fn++) {
                if ((raw_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)fn, 0) & 0xFFFF) != 0xFFFF) {
                    add((uint8_t)bus, (uint8_t)slot, (uint8_t)fn);
                }
            }
        }
    }
}

bool pci_find_id(uint16_t vendor, uint16_t device, int n, struct pci_device *out) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].vendor == vendor && devices[i].device == device && n-- == 0) {
            *out = devices[i];
            return true;
        }
    }
    return false;
}

/* --- capabilities and message-signalled interrupts (drivers/pci/msi) ------------------------- */

#define PCI_STATUS          0x06
#define PCI_STATUS_CAP_LIST 0x10
#define PCI_CAPABILITY_LIST 0x34

uint8_t pci_find_capability(const struct pci_device *d, uint8_t id, uint8_t after) {
    if (!(pci_read16(d, PCI_STATUS) & PCI_STATUS_CAP_LIST)) {
        return 0;
    }
    uint8_t pos = after ? pci_read8(d, (uint8_t)(after + 1)) : pci_read8(d, PCI_CAPABILITY_LIST);
    for (int guard = 0; pos >= 0x40 && guard < 48; guard++) {
        pos &= ~3;
        if (pci_read8(d, pos) == id) {
            return pos;
        }
        pos = pci_read8(d, (uint8_t)(pos + 1));
    }
    return 0;
}

/* Vectors 0x50-0xDF are handed out to devices, one each. */
#define MSI_VECTOR_FIRST 0x50
#define MSI_VECTOR_LAST  0xDF

static struct {
    void (*handler)(void *arg);
    void *arg;
} msi_handlers[256];
static uint8_t next_vector = MSI_VECTOR_FIRST;

static void msi_interrupt(struct interrupt_frame *frame) {
    uint8_t v = (uint8_t)frame->vector;
    if (msi_handlers[v].handler) {
        msi_handlers[v].handler(msi_handlers[v].arg);
    }
}

#define MSI_ADDRESS_BASE 0xFEE00000u   /* the local APICs' window; bits 19:12 = destination APIC id */

bool pci_enable_msi(const struct pci_device *d, void (*handler)(void *arg), void *arg, bool allow_msix) {
    uint8_t msix = allow_msix ? pci_find_capability(d, PCI_CAP_MSIX, 0) : 0;
    uint8_t msi = pci_find_capability(d, PCI_CAP_MSI, 0);
    if ((!msix && !msi) || next_vector > MSI_VECTOR_LAST) {
        return false;
    }
    uint8_t vector = next_vector++;
    msi_handlers[vector].handler = handler;
    msi_handlers[vector].arg = arg;
    interrupt_register(vector, msi_interrupt);
    struct cpu *bsp = smp_cpu(0);
    uint32_t address = MSI_ADDRESS_BASE | (bsp ? bsp->lapic_id & 0xFF : 0) << 12;
    uint32_t data = vector;   /* fixed delivery, edge triggered */

    if (msix) {
        /* The table lives in one of the device's memory BARs. */
        uint16_t control = pci_read16(d, (uint8_t)(msix + 2));
        uint32_t table = pci_read32(d, (uint8_t)(msix + 4));
        int entries = (control & 0x7FF) + 1;
        uint64_t bar = pci_bar_address(d, table & 7);
        if (!bar) {
            return false;
        }
        volatile uint32_t *t = vmm_map_mmio(bar + (table & ~7u), (size_t)entries * 16);
        if (!t) {
            return false;
        }
        pci_write16(d, (uint8_t)(msix + 2), (uint16_t)(control | 0x8000 | 0x4000));   /* on, all masked */
        for (int i = 0; i < entries; i++) {
            t[i * 4 + 0] = address;
            t[i * 4 + 1] = 0;
            t[i * 4 + 2] = data;
            t[i * 4 + 3] = 0;   /* unmasked */
        }
        pci_write16(d, (uint8_t)(msix + 2), (uint16_t)((control | 0x8000) & ~0x4000));
    } else {
        uint16_t control = pci_read16(d, (uint8_t)(msi + 2));
        bool is64 = control & 0x80;
        pci_write32(d, (uint8_t)(msi + 4), address);
        if (is64) {
            pci_write32(d, (uint8_t)(msi + 8), 0);
            pci_write16(d, (uint8_t)(msi + 12), (uint16_t)data);
        } else {
            pci_write16(d, (uint8_t)(msi + 8), (uint16_t)data);
        }
        /* One message (multiple-message enable = 0), on. */
        pci_write16(d, (uint8_t)(msi + 2), (uint16_t)((control & ~0x70) | 1));
    }
    pci_write16(d, PCI_COMMAND, (uint16_t)(pci_read16(d, PCI_COMMAND) | PCI_COMMAND_INTX_OFF));
    return true;
}

bool pci_find_class(uint8_t class_code, uint8_t subclass, int n, struct pci_device *out) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].class_code == class_code && devices[i].subclass == subclass && n-- == 0) {
            *out = devices[i];
            return true;
        }
    }
    return false;
}

uint16_t pci_bar_io(const struct pci_device *d, int bar) {
    uint32_t v = pci_read32(d, (uint8_t)(0x10 + 4 * bar));
    return v & 1 ? (uint16_t)(v & ~3u) : 0;
}

uint64_t pci_bar_address(const struct pci_device *d, int bar) {
    uint8_t offset = (uint8_t)(0x10 + 4 * bar);
    uint32_t low = pci_read32(d, offset);
    if (low & 1) {
        return 0;   /* an I/O port BAR */
    }
    uint64_t address = low & ~0xFull;
    if (((low >> 1) & 3) == 2) {   /* 64-bit: the next BAR holds the upper half */
        address |= (uint64_t)pci_read32(d, (uint8_t)(offset + 4)) << 32;
    }
    return address;
}

/* __pci_read_base: write all ones, read back which address bits stick, restore. Memory decoding is off
 * meanwhile so the device never answers at the half-written address. */
uint64_t pci_bar_size(const struct pci_device *d, int bar) {
    uint8_t offset = (uint8_t)(0x10 + 4 * bar);
    uint32_t low = pci_read32(d, offset);
    if (low & 1) {
        return 0;
    }
    bool is64 = ((low >> 1) & 3) == 2;
    uint16_t command = pci_read16(d, PCI_COMMAND);
    pci_write16(d, PCI_COMMAND, (uint16_t)(command & ~(PCI_COMMAND_MEMORY | PCI_COMMAND_IO)));

    pci_write32(d, offset, 0xFFFFFFFF);
    uint64_t mask = pci_read32(d, offset) & ~0xFull;
    pci_write32(d, offset, low);
    if (is64) {
        uint32_t high = pci_read32(d, (uint8_t)(offset + 4));
        pci_write32(d, (uint8_t)(offset + 4), 0xFFFFFFFF);
        mask |= (uint64_t)pci_read32(d, (uint8_t)(offset + 4)) << 32;
        pci_write32(d, (uint8_t)(offset + 4), high);
    } else {
        mask |= 0xFFFFFFFF00000000ull;
    }

    pci_write16(d, PCI_COMMAND, command);
    return mask & 0xFFFFFFFFFFFFFFF0ull ? ~mask + 1 : 0;
}

int pci_device_count(void) {
    return device_count;
}

const struct pci_device *pci_device_at(int i) {
    return i >= 0 && i < device_count ? &devices[i] : 0;
}
