#pragma once

/*
 * Running Linux programs: static x86_64 binaries built against musl (the
 * disk tools: mkfs.*, e2fsprogs, ntfsprogs-plus, see scripts/build_ports.sh),
 * marked ELFOSABI_LINUX in their header. They get a Linux start-up stack
 * (argv, environment, auxiliary vector), Linux system call numbers, anonymous
 * mmap, FS-based thread-local storage, and the block device ioctls the tools
 * use on /dev files. Only what those programs need: no fork/exec,
 * signals or sockets.
 */

#include <stdint.h>

struct process;
struct elf_info;
struct interrupt_frame;

int linux_setup_stack(struct process *p, int argc, char **argv, const struct elf_info *info);
/* Handle the system call in the frame (number in rax); returns the result for rax. */
int64_t linux_syscall(struct process *p, struct interrupt_frame *f);
/* Free the process's mapping list (its pages go with the address space). */
void linux_free_maps(struct process *p);
