#include <stddef.h>

#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "acpi/acpi.h"
#include "drivers/pit.h"
#include "lib/panic.h"
#include "mm/vmm.h"

/* --- MADT ("APIC" table) --------------------------------------------------- */

struct madt {
    struct acpi_sdt_header header;
    uint32_t lapic_address;
    uint32_t flags;
    uint8_t  entries[];
} __attribute__((packed));

enum {
    MADT_LAPIC          = 0,
    MADT_IOAPIC         = 1,
    MADT_ISO            = 2,   /* interrupt source override */
    MADT_LAPIC_OVERRIDE = 5,   /* 64-bit local APIC address */
};

struct madt_entry_header { uint8_t type, length; } __attribute__((packed));

struct madt_lapic {
    struct madt_entry_header h;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;            /* bit 0: enabled, bit 1: online capable */
} __attribute__((packed));

struct madt_ioapic {
    struct madt_entry_header h;
    uint8_t  id;
    uint8_t  reserved;
    uint32_t address;
    uint32_t gsi_base;
} __attribute__((packed));

struct madt_iso {
    struct madt_entry_header h;
    uint8_t  bus;              /* 0 = ISA */
    uint8_t  source;           /* ISA IRQ */
    uint32_t gsi;              /* global system interrupt it is wired to */
    uint16_t flags;            /* polarity (bits 0-1), trigger mode (bits 2-3) */
} __attribute__((packed));

struct madt_lapic_override {
    struct madt_entry_header h;
    uint16_t reserved;
    uint64_t address;
} __attribute__((packed));

/* --- state ------------------------------------------------------------------ */

#define MAX_IOAPICS 8

struct ioapic {
    volatile uint32_t *regs;
    uint32_t gsi_base;
    uint32_t gsi_count;
};

/* How each ISA IRQ is wired: identity, active high, edge, unless overridden. */
struct isa_route {
    uint32_t gsi;
    bool active_low;
    bool level_triggered;
};

static volatile uint32_t *lapic;
static struct ioapic ioapics[MAX_IOAPICS];
static unsigned ioapic_count;
static struct isa_route isa_routes[16];
static unsigned cpu_count;
static uint64_t timer_frequency;
static uint64_t tsc_frequency;

static inline uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

/* --- local APIC --------------------------------------------------------------- */

enum {
    LAPIC_ID          = 0x020,
    LAPIC_TPR         = 0x080,
    LAPIC_EOI         = 0x0B0,
    LAPIC_SVR         = 0x0F0,
    LAPIC_ICR_LOW     = 0x300,
    LAPIC_ICR_HIGH    = 0x310,
    LAPIC_LVT_TIMER   = 0x320,
    LAPIC_TIMER_INIT  = 0x380,
    LAPIC_TIMER_COUNT = 0x390,
    LAPIC_TIMER_DIV   = 0x3E0,
};

#define LAPIC_SVR_ENABLE     (1u << 8)
#define LAPIC_SPURIOUS_VEC   0xFF
#define LVT_MASKED           (1u << 16)
#define LVT_TIMER_PERIODIC   (1u << 17)
#define TIMER_DIV_16         0x3

#define MSR_IA32_APIC_BASE   0x1B
#define APIC_BASE_ENABLE     (1ULL << 11)
#define APIC_BASE_X2APIC     (1ULL << 10)

static uint32_t lapic_read(uint32_t reg) {
    return lapic[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t value) {
    lapic[reg / 4] = value;
}

void lapic_eoi(void) {
    if (lapic) {
        lapic_write(LAPIC_EOI, 0);
    }
}

uint32_t lapic_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}

/* This CPU's local APIC: on, accepting everything, timer stopped (the registers are at the same
   address on every CPU; each sees its own). */
static void lapic_enable_here(void) {
    wrmsr(MSR_IA32_APIC_BASE, rdmsr(MSR_IA32_APIC_BASE) | APIC_BASE_ENABLE);
    lapic_write(LAPIC_TPR, 0);                                  /* accept all priorities */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VEC);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
}

