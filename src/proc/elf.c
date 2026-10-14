#include <stdbool.h>

#include "proc/elf.h"
#include "proc/dynlink.h"
#include "lib/errno.h"
#include "lib/string.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

struct elf64_header {
    uint8_t  ident[16];      /* "\x7f" "ELF", class, data, version, ... */
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;          /* program header table offset */
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct elf64_phdr {
    uint32_t type;
    uint32_t flags;          /* PF_X, PF_W, PF_R */
    uint64_t offset;         /* where the segment's bytes are in the file */
    uint64_t vaddr;          /* where they go in memory */
    uint64_t paddr;
    uint64_t filesz;         /* bytes in the file... */
    uint64_t memsz;          /* ...and in memory; the rest is zeroed (.bss) */
    uint64_t align;
};

#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ET_EXEC     2
#define ET_DYN      3   /* position-independent: our programs (they use the shared libraries) */
#define EM_X86_64   62
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define ELFOSABI_LINUX 3   /* set by scripts/build_ports.sh on the Linux programs it builds */
#define PF_X        1
#define PF_W        2

#define USER_LOAD_MIN 0x10000   /* keep low memory unmapped so NULL faults */
#define PIE_BASE      0x400000  /* where a position-independent program goes */

static bool header_ok(const struct elf64_header *h, size_t size) {
    return size >= sizeof(*h) &&
           memcmp(h->ident, "\x7f" "ELF", 4) == 0 &&
           h->ident[4] == ELFCLASS64 &&
           h->ident[5] == ELFDATA2LSB &&
           (h->type == ET_EXEC || h->type == ET_DYN) &&
           h->machine == EM_X86_64 &&
           h->phentsize == sizeof(struct elf64_phdr) &&
           h->phoff < size &&
           h->phnum <= (size - h->phoff) / sizeof(struct elf64_phdr);
}

static int load_segment(uint64_t space, const uint8_t *image, const struct elf64_phdr *ph, uint64_t base) {
    uint32_t flags = VMM_USER;
    if (ph->flags & PF_W) {
        flags |= VMM_WRITE;
    }
    if (ph->flags & PF_X) {
        flags |= VMM_EXEC;
    }

    uint64_t vaddr = ph->vaddr + base;
    uint64_t start = ALIGN_DOWN(vaddr, PAGE_SIZE);
    uint64_t end = ALIGN_UP(vaddr + ph->memsz, PAGE_SIZE);
    uint64_t file_end = vaddr + ph->filesz;   /* bytes past this are zero */

    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t phys;
        if (!vmm_user_lookup(space, va, false, &phys)) {
            phys = pmm_alloc_page();
            if (!phys) {
                return -ENOMEM;
            }
            memset(phys_to_virt(phys), 0, PAGE_SIZE);
        }
        /* (Re)map with this segment's permissions; a page shared by two
           segments keeps its contents and gets the later one's flags. */
        if (!vmm_map_user(space, va, ALIGN_DOWN(phys, PAGE_SIZE), flags)) {
            return -ENOMEM;
        }

        /* Copy the part of the file that falls inside this page, through the direct map. */
        uint64_t copy_from = va > vaddr ? va : vaddr;
        uint64_t copy_to = va + PAGE_SIZE < file_end ? va + PAGE_SIZE : file_end;
        if (copy_from < copy_to) {
            uint8_t *dst = (uint8_t *)phys_to_virt(ALIGN_DOWN(phys, PAGE_SIZE)) + (copy_from - va);
            memcpy(dst, image + ph->offset + (copy_from - vaddr), copy_to - copy_from);
        }
    }
    return 0;
}

int elf_load(uint64_t space, const uint8_t *image, size_t size, struct elf_info *info) {
    memset(info, 0, sizeof(*info));
    uint64_t *image_end = &info->image_end;
    const struct elf64_header *h = (const struct elf64_header *)image;
    if (!header_ok(h, size)) {
        return -ENOEXEC;
    }
    const struct elf64_phdr *phdrs = (const struct elf64_phdr *)(image + h->phoff);
    info->linux_abi = h->ident[7] == ELFOSABI_LINUX;
    info->phnum = h->phnum;
    uint64_t base = h->type == ET_DYN ? PIE_BASE : 0;
    bool dynamic = false;

    bool entry_in_segment = false;
    for (uint16_t i = 0; i < h->phnum; i++) {
        const struct elf64_phdr *ph = &phdrs[i];
        dynamic |= ph->type == PT_DYNAMIC;
        if (ph->type != PT_LOAD || ph->memsz == 0) {
            continue;
        }
        /* Everything must come from inside the file and land in user space. */
        if (ph->filesz > ph->memsz || ph->offset > size || ph->filesz > size - ph->offset ||
            ph->vaddr + base < USER_LOAD_MIN || ph->vaddr + ph->memsz < ph->vaddr ||
            ph->vaddr + base + ph->memsz > USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE) {
            return -ENOEXEC;
        }
        int err = load_segment(space, image, ph, base);
        if (err) {
            return err;
        }
        if (base + ph->vaddr + ph->memsz > *image_end) {
            *image_end = base + ph->vaddr + ph->memsz;
        }
        /* The program headers are in memory if a segment loads that part of the file. */
        if (h->phoff >= ph->offset && h->phoff + (uint64_t)h->phnum * sizeof(struct elf64_phdr) <= ph->offset + ph->filesz) {
            info->phdr = base + ph->vaddr + (h->phoff - ph->offset);
        }
        if (h->entry >= ph->vaddr && h->entry < ph->vaddr + ph->memsz && (ph->flags & PF_X)) {
            entry_in_segment = true;
        }
    }
    if (!entry_in_segment) {
        return -ENOEXEC;
    }
    info->entry = base + h->entry;
    /* A program that uses shared libraries: those, and its references into them. */
    if (dynamic && !info->linux_abi) {
        int err = dynlink_program(space, image, size, base);
        if (err) {
            return err;
        }
    }
    return 0;
}
