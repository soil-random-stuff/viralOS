#pragma once

#include <stdint.h>

/*
 * Vector layout:
 *   0-31    CPU exceptions
 *   32-47   legacy ISA IRQs 0-15, routed through the I/O APIC
 *   48      local APIC timer
 *   0xE0-EF masked 8259 PICs (never expected)
 *   0xFF    local APIC spurious interrupt
 */
#define IRQ_BASE         32
#define IRQ_COUNT        16
#define VECTOR_TIMER     48
#define VECTOR_SPURIOUS  0xFF

/* Stack layout built by isr.S, lowest address first. */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    /* pushed by the CPU */
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*interrupt_handler_t)(struct interrupt_frame *frame);

void idt_init(void);
/* Load the (shared) IDT on another CPU. */
void idt_load(void);

/* Handlers run with interrupts disabled; the EOI is sent for you afterwards. */
void interrupt_register(uint8_t vector, interrupt_handler_t handler);

/* Install a handler for ISA IRQ `irq` (0-15) and unmask it in the I/O APIC. */
void irq_install(unsigned irq, interrupt_handler_t handler);
