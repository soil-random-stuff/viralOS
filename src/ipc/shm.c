/*
 * Shared memory objects: a list of physical pages, mapped into each
 * process that asks (at a different address in each, possibly). The pages
 * belong to the object, not to any process: mappings use VMM_DEVICE so an
 * exiting process doesn't free them. The object counts its open handles
 * plus its mappings, and frees the pages when that reaches zero.
 */

#include "ipc/ipc.h"
#include "arch/x86_64/cpu.h"
#include "lib/errno.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/file.h"
#include "proc/process.h"

struct shm {
    uint64_t size;
    uint64_t pages;
    uint64_t *frames;
    uint32_t refs;
};

static void free_pages(struct shm *s, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        pmm_free_page(s->frames[i]);
    }
}

int shm_create(uint64_t size, struct file **out) {
    if (size == 0 || size > OS_SHM_MAX) {
        return -EINVAL;
    }
    struct shm *s = kzalloc(sizeof(*s));
    struct file *f = file_new(FILE_SHM);
    uint64_t pages = ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE;
    uint64_t *frames = kmalloc(sizeof(uint64_t) * pages);
    if (!s || !f || !frames) {
        kfree(s);
        kfree(f);
        kfree(frames);
        return -ENOMEM;
    }
    s->size = size;
    s->pages = pages;
    s->frames = frames;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            free_pages(s, i);
            kfree(frames);
            kfree(s);
            kfree(f);
            return -ENOMEM;
        }
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        frames[i] = phys;
    }
    s->refs = 1;   /* the handle */
    f->object = s;
    *out = f;
    return 0;
}

void shm_put(struct shm *s) {
    uint64_t irq = irq_save();
    bool last = --s->refs == 0;
    irq_restore(irq);
    if (last) {
        free_pages(s, s->pages);
        kfree(s->frames);
        kfree(s);
    }
}

int64_t shm_map(struct process *p, struct file *f, uint64_t *size) {
    struct shm *s = f->object;
    int slot = -1;
    for (int i = 0; i < PROCESS_MAX_SHM; i++) {
        if (!p->shm_maps[i].shm) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -EMFILE;
    }
    if (!p->shm_next) {
        p->shm_next = USER_SHM_BASE;
    }
    uint64_t addr = p->shm_next;
    for (uint64_t i = 0; i < s->pages; i++) {
        if (!vmm_map_user(p->space, addr + i * PAGE_SIZE, s->frames[i], VMM_WRITE | VMM_DEVICE)) {
            uint64_t ignored;
            for (uint64_t j = 0; j < i; j++) {
                vmm_unmap_user(p->space, addr + j * PAGE_SIZE, &ignored);
            }
            return -ENOMEM;
        }
    }
    /* Never reuse addresses, and leave an unmapped page between mappings. */
    p->shm_next += (s->pages + 1) * PAGE_SIZE;

    uint64_t irq = irq_save();
    s->refs++;   /* the mapping */
    irq_restore(irq);
    p->shm_maps[slot] = (struct shm_mapping){ addr, s->pages, s };
    *size = s->size;
    return (int64_t)addr;
}

static void unmap_slot(struct process *p, int slot) {
    struct shm_mapping *m = &p->shm_maps[slot];
    uint64_t ignored;
    for (uint64_t i = 0; i < m->pages; i++) {
        vmm_unmap_user(p->space, m->address + i * PAGE_SIZE, &ignored);
    }
    shm_put(m->shm);
    m->shm = NULL;
}

int shm_unmap(struct process *p, uint64_t address) {
    for (int i = 0; i < PROCESS_MAX_SHM; i++) {
        if (p->shm_maps[i].shm && p->shm_maps[i].address == address) {
            unmap_slot(p, i);
            return 0;
        }
    }
    return -EINVAL;
}

void shm_process_exit(struct process *p) {
    /* The page tables go away with the address space; just drop the references. */
    for (int i = 0; i < PROCESS_MAX_SHM; i++) {
        if (p->shm_maps[i].shm) {
            shm_put(p->shm_maps[i].shm);
            p->shm_maps[i].shm = NULL;
        }
    }
}

int shm_copy(struct file *f, uint64_t offset, void *buf, size_t len, bool into) {
    struct shm *s = f->object;
    if (offset > s->size || len > s->size - offset) {
        return -EINVAL;
    }
    uint8_t *b = buf;
    while (len) {
        uint64_t page = offset / PAGE_SIZE, in = offset % PAGE_SIZE;
        size_t n = PAGE_SIZE - in < len ? (size_t)(PAGE_SIZE - in) : len;
        uint8_t *mem = (uint8_t *)phys_to_virt(s->frames[page]) + in;
        if (into) {
            memcpy(mem, b, n);
        } else {
            memcpy(b, mem, n);
        }
        b += n;
        offset += n;
        len -= n;
    }
    return 0;
}