void lapic_init_ap(void) {
    lapic_enable_here();
}

#define ICR_PENDING      (1u << 12)
#define ICR_NMI          (4u << 8)
#define ICR_ASSERT       (1u << 14)
#define ICR_ALL_BUT_SELF (3u << 18)

static void icr_wait(void) {
    for (int i = 0; i < 1000000 && (lapic_read(LAPIC_ICR_LOW) & ICR_PENDING); i++) {
        __asm__ volatile ("pause");
    }
}

void lapic_send_ipi(uint32_t apic_id, uint8_t vector) {
    uint64_t irq = irq_save();
    icr_wait();
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, vector | ICR_ASSERT);   /* fixed delivery, physical destination */
    irq_restore(irq);
}

void lapic_send_nmi_all_but_self(void) {
    icr_wait();
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW, ICR_NMI | ICR_ASSERT | ICR_ALL_BUT_SELF);
    icr_wait();
}

static void lapic_init(uint64_t phys) {
    uint64_t base = rdmsr(MSR_IA32_APIC_BASE);
    if (base & APIC_BASE_X2APIC) {
        panic("apic: firmware left the CPU in x2APIC mode, which is not supported yet");
    }
    wrmsr(MSR_IA32_APIC_BASE, base | APIC_BASE_ENABLE);

    lapic = vmm_map_mmio(phys, 0x1000);
    if (!lapic) {
        panic("apic: cannot map local APIC registers");
    }
    lapic_write(LAPIC_TPR, 0);                                  /* accept all priorities */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VEC);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
}

/*
 * The APIC timer counts down at the (unknown) bus clock divided by 16, so
 * measure it against the PIT, whose frequency is fixed at 1.193182 MHz.
 */
static void calibrate_timer(void) {
    lapic_write(LAPIC_TIMER_DIV, TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);

    pit_oneshot_start(10000);
    uint64_t tsc_start = read_tsc();
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);
    pit_oneshot_wait();
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_COUNT);
    tsc_frequency = (read_tsc() - tsc_start) * 100;   /* the time stamp counter too (timer.c: uptime_us) */
    lapic_write(LAPIC_TIMER_INIT, 0);

    timer_frequency = (uint64_t)elapsed * 100;   /* counts in 10 ms -> per second */
    if (timer_frequency == 0) {
        panic("apic: timer calibration failed");
    }
}

void lapic_timer_start_ap(uint32_t hz, uint8_t vector) {
    /* Same bus clock on every CPU: the BSP's calibration holds. */
    lapic_write(LAPIC_TIMER_DIV, TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, vector | LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_INIT, (uint32_t)(timer_frequency / hz));
}

void lapic_timer_start(uint32_t hz, uint8_t vector) {
    calibrate_timer();
    lapic_write(LAPIC_TIMER_DIV, TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, vector | LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_INIT, (uint32_t)(timer_frequency / hz));
}

uint64_t lapic_tsc_frequency(void) {
    return tsc_frequency;
}

uint64_t lapic_timer_frequency(void) {
    return timer_frequency;
}

/* --- I/O APIC ------------------------------------------------------------------ */

enum {
    IOAPIC_REGSEL  = 0x00 / 4,
    IOAPIC_WINDOW  = 0x10 / 4,
    IOAPIC_VERSION = 0x01,
    IOAPIC_REDTBL  = 0x10,     /* entry n: registers 0x10 + 2n (low), +1 (high) */
};

#define REDIR_ACTIVE_LOW  (1u << 13)
#define REDIR_LEVEL       (1u << 15)
#define REDIR_MASKED      (1u << 16)

static uint32_t ioapic_read(struct ioapic *io, uint32_t reg) {
    io->regs[IOAPIC_REGSEL] = reg;
    return io->regs[IOAPIC_WINDOW];
}

static void ioapic_write(struct ioapic *io, uint32_t reg, uint32_t value) {
    io->regs[IOAPIC_REGSEL] = reg;
    io->regs[IOAPIC_WINDOW] = value;
}

