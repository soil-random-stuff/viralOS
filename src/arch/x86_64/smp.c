/*
 * The other CPUs, and the lock that keeps kernel code on one of them at a time (see smp.h).
 *
 * Starting an AP: Limine has already woken every processor and parked it in its own code, on its own
 * page tables and stack (in bootloader memory, which the kernel later takes back). Writing an entry
 * point into the processor's limine_mp_info sends it there: ap_start (below, in assembly) switches to
 * the kernel's page tables and a stack of its own at once, then ap_main waits for the BSP to finish
 * setting up the kernel before joining in.
 */

#include <limine.h>

#include "arch/x86_64/smp.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/fpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/syscall.h"
#include "drivers/pit.h"
#include "drivers/timer.h"
#include "lib/kprintf.h"
#include "lib/panic.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "sched/sched.h"

#define NO_OWNER     0xFFFFFFFFu
#define AP_STACK_SIZE (64 * 1024)

static struct cpu bsp;
static struct cpu *cpus[MAX_CPUS];
static unsigned cpu_total = 1;          /* known (online or starting) */
static volatile unsigned cpus_online = 1;

static volatile uint32_t lock_owner = 0;   /* the BSP holds it from the start */
static volatile uint64_t tlb_generation;

/* For ap_start, which runs before it has a stack or the kernel's page tables. */
uint64_t smp_kernel_cr3;
static volatile bool aps_released;
static volatile unsigned aps_parked;

/* --- the big kernel lock ---------------------------------------------------------------------------- */

void kernel_lock(void) {
    struct cpu *c = this_cpu();
    uint32_t free = NO_OWNER;
    while (!__atomic_compare_exchange_n(&lock_owner, &free, c->index, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        free = NO_OWNER;
        __asm__ volatile ("pause");
    }
    c->holds_lock = true;
    /* Kernel mappings changed while we were away: drop what this CPU had cached. */
    uint64_t gen = __atomic_load_n(&tlb_generation, __ATOMIC_ACQUIRE);
    if (c->tlb_generation != gen) {
        c->tlb_generation = gen;
        write_cr3(read_cr3());
    }
}

void kernel_unlock(void) {
    struct cpu *c = this_cpu();
    c->holds_lock = false;
    c->tlb_generation = __atomic_load_n(&tlb_generation, __ATOMIC_ACQUIRE);
    __atomic_store_n(&lock_owner, NO_OWNER, __ATOMIC_RELEASE);
}

void smp_kernel_mappings_changed(void) {
    if (cpus_online > 1) {
        __atomic_add_fetch(&tlb_generation, 1, __ATOMIC_RELEASE);
        this_cpu()->tlb_generation = tlb_generation;   /* (this CPU did its own invlpg) */
    }
}

/* --- CPUs ------------------------------------------------------------------------------------------- */

unsigned smp_cpu_count(void) {
    return cpus_online;
}

struct cpu *smp_cpu(unsigned index) {
    return index < cpu_total ? cpus[index] : NULL;
}

void smp_init_bsp(void) {
    bsp.self = &bsp;
    bsp.index = 0;
    bsp.online = true;
    bsp.holds_lock = true;
    cpus[0] = &bsp;
    gdt_init(&bsp);
    syscall_set_gs(&bsp);
}

void smp_kick(struct cpu *cpu) {
    if (cpu && cpu != this_cpu() && cpu->online) {
        lapic_send_ipi(cpu->lapic_id, VECTOR_RESCHEDULE);
    }
}

void smp_halt_others(void) {
    if (cpus_online > 1) {
        lapic_send_nmi_all_but_self();
    }
}

/* --- starting the APs -------------------------------------------------------------------------------- */

void ap_start(struct limine_mp_info *info);   /* below, assembly */
__attribute__((noreturn)) void ap_main(struct cpu *c);

/*
 * The AP arrives here from the bootloader with its info in RDI, still on the bootloader's page
 * tables and stack. extra_argument holds its struct cpu, whose kernel_rsp is its first stack.
 */
__asm__(
    ".global ap_start\n"
    "ap_start:\n"
    "    movq smp_kernel_cr3(%rip), %rax\n"
    "    movq %rax, %cr3\n"                 /* the kernel's page tables (they map the kernel and the HHDM too) */
    "    movq 24(%rdi), %rdi\n"             /* extra_argument: the struct cpu */
    "    movq 0(%rdi), %rsp\n"              /* cpu->kernel_rsp: its own stack */
    "    xorl %ebp, %ebp\n"
    "    call ap_main\n"
    "1:  cli\n"
    "    hlt\n"
    "    jmp 1b\n");

/* Make this CPU's control registers match the BSP's (paging features, NX, PAT for the framebuffer). */
static uint64_t bsp_cr0, bsp_cr4, bsp_efer, bsp_pat;

static uint64_t read_cr0_(void) { uint64_t v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v; }
static uint64_t read_cr4_(void) { uint64_t v; __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); return v; }

