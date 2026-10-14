.section .text

/*
 * void context_switch(uint64_t *save_rsp, uint64_t load_rsp)
 *
 * Saves the callee-saved registers on the current stack, stores the stack
 * pointer in *save_rsp, then loads load_rsp and pops the other thread's
 * registers. The final `ret` returns into wherever that thread last called
 * context_switch (or into thread_trampoline for a brand new thread).
 * Everything else (caller-saved registers, RFLAGS) is the caller's business:
 * we're always called from C with interrupts disabled.
 */
.global context_switch
context_switch:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)

    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret

/*
 * First code a new thread runs: thread_create() left its entry function in
 * r12 and argument in r13, so pass them on to the C side.
 */
.global thread_trampoline
thread_trampoline:
    movq %r12, %rdi
    movq %r13, %rsi
    call thread_entry
    ud2

/*
 * void switch_stack_and_call(uint64_t stack_top, void (*fn)(void))
 *
 * Moves onto a new stack and calls fn there; never returns. RBP is cleared
 * so backtraces stop at fn instead of wandering into the old stack.
 */
.global switch_stack_and_call
switch_stack_and_call:
    movq %rdi, %rsp
    xorl %ebp, %ebp
    call *%rsi
1:
    cli
    hlt
    jmp 1b

.section .note.GNU-stack, "", @progbits
