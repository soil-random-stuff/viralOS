#include <stdbool.h>
#include <stddef.h>

#include "acpi/power.h"
#include "acpi/acpi.h"
#include "arch/x86_64/cpu.h"
#include "block/block.h"
#include "drivers/timer.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/mm.h"
#include "sched/sched.h"

#include <viralos.h>

/* Byte offsets of the FADT fields we use (from the start of the table). */
#define FADT_DSDT          40
#define FADT_SMI_CMD       48
#define FADT_ACPI_ENABLE   52
#define FADT_PM1A_CNT      64
#define FADT_PM1B_CNT      68
#define FADT_FLAGS         112
#define FADT_RESET_REG     116   /* generic address: space, width, offset, size, address */
#define FADT_RESET_VALUE   128
#define FADT_X_DSDT        140

#define FLAG_RESET_REG_SUP (1u << 10)
#define PM1_SCI_EN         1
#define PM1_SLP_EN         (1u << 13)

static uint32_t read32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t read64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

/* One small integer of the \_S5 package: BytePrefix n, Zero, One, or a raw byte. */
static const uint8_t *aml_small_int(const uint8_t *p, uint8_t *out) {
    if (*p == 0x0A) {
        *out = p[1];
        return p + 2;
    }
    *out = *p == 0x01 ? 1 : *p == 0x00 ? 0 : *p;
    return p + 1;
}

/* Find SLP_TYPa/b for S5 in the DSDT. */
static bool find_s5(const struct acpi_sdt_header *dsdt, uint8_t *a, uint8_t *b) {
    const uint8_t *start = (const uint8_t *)dsdt + sizeof(*dsdt);
    const uint8_t *end = (const uint8_t *)dsdt + dsdt->length;
    for (const uint8_t *p = start; p + 8 < end; p++) {
        if (memcmp(p, "_S5_", 4) != 0) {
            continue;
        }
        /* NameOp (0x08) before the name, maybe with a root prefix '\'; PackageOp after. */
        bool named = p[-1] == 0x08 || (p[-1] == 0x5C && p[-2] == 0x08);   /* 0x5C: '\' */
        if (!named || p[4] != 0x12) {
            continue;
        }
        const uint8_t *q = p + 5;
        q += ((*q >> 6) & 3) + 1;   /* PkgLength: 1-4 bytes */
        q++;                        /* NumElements */
        q = aml_small_int(q, a);
        aml_small_int(q, b);
        return true;
    }
    return false;
}

static void power_off(void) {
    const uint8_t *fadt = (const uint8_t *)acpi_find_table("FACP");
    if (fadt) {
        const struct acpi_sdt_header *h = (const struct acpi_sdt_header *)fadt;
        uint64_t dsdt_phys = h->length >= FADT_X_DSDT + 8 && read64(fadt + FADT_X_DSDT)
                                 ? read64(fadt + FADT_X_DSDT) : read32(fadt + FADT_DSDT);
        uint16_t pm1a = (uint16_t)read32(fadt + FADT_PM1A_CNT);
        uint16_t pm1b = (uint16_t)read32(fadt + FADT_PM1B_CNT);
        uint8_t typ_a, typ_b;
        if (dsdt_phys && pm1a && find_s5(phys_to_virt(dsdt_phys), &typ_a, &typ_b)) {
            /* ACPI mode must be on for the sleep registers to work. */
            uint32_t smi = read32(fadt + FADT_SMI_CMD);
            uint8_t enable = fadt[FADT_ACPI_ENABLE];
            if (!(inw(pm1a) & PM1_SCI_EN) && smi && enable) {
                outb((uint16_t)smi, enable);
                for (int i = 0; i < 300 && !(inw(pm1a) & PM1_SCI_EN); i++) {
                    io_wait();
                }
            }
            outw(pm1a, (uint16_t)(typ_a << 10 | PM1_SLP_EN));
            if (pm1b) {
                outw(pm1b, (uint16_t)(typ_b << 10 | PM1_SLP_EN));
            }
        }
    }
    /* Still here: the fixed ports of QEMU's q35/PIIX machines and Bochs. */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
}

static void restart(void) {
    const uint8_t *fadt = (const uint8_t *)acpi_find_table("FACP");
    if (fadt) {
        const struct acpi_sdt_header *h = (const struct acpi_sdt_header *)fadt;
        bool has_reset = h->length >= FADT_RESET_VALUE + 1 &&
                         (read32(fadt + FADT_FLAGS) & FLAG_RESET_REG_SUP);
        uint64_t address = read64(fadt + FADT_RESET_REG + 4);
        if (has_reset && fadt[FADT_RESET_REG] == 1 && address) {   /* I/O space */
            outb((uint16_t)address, fadt[FADT_RESET_VALUE]);
        }
    }
    /* The keyboard controller can pulse the CPU's reset line. */
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {
    }
    outb(0x64, 0xFE);
    for (int i = 0; i < 1000; i++) {
        io_wait();
    }
    /* Last resort: an exception with no IDT to handle it is a triple fault, a reset. */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) none = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" : : "m"(none));
}

int power_off_or_restart(int how) {
    if (how != OS_POWER_OFF && how != OS_POWER_RESTART) {
        return -EINVAL;
    }
    kprintf("\n%s...\n", how == OS_POWER_OFF ? "Powering off" : "Restarting");
    block_shutdown();     /* everything on the disks, and the disks told to keep it */
    sched_sleep_ms(50);   /* let the message reach the serial port */
    interrupts_disable();
    if (how == OS_POWER_OFF) {
        power_off();
        /* Some machines take a moment. */
        for (int i = 0; i < 100000; i++) {
            io_wait();
        }
        interrupts_enable();
        kprintf("The computer didn't turn off: it's safe to switch it off now.\n");
        return -EIO;
    }
    restart();
    interrupts_enable();
    return -EIO;
}
