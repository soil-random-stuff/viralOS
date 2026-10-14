/*
 * Interrupt Descriptor Table and the C side of interrupt handling:
 * CPU exceptions end in a register dump, everything else goes to the
 * handler registered for its vector.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/smp.h"
#include "drivers/timer.h"
#include "drivers/console.h"
#include "lib/kprintf.h"
#include "lib/panic.h"
#include "mm/vmm.h"
#include "proc/process.h"
#include "sched/sched.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define GATE_INTERRUPT 0x8E /* present, DPL 0, 64-bit interrupt gate (clears IF) */

extern const uint64_t isr_stub_table[256];

static struct idt_entry idt[256];
static interrupt_handler_t handlers[256];
static uint64_t unexpected_interrupts;

static const char *const exception_names[32] = {
    "Divide Error (#DE)",           "Debug (#DB)",
    "Non-Maskable Interrupt",       "Breakpoint (#BP)",
    "Overflow (#OF)",               "Bound Range Exceeded (#BR)",
    "Invalid Opcode (#UD)",         "Device Not Available (#NM)",
    "Double Fault (#DF)",           "Coprocessor Segment Overrun",
    "Invalid TSS (#TS)",            "Segment Not Present (#NP)",
    "Stack-Segment Fault (#SS)",    "General Protection Fault (#GP)",
    "Page Fault (#PF)",             "Reserved",
    "x87 Floating-Point (#MF)",     "Alignment Check (#AC)",
    "Machine Check (#MC)",          "SIMD Floating-Point (#XM)",
    "Virtualization (#VE)",         "Control Protection (#CP)",
    "Reserved",                     "Reserved",
    "Reserved",                     "Reserved",
    "Reserved",                     "Reserved",
    "Hypervisor Injection (#HV)",   "VMM Communication (#VC)",
    "Security Exception (#SX)",     "Reserved",
};

static void set_gate(uint8_t vector, uint64_t handler, uint8_t ist) {
    idt[vector] = (struct idt_entry){
        .offset_low  = handler & 0xFFFF,
        .selector    = GDT_KERNEL_CODE,
        .ist         = ist,
        .type_attr   = GATE_INTERRUPT,
        .offset_mid  = (handler >> 16) & 0xFFFF,
        .offset_high = handler >> 32,
    };
}

void idt_load(void) {
    struct idt_pointer idtr = {
        .limit = sizeof(idt) - 1,
        .base = (uint64_t)idt,
    };
    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

static void reschedule_ipi(struct interrupt_frame *frame) {
    (void)frame;   /* the work happens on the way out: sched_preempt() */
}

void idt_init(void) {
    for (unsigned v = 0; v < 256; v++) {
        set_gate(v, isr_stub_table[v], v == 8 ? IST_DOUBLE_FAULT : 0);
    }
    handlers[VECTOR_RESCHEDULE] = reschedule_ipi;
    idt_load();
}

void interrupt_register(uint8_t vector, interrupt_handler_t handler) {
    handlers[vector] = handler;
}

void irq_install(unsigned irq, interrupt_handler_t handler) {
    if (irq < IRQ_COUNT) {
        interrupt_register(IRQ_BASE + irq, handler);
        ioapic_route_isa_irq(irq, IRQ_BASE + irq);
    }
}

static void describe_page_fault(uint64_t error) {
    kprintf("Faulting address: %p (%s, %s, %s mode%s%s)\n",
            (void *)read_cr2(),
            error & 0x01 ? "protection violation" : "page not present",
            error & 0x02 ? "write" : "read",
            error & 0x04 ? "user" : "kernel",
            error & 0x08 ? ", reserved bit set" : "",
            error & 0x10 ? ", instruction fetch" : "");
}

__attribute__((noreturn)) static void exception_panic(struct interrupt_frame *f) {
    panic_begin();
    kprintf("\n KERNEL PANIC: %s, vector %lu, error code 0x%lx \n",
            exception_names[f->vector], f->vector, f->error_code);
    console_set_color(0xFFB0B0, 0x101018);

    const struct thread *t = sched_current();
    if (t) {
        kprintf("In thread %lu '%s'\n", t->id, t->name);
    }
    if (f->vector == 14) {
        describe_page_fault(f->error_code);
    }
    /* A stack overflow page-faults on the guard page, and pushing the #PF frame
       onto that same stack faults again, so it usually arrives as a #DF. */
    if ((f->vector == 14 || f->vector == 8) && vmm_is_stack_guard(read_cr2())) {
        kprintf("CR2 %p is in a kernel stack guard page: stack overflow!\n",
                (void *)read_cr2());
    }

    kprintf("RAX=%016lx RBX=%016lx RCX=%016lx RDX=%016lx\n", f->rax, f->rbx, f->rcx, f->rdx);
    kprintf("RSI=%016lx RDI=%016lx RBP=%016lx RSP=%016lx\n", f->rsi, f->rdi, f->rbp, f->rsp);
    kprintf("R8 =%016lx R9 =%016lx R10=%016lx R11=%016lx\n", f->r8, f->r9, f->r10, f->r11);
    kprintf("R12=%016lx R13=%016lx R14=%016lx R15=%016lx\n", f->r12, f->r13, f->r14, f->r15);
    kprintf("RIP=%016lx CS=%04lx SS=%04lx RFLAGS=%08lx\n", f->rip, f->cs, f->ss, f->rflags);
    kprintf("CR2=%016lx CR3=%016lx\n", read_cr2(), read_cr3());
    print_backtrace(f->rip, f->rbp);

    kprintf("\nSystem halted.\n");
    halt_forever();
}

static bool from_user_mode(const struct interrupt_frame *frame) {
    return (frame->cs & 3) == 3;
}

void interrupt_dispatch(struct interrupt_frame *frame) {
    /* Another CPU panicked and stopped us (an NMI): stay stopped. */
    if (frame->vector == 2 && panic_in_progress()) {
        halt_forever();
    }
    struct cpu *cpu = this_cpu();
    /* The clock keeps going even while another CPU holds the kernel lock for a while. */
    if (frame->vector == VECTOR_TIMER && cpu->index == 0) {
        timer_count_tick();
    }
    /* Kernel code runs under the big kernel lock: take it unless this CPU was already in the kernel
       with it (an interrupt from a program, or from the idle loop, needs it). */
    bool took = !cpu->holds_lock;
    if (took) {
        kernel_lock();
    }

    if (frame->vector < 32) {
        /* A program's mistake only takes down that program. */
        if (from_user_mode(frame)) {
            process_fault(frame, exception_names[frame->vector]);
        }
        exception_panic(frame);
    }
    /* The APIC's spurious vector must not be acknowledged. */
    if (frame->vector == VECTOR_SPURIOUS) {
        if (took) {
            kernel_unlock();
        }
        return;
    }

    if (handlers[frame->vector]) {
        handlers[frame->vector](frame);
    } else {
        unexpected_interrupts++;
    }
    lapic_eoi();

    /*
     * If the handler made another thread runnable or the time slice ran
     * out, switch now. We are on the interrupted thread's stack, so the
     * switch simply parks this whole interrupt frame until it runs again.
     */
    sched_preempt();

    /* About to resume a program: if it was killed (Ctrl+C, `kill`), end it instead. */
    if (from_user_mode(frame)) {
        process_exit_if_killed();
    }
    /* (This thread may have been away and come back on another CPU: whichever it is now, it holds
       the lock, and lets go if the code we return to didn't have it.) */
    if (took) {
        kernel_unlock();
    }
}
