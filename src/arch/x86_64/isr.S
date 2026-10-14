/*
 * Interrupt entry stubs for all 256 vectors.
 *
 * Every stub leaves the stack in the same shape before jumping to isr_common:
 * the CPU pushed SS, RSP, RFLAGS, CS, RIP (and an error code for some
 * exceptions); stubs for vectors without an error code push a dummy 0 so that
 * struct interrupt_frame (idt.h) always lines up. Then the vector number.
 */

.section .text

.macro ISR_NOERR vec
isr_stub_\vec:
    pushq $0
    pushq $\vec
    jmp isr_common
.endm

.macro ISR_ERR vec
isr_stub_\vec:
    pushq $\vec
    jmp isr_common
.endm

/* Exceptions that push an error code: 8, 10-14, 17, 21, 29, 30. */
.irp v, 0,1,2,3,4,5,6,7,9,15,16,18,19,20,22,23,24,25,26,27,28,31
    ISR_NOERR \v
.endr
.irp v, 8,10,11,12,13,14,17,21,29,30
    ISR_ERR \v
.endr
/* Everything from 32 up is an interrupt, never with an error code. */
.altmacro
.set vec, 32
.rept 256 - 32
    ISR_NOERR %vec
    .set vec, vec + 1
.endr

isr_common:
    /* From user mode (CS's low bits, 24 bytes up: vector, error code, RIP, CS): the kernel's GS. */
    testb $3, 24(%rsp)
    jz 1f
    swapgs
1:
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

    /* 22 qwords are on the stack now (176 bytes), so RSP is still 16-byte aligned. */
    movq %rsp, %rdi
    cld
    call interrupt_dispatch

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

    addq $16, %rsp      /* drop vector + error code */
    testb $3, 8(%rsp)   /* back to user mode (CS, above RIP): the program's GS */
    jz 2f
    swapgs
2:
    iretq

.macro STUB_ADDRESS vec
    .quad isr_stub_\vec
.endm

.section .rodata
.global isr_stub_table
.balign 8
isr_stub_table:
.set vec, 0
.rept 256
    STUB_ADDRESS %vec
    .set vec, vec + 1
.endr

.section .note.GNU-stack, "", @progbits
