#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Local APIC (one per CPU: timer, EOI) and I/O APIC (routes device IRQs to
 * CPUs), discovered through the ACPI MADT.
 */
void apic_init(void);

void lapic_eoi(void);
uint32_t lapic_id(void);

/* Start the local APIC timer in periodic mode, firing `vector` at `hz` (calibrating it first). */
void lapic_timer_start(uint32_t hz, uint8_t vector);
/* The same on another CPU, with the bootstrap processor's calibration. */
void lapic_timer_start_ap(uint32_t hz, uint8_t vector);
/* Turn on an application processor's local APIC. */
void lapic_init_ap(void);
/* An interrupt for one CPU (by local APIC id), or an NMI for all the others. */
void lapic_send_ipi(uint32_t apic_id, uint8_t vector);
void lapic_send_nmi_all_but_self(void);
/* Timer input clock (bus clock / divider) measured during calibration. */
uint64_t lapic_timer_frequency(void);
/* The time stamp counter's rate, measured at the same time (0 if it didn't count). */
uint64_t lapic_tsc_frequency(void);

/* Route legacy ISA IRQ `irq` to `vector` on this CPU and unmask it. */
void ioapic_route_isa_irq(unsigned irq, uint8_t vector);

unsigned apic_cpu_count(void);
unsigned apic_ioapic_count(void);
