#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the loader found out about a program. */
struct elf_info {
    uint64_t entry;          /* where it starts */
    uint64_t image_end;      /* end of the highest segment (where the heap can start) */
    uint64_t phdr;           /* the program headers in memory (0 if not loaded); for AT_PHDR */
    uint16_t phnum;
    bool linux_abi;          /* marked ELFOSABI_LINUX: a Linux program (proc/linux.c) */
};

/*
 * Map the PT_LOAD segments of an x86_64 ELF executable into the user address space `space`: a static
 * one where it says, a position-independent one at 4 MiB with the shared libraries it needs
 * (proc/dynlink.c). Returns 0 and fills *info, or -ENOEXEC/-ENOMEM.
 */
int elf_load(uint64_t space, const uint8_t *image, size_t size, struct elf_info *info);