#define MSR_EFER 0xC0000080
#define MSR_PAT  0x277

/*
 * Until it parks, an AP runs alongside the BSP without the kernel lock: it must not allocate or
 * touch anything shared (smp_start_aps prepares all it needs). `stage` tells the BSP how far it got.
 */
__attribute__((noreturn)) void ap_main(struct cpu *c) {
    c->stage = 1;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(bsp_cr0) : "memory");
    __asm__ volatile ("mov %0, %%cr4" : : "r"(bsp_cr4 & ~(1ULL << 18)) : "memory");   /* (OSXSAVE: fpu_init) */
    c->stage = 2;
    wrmsr(MSR_EFER, bsp_efer);
    wrmsr(MSR_PAT, bsp_pat);
    c->stage = 3;
    gdt_init(c);
    c->stage = 4;
    syscall_set_gs(c);
    idt_load();
    c->stage = 5;

    __atomic_add_fetch(&aps_parked, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&aps_released, __ATOMIC_ACQUIRE)) {
        __asm__ volatile ("pause");
    }
    if (c->left_out) {   /* too late: the BSP went on without it */
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    /* The kernel is up: join in. */
    kernel_lock();
    c->lapic_id = lapic_id();
    lapic_init_ap();
    fpu_init_cpu();
    syscall_init();
    sched_init_cpu(c);
    c->online = true;
    __atomic_add_fetch(&cpus_online, 1, __ATOMIC_RELEASE);
    lapic_timer_start_ap(timer_hz(), VECTOR_TIMER);
    kprintf("       CPU %u (local APIC %u) is running\n", c->index, c->lapic_id);
    sched_idle();   /* lets go of the lock; from now on it runs whatever the scheduler gives it */
}

void smp_start_aps(void *response) {
    struct limine_mp_response *mp = response;
    bsp_cr0 = read_cr0_();
    bsp_cr4 = read_cr4_();
    bsp_efer = rdmsr(MSR_EFER);
    bsp_pat = rdmsr(MSR_PAT);
    smp_kernel_cr3 = vmm_kernel_space();
    bsp.lapic_id = mp ? mp->bsp_lapic_id : 0;
    if (!mp || mp->cpu_count <= 1) {
        return;
    }
    unsigned wanted = 0;
    for (uint64_t i = 0; i < mp->cpu_count && cpu_total < MAX_CPUS; i++) {
        struct limine_mp_info *info = mp->cpus[i];
        if (info->lapic_id == mp->bsp_lapic_id) {
            continue;
        }
        struct cpu *c = kzalloc(sizeof(*c));
        uint64_t stack = vmm_alloc_kernel_stack(AP_STACK_SIZE);
        uint64_t df_stack = vmm_alloc_kernel_stack(16384);
        if (!c || !stack || !df_stack) {
            kfree(c);
            kprintf("[WARN] smp: no memory to start CPU with local APIC %u\n", info->lapic_id);
            continue;
        }
        c->self = c;
        c->index = cpu_total;
        c->lapic_id = info->lapic_id;
        c->kernel_rsp = stack;
        c->double_fault_stack = df_stack;
        cpus[cpu_total++] = c;
        info->extra_argument = (uint64_t)c;
        __atomic_store_n(&info->goto_address, (limine_goto_address)ap_start, __ATOMIC_SEQ_CST);
        wanted++;
    }
    /* They must be off the bootloader's memory before it's taken back. Give them a second (timed with
       the PIT: the timer isn't running yet); any that haven't parked by then are left out. */
    for (int ms = 0; ms < 1000 && __atomic_load_n(&aps_parked, __ATOMIC_ACQUIRE) < wanted; ms += 10) {
        pit_oneshot_start(10000);
        pit_oneshot_wait();
    }
    if (aps_parked < wanted) {
        for (unsigned i = 1; i < cpu_total; i++) {
            if (cpus[i]->stage < 5) {
                cpus[i]->left_out = true;
                kprintf("[WARN] smp: CPU %u (local APIC %u) didn't start (stopped at step %u of 5); "
                        "going on without it\n", i, cpus[i]->lapic_id, cpus[i]->stage);
            }
        }
    }
}

void smp_release_aps(void) {
    if (cpu_total <= 1) {
        return;
    }
    unsigned expected = 1;
    for (unsigned i = 1; i < cpu_total; i++) {
        expected += !cpus[i]->left_out;
    }
    __atomic_store_n(&aps_released, true, __ATOMIC_RELEASE);
    /* Wait (letting them have the lock) until they're all up. */
    uint64_t start = uptime_ms();
    while (cpus_online < expected && uptime_ms() - start < 2000) {
        sched_sleep_ms(5);
    }
}
