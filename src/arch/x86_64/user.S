/*
 * Crossing between ring 3 (user programs) and ring 0 (kernel).
 *
 * GS: while the CPU runs kernel code, its GS base points at its struct cpu
 * (smp.h); while it runs a program, at 0 (the program's). `swapgs` exchanges
 * the two on every crossing: here, in isr.S for interrupts from user mode.
 */

#define USER_CS 0x23   /* GDT_USER_CODE | ring 3 */
#define USER_SS 0x1B   /* GDT_USER_DATA | ring 3 */

/* struct cpu offsets */
#define CPU_KERNEL_RSP 0
#define CPU_USER_RSP   8

.section .text

/*
 * Entry point of the `syscall` instruction. The CPU has put the user RIP in
 * RCX and RFLAGS in R11, masked interrupts (see SFMASK), switched CS/SS to
 * the kernel ones, and done nothing else: RSP is still the user's stack.
 *
 * We build the same struct interrupt_frame an interrupt would, so all
 * kernel code sees one frame layout, and leave through iretq.
 */
.global syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:CPU_USER_RSP
    movq %gs:CPU_KERNEL_RSP, %rsp

    pushq $USER_SS
    pushq %gs:CPU_USER_RSP
    pushq %r11                  /* user RFLAGS */
    pushq $USER_CS
    pushq %rcx                  /* user RIP */

    pushq $0                    /* error code */
    pushq $0x80                 /* "vector": marks a syscall frame */

    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %rdi
    xorl %ebp, %ebp             /* backtraces stop here */
    cld
    call syscall_dispatch

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    cli                         /* (the frame's RFLAGS turns them back on) */
    swapgs                      /* back to the program's GS */
    iretq

/*
 * void enter_user_mode_asm(uint64_t rip, uint64_t rsp)
 *
 * Start running user code: fake the frame an interrupt from ring 3 would
 * have pushed and "return" to it. Registers are cleared so no kernel values
 * leak into the program.
 */
.global enter_user_mode_asm
enter_user_mode_asm:
    cli
    pushq $USER_SS
    pushq %rsi
    pushq $0x202                /* RFLAGS: interrupts enabled */
    pushq $USER_CS
    pushq %rdi

    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %edi, %edi
    xorl %ebp, %ebp
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    swapgs                      /* the program's GS */
    iretq

.section .note.GNU-stack, "", @progbits
