#include "arch/x86_64/syscall.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/smp.h"

#define MSR_EFER           0xC0000080
#define MSR_STAR           0xC0000081
#define MSR_LSTAR          0xC0000082
#define MSR_SFMASK         0xC0000084
#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

#define EFER_SCE (1ULL << 0)   /* enable syscall/sysret */

/* RFLAGS bits cleared on syscall entry: IF (no interrupts until we're on
   the kernel stack), DF, TF and AC. */
#define SFMASK_BITS ((1ULL << 9) | (1ULL << 10) | (1ULL << 8) | (1ULL << 18))

void syscall_entry(void);

void syscall_init(void) {
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

    /*
     * STAR[47:32]: selector loaded into CS on syscall (SS = that + 8).
     * STAR[63:48]: base for sysret (CS = base + 16, SS = base + 8), which
     * is why the GDT has user data right below user code. We return with
     * iretq, but keep sysret valid anyway.
     */
    uint64_t star = ((uint64_t)GDT_KERNEL_CODE << 32) | ((uint64_t)(GDT_KERNEL_DATA | 3) << 48);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, SFMASK_BITS);
}

void syscall_set_gs(struct cpu *cpu) {
    /* In the kernel GS points at this CPU's struct cpu; programs get GS base 0 (swapgs in user.S). */
    wrmsr(MSR_GS_BASE, (uint64_t)cpu);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

void enter_user_mode_asm(uint64_t rip, uint64_t rsp);   /* user.S */

void enter_user_mode(uint64_t rip, uint64_t rsp) {
    interrupts_disable();
    kernel_unlock();   /* programs run without the kernel lock */
    enter_user_mode_asm(rip, rsp);
    __builtin_unreachable();
}

void syscall_set_kernel_stack(uint64_t stack_top) {
    this_cpu()->kernel_rsp = stack_top;
}
