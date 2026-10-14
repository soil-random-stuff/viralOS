#pragma once

#include <stdint.h>

struct cpu;

/* Program this CPU's MSRs that make the `syscall` instruction enter the kernel. */
void syscall_init(void);
/* Point this CPU's GS at `cpu` for kernel code (user code sees GS base 0). */
void syscall_set_gs(struct cpu *cpu);

/* Kernel stack the next syscall from user mode should run on. */
void syscall_set_kernel_stack(uint64_t stack_top);

/* Start executing user code at `rip` with stack `rsp` (lets go of the kernel lock). */
__attribute__((noreturn)) void enter_user_mode(uint64_t rip, uint64_t rsp);

#define SYSCALL_FRAME_VECTOR 0x80   /* interrupt_frame.vector of a syscall */
