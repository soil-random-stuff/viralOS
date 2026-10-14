/*
 * Program entry. The kernel starts us with RSP pointing at argc, followed
 * by the argv pointers (see setup_stack in the kernel's process.c).
 */
.section .text.start
.global _start
_start:
    xorl %ebp, %ebp          /* outermost frame */
    movq (%rsp), %rdi        /* argc */
    leaq 8(%rsp), %rsi       /* argv */
    call main
    movl %eax, %edi
    call exit                /* never returns */
    ud2

.section .note.GNU-stack, "", @progbits