static void ioapic_add(uint64_t phys, uint32_t gsi_base) {
    if (ioapic_count == MAX_IOAPICS) {
        return;
    }
    struct ioapic *io = &ioapics[ioapic_count];
    io->regs = vmm_map_mmio(phys, 0x20);
    if (!io->regs) {
        panic("apic: cannot map I/O APIC registers");
    }
    io->gsi_base = gsi_base;
    io->gsi_count = ((ioapic_read(io, IOAPIC_VERSION) >> 16) & 0xFF) + 1;

    for (uint32_t i = 0; i < io->gsi_count; i++) {
        ioapic_write(io, IOAPIC_REDTBL + 2 * i, REDIR_MASKED);
        ioapic_write(io, IOAPIC_REDTBL + 2 * i + 1, 0);
    }
    ioapic_count++;
}

void ioapic_route_isa_irq(unsigned irq, uint8_t vector) {
    struct isa_route *route = &isa_routes[irq];
    for (unsigned i = 0; i < ioapic_count; i++) {
        struct ioapic *io = &ioapics[i];
        if (route->gsi < io->gsi_base || route->gsi >= io->gsi_base + io->gsi_count) {
            continue;
        }
        uint32_t pin = route->gsi - io->gsi_base;
        uint32_t low = vector;   /* fixed delivery, physical destination, unmasked */
        if (route->active_low) {
            low |= REDIR_ACTIVE_LOW;
        }
        if (route->level_triggered) {
            low |= REDIR_LEVEL;
        }
        ioapic_write(io, IOAPIC_REDTBL + 2 * pin + 1, lapic_id() << 24);
        ioapic_write(io, IOAPIC_REDTBL + 2 * pin, low);
        return;
    }
    panic("apic: no I/O APIC handles GSI %u (ISA IRQ %u)", route->gsi, irq);
}

/* --- setup --------------------------------------------------------------------- */

void apic_init(void) {
    const struct madt *madt = (const struct madt *)acpi_find_table("APIC");
    if (!madt) {
        panic("apic: no MADT in the ACPI tables");
    }

    for (unsigned irq = 0; irq < 16; irq++) {
        isa_routes[irq] = (struct isa_route){ .gsi = irq };
    }

    uint64_t lapic_phys = madt->lapic_address;
    const uint8_t *p = madt->entries;
    const uint8_t *end = (const uint8_t *)madt + madt->header.length;

    while (p + sizeof(struct madt_entry_header) <= end) {
        const struct madt_entry_header *h = (const struct madt_entry_header *)p;
        if (h->length < sizeof(*h)) {
            break;
        }
        switch (h->type) {
        case MADT_LAPIC: {
            const struct madt_lapic *e = (const void *)h;
            if (e->flags & 0x3) {
                cpu_count++;
            }
            break;
        }
        case MADT_IOAPIC: {
            const struct madt_ioapic *e = (const void *)h;
            ioapic_add(e->address, e->gsi_base);
            break;
        }
        case MADT_ISO: {
            const struct madt_iso *e = (const void *)h;
            if (e->bus == 0 && e->source < 16) {
                /* Polarity/trigger 0b00 means "conforms to the bus": ISA is active high, edge. */
                isa_routes[e->source] = (struct isa_route){
                    .gsi = e->gsi,
                    .active_low = (e->flags & 0x3) == 0x3,
                    .level_triggered = ((e->flags >> 2) & 0x3) == 0x3,
                };
            }
            break;
        }
        case MADT_LAPIC_OVERRIDE: {
            const struct madt_lapic_override *e = (const void *)h;
            lapic_phys = e->address;
            break;
        }
        }
        p += h->length;
    }

    if (ioapic_count == 0) {
        panic("apic: no I/O APIC found");
    }
    lapic_init(lapic_phys);
}

unsigned apic_cpu_count(void) {
    return cpu_count;
}

unsigned apic_ioapic_count(void) {
    return ioapic_count;
}
