/*
 * The dynamic linker, in the kernel (see dynlink.h). What it handles is what our toolchain produces
 * for x86_64 position-independent code: RELA relocations of types RELATIVE, 64, GLOB_DAT and
 * JUMP_SLOT, symbols found through the SysV hash table (linked --hash-style=sysv). The relocation
 * rules follow the System V x86_64 ABI (as musl's ldso/dynlink.c applies them).
 */

#include "proc/dynlink.h"
#include "fs/vfs.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sync.h"
#include <paths.h>

#define LIBRARY_DIR    _PATH_LIB "/"
#define LIBRARY_BASE   0x0000300000000000ULL   /* libraries go up from here, one after another */
#define LIBRARY_GAP    (2ULL << 20)
#define MAX_DEPS       16
#define MAX_SCOPE      64

/* --- ELF structures ------------------------------------------------------------------------------- */

struct ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

struct dyn {
    int64_t tag;
    uint64_t val;
};

struct sym {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
};

struct rela {
    uint64_t offset, info;
    int64_t addend;
};

#define ET_DYN      3
#define EM_X86_64   62
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PF_X        1
#define PF_W        2

#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_STRSZ    10
#define DT_TEXTREL  22
#define DT_JMPREL   23
#define DT_INIT_ARRAYSZ 27

#define R_X86_64_NONE      0
#define R_X86_64_64        1
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8

#define STB_WEAK 2
#define SHN_UNDEF 0

/* --- an object's dynamic information, read from its file -------------------------------------------- */

struct tables {
    struct sym *syms;
    uint32_t nsyms;
    char *strs;
    uint64_t strsz;
    uint32_t nbucket;
    uint32_t *buckets, *chains;
    struct rela *rela, *jmprel;
    uint64_t nrela, njmprel;
    uint64_t needed[MAX_DEPS];   /* DT_NEEDED name offsets in strs */
    int nneeded;
};

/* Where a virtual address (relative to the object's own 0) is in the file, for `len` bytes. */
static const void *file_at(const uint8_t *image, size_t size, uint64_t vaddr, uint64_t len) {
    const struct ehdr *h = (const struct ehdr *)image;
    const struct phdr *ph = (const struct phdr *)(image + h->phoff);
    for (uint16_t i = 0; i < h->phnum; i++) {
        if (ph[i].type == PT_LOAD && vaddr >= ph[i].vaddr && vaddr - ph[i].vaddr <= ph[i].filesz &&
            len <= ph[i].filesz - (vaddr - ph[i].vaddr)) {
            uint64_t off = ph[i].offset + (vaddr - ph[i].vaddr);
            return off <= size && len <= size - off ? image + off : NULL;
        }
    }
    return NULL;
}

/* Copy the dynamic tables out of the file (`copy`: into memory of their own, for a library that
   stays; else they point into the image). */
static int read_tables(const uint8_t *image, size_t size, struct tables *t, bool copy) {
    memset(t, 0, sizeof(*t));
    const struct ehdr *h = (const struct ehdr *)image;
    const struct phdr *ph = (const struct phdr *)(image + h->phoff);
    const struct dyn *dyn = NULL;
    uint64_t ndyn = 0;
    for (uint16_t i = 0; i < h->phnum; i++) {
        if (ph[i].type == PT_DYNAMIC) {
            dyn = file_at(image, size, ph[i].vaddr, ph[i].filesz);
            ndyn = ph[i].filesz / sizeof(struct dyn);
        }
    }
    if (!dyn) {
        return -ENOEXEC;
    }
    uint64_t strtab = 0, symtab = 0, hash = 0, rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0;
    for (uint64_t i = 0; i < ndyn && dyn[i].tag != DT_NULL; i++) {
        switch (dyn[i].tag) {
        case DT_NEEDED:
            if (t->nneeded < MAX_DEPS) {
                t->needed[t->nneeded++] = dyn[i].val;
            }
            break;
        case DT_STRTAB:   strtab = dyn[i].val; break;
        case DT_STRSZ:    t->strsz = dyn[i].val; break;
        case DT_SYMTAB:   symtab = dyn[i].val; break;
        case DT_HASH:     hash = dyn[i].val; break;
        case DT_RELA:     rela = dyn[i].val; break;
        case DT_RELASZ:   relasz = dyn[i].val; break;
        case DT_JMPREL:   jmprel = dyn[i].val; break;
        case DT_PLTRELSZ: pltrelsz = dyn[i].val; break;
        case DT_TEXTREL:
            kprintf("[WARN] dynlink: code that needs fixing up (not built position-independent)\n");
            return -ENOEXEC;
        case DT_INIT_ARRAYSZ:
            if (dyn[i].val) {
                kprintf("[WARN] dynlink: constructors aren't supported\n");
                return -ENOEXEC;
            }
            break;
        }
    }
    const uint32_t *hdr = hash ? file_at(image, size, hash, 8) : NULL;
    if (!strtab || !symtab || !hdr) {
        kprintf("[WARN] dynlink: no symbol table or hash table (link with --hash-style=sysv)\n");
        return -ENOEXEC;
    }
    t->nbucket = hdr[0];
    t->nsyms = hdr[1];
    const void *hashes = file_at(image, size, hash, 8 + 4ULL * (t->nbucket + t->nsyms));
    const void *syms = file_at(image, size, symtab, (uint64_t)t->nsyms * sizeof(struct sym));
    const void *strs = file_at(image, size, strtab, t->strsz);
    const void *relas = relasz ? file_at(image, size, rela, relasz) : NULL;
    const void *jmps = pltrelsz ? file_at(image, size, jmprel, pltrelsz) : NULL;
    if (!hashes || !syms || !strs || (relasz && !relas) || (pltrelsz && !jmps)) {
        return -ENOEXEC;
    }
    t->nrela = relasz / sizeof(struct rela);
    t->njmprel = pltrelsz / sizeof(struct rela);
    if (!copy) {
        t->buckets = (uint32_t *)hashes + 2;
        t->chains = t->buckets + t->nbucket;
        t->syms = (struct sym *)syms;
        t->strs = (char *)strs;
        t->rela = (struct rela *)relas;
        t->jmprel = (struct rela *)jmps;
        return 0;
    }
    /* A library keeps its symbols and hash table (for programs that start later); relocations are
       only needed once, now. */
    t->buckets = kmalloc(4ULL * (t->nbucket + t->nsyms));
    t->syms = kmalloc((size_t)t->nsyms * sizeof(struct sym));
    t->strs = kmalloc(t->strsz + 1);
    if (!t->buckets || !t->syms || !t->strs) {
        kfree(t->buckets);
        kfree(t->syms);
        kfree(t->strs);
        return -ENOMEM;
    }
    memcpy(t->buckets, (const uint32_t *)hashes + 2, 4ULL * (t->nbucket + t->nsyms));
    t->chains = t->buckets + t->nbucket;
    memcpy(t->syms, syms, (size_t)t->nsyms * sizeof(struct sym));
    memcpy(t->strs, strs, t->strsz);
    t->strs[t->strsz] = '\0';
    t->rela = (struct rela *)relas;   /* (still in the image: used before it's freed) */
    t->jmprel = (struct rela *)jmps;
    return 0;
}

static uint32_t sysv_hash(const char *name) {
    uint32_t h = 0;
    for (const uint8_t *s = (const uint8_t *)name; *s; s++) {
        h = (h << 4) + *s;
        uint32_t g = h & 0xF0000000;
        if (g) {
            h ^= g >> 24;
        }
        h &= ~g;
    }
    return h;
}

/* The object's own definition of `name` (its value, relative to the object), or NULL. */
static const struct sym *find_defined(const struct tables *t, const char *name, uint32_t hash) {
    if (!t->nbucket) {
        return NULL;
    }
    for (uint32_t i = t->buckets[hash % t->nbucket]; i && i < t->nsyms; i = t->chains[i]) {
        const struct sym *s = &t->syms[i];
        if (s->shndx != SHN_UNDEF && s->name < t->strsz && strcmp(t->strs + s->name, name) == 0) {
            return s;
        }
    }
    return NULL;
}

/* --- libraries --------------------------------------------------------------------------------------- */

struct library {
    char name[64];
    uint64_t base;
    uint64_t pages;
    uint64_t *frames;            /* per page of [base, base + pages * 4K): 0 for a hole */
    uint8_t *page_flags;         /* PF_W / PF_X per page */
    struct tables t;
    struct library *deps[MAX_DEPS];
    int ndeps;
    struct library *next;
};

static struct mutex lock = MUTEX_INIT;
static struct library *libraries;
static uint64_t next_base = LIBRARY_BASE;

/* Everything `roots` use, in search order: the roots, then breadth first what they need. */
static int scope_of(struct library **roots, int nroots, struct library **out) {
    int n = 0;
    for (int i = 0; i < nroots && n < MAX_SCOPE; i++) {
        out[n++] = roots[i];
    }
    for (int i = 0; i < n; i++) {
        for (int d = 0; d < out[i]->ndeps; d++) {
            bool seen = false;
            for (int k = 0; k < n && !seen; k++) {
                seen = out[k] == out[i]->deps[d];
            }
            if (!seen && n < MAX_SCOPE) {
                out[n++] = out[i]->deps[d];
            }
        }
    }
    return n;
}

static bool lookup(struct library **scope, int n, const char *name, uint64_t *addr) {
    uint32_t hash = sysv_hash(name);
    for (int i = 0; i < n; i++) {
        const struct sym *s = find_defined(&scope[i]->t, name, hash);
        if (s) {
            *addr = scope[i]->base + s->value;
            return true;
        }
    }
    return false;
}

/* The value a relocation puts at its place, or an error for a symbol nobody defines. `self` resolves
   the object's own symbols (base, tables); `scope` the libraries it uses. */
static int resolve(const struct rela *r, uint64_t base, const struct tables *self, struct library **scope, int n,
                   const char *who, uint64_t *value) {
    uint32_t type = (uint32_t)r->info, index = (uint32_t)(r->info >> 32);
    if (type == R_X86_64_RELATIVE) {
        *value = base + (uint64_t)r->addend;
        return 0;
    }
    if (type != R_X86_64_64 && type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT) {
        kprintf("[WARN] dynlink: %s: relocation type %u isn't supported\n", who, type);
        return -ENOEXEC;
    }
    if (index >= self->nsyms) {
        return -ENOEXEC;
    }
    const struct sym *s = &self->syms[index];
    const char *name = s->name < self->strsz ? self->strs + s->name : "";
    uint64_t addr;
    if (s->shndx != SHN_UNDEF) {
        addr = base + s->value;   /* its own (-Bsymbolic; and a program's own symbols) */
    } else if (!lookup(scope, n, name, &addr)) {
        if ((s->info >> 4) == STB_WEAK) {
            addr = 0;
        } else {
            kprintf("[WARN] dynlink: %s needs '%s', which no library has\n", who, name);
            return -ENOEXEC;
        }
    }
    *value = type == R_X86_64_64 ? addr + (uint64_t)r->addend : addr;
    return 0;
}

static int read_file(const char *path, uint8_t **out, size_t *size) {
    struct vnode *n;
    int err = vfs_resolve(NULL, path, &n);
    if (err) {
        return err;
    }
    uint8_t *buf = n->size ? kmalloc(n->size) : NULL;
    int64_t got = buf ? vfs_read(n, 0, buf, n->size) : -ENOMEM;
    uint64_t want = n->size;
    vfs_put(n);
    if (got < 0 || (uint64_t)got != want) {
        kfree(buf);
        return got < 0 ? (int)got : -EIO;
    }
    *out = buf;
    *size = want;
    return 0;
}

static bool lib_header_ok(const uint8_t *image, size_t size) {
    const struct ehdr *h = (const struct ehdr *)image;
    return size >= sizeof(*h) && memcmp(h->ident, "\x7f" "ELF", 4) == 0 && h->ident[4] == 2 && h->type == ET_DYN &&
           h->machine == EM_X86_64 && h->phentsize == sizeof(struct phdr) && h->phoff < size &&
           h->phnum <= (size - h->phoff) / sizeof(struct phdr);
}

static struct library *lib_get(const char *name, int depth, int *err);

/* Load a library the first time it's needed: its pages, then what it needs, then its fix-ups. */
static struct library *lib_load(const char *name, int depth, int *err) {
    char path[VFS_PATH_MAX];
    ksnprintf(path, sizeof(path), LIBRARY_DIR "%s", name);
    uint8_t *image = NULL;
    size_t size = 0;
    *err = read_file(path, &image, &size);
    if (*err) {
        kprintf("[WARN] dynlink: can't read %s: %s\n", path, strerror(*err));
        return NULL;
    }
    struct library *l = NULL;
    if (!lib_header_ok(image, size)) {
        kprintf("[WARN] dynlink: %s isn't a shared library\n", path);
        *err = -ENOEXEC;
        goto out;
    }
    const struct ehdr *h = (const struct ehdr *)image;
    const struct phdr *ph = (const struct phdr *)(image + h->phoff);
    uint64_t span = 0;
    for (uint16_t i = 0; i < h->phnum; i++) {
        if (ph[i].type == PT_LOAD) {
            if (ph[i].filesz > ph[i].memsz || ph[i].offset > size || ph[i].filesz > size - ph[i].offset ||
                ph[i].vaddr + ph[i].memsz < ph[i].vaddr || ph[i].vaddr + ph[i].memsz > (1ULL << 32)) {
                *err = -ENOEXEC;
                goto out;
            }
            if (ph[i].vaddr + ph[i].memsz > span) {
                span = ph[i].vaddr + ph[i].memsz;
            }
        }
    }
    l = kzalloc(sizeof(*l));
    if (!l) {
        *err = -ENOMEM;
        goto out;
    }
    strlcpy(l->name, name, sizeof(l->name));
    l->pages = ALIGN_UP(span, PAGE_SIZE) / PAGE_SIZE;
    l->frames = kzalloc(l->pages * sizeof(uint64_t));
    l->page_flags = kzalloc(l->pages);
    if (!l->frames || !l->page_flags) {
        *err = -ENOMEM;
        goto fail;
    }
    l->base = next_base;
    next_base += ALIGN_UP(l->pages * PAGE_SIZE, LIBRARY_GAP) + LIBRARY_GAP;

    /* Its pages, as they'll be in every process (before the fix-ups). */
    for (uint16_t i = 0; i < h->phnum; i++) {
        if (ph[i].type != PT_LOAD || !ph[i].memsz) {
            continue;
        }
        for (uint64_t va = ALIGN_DOWN(ph[i].vaddr, PAGE_SIZE); va < ph[i].vaddr + ph[i].memsz; va += PAGE_SIZE) {
            uint64_t page = va / PAGE_SIZE;
            if (!l->frames[page]) {
                uint64_t phys = pmm_alloc_page();
                if (!phys) {
                    *err = -ENOMEM;
                    goto fail;
                }
                memset(phys_to_virt(phys), 0, PAGE_SIZE);
                l->frames[page] = phys;
            }
            l->page_flags[page] |= (uint8_t)(ph[i].flags & (PF_W | PF_X));
            uint64_t from = va > ph[i].vaddr ? va : ph[i].vaddr;
            uint64_t to = va + PAGE_SIZE < ph[i].vaddr + ph[i].filesz ? va + PAGE_SIZE : ph[i].vaddr + ph[i].filesz;
            if (from < to) {
                memcpy((uint8_t *)phys_to_virt(l->frames[page]) + (from - va), image + ph[i].offset + (from - ph[i].vaddr),
                       to - from);
            }
        }
    }
    *err = read_tables(image, size, &l->t, true);
    if (*err) {
        kprintf("[WARN] dynlink: %s: bad dynamic section\n", path);
        goto fail;
    }
    /* Registered before what it needs is loaded, so a loop of libraries ends. */
    l->next = libraries;
    libraries = l;

    for (int i = 0; i < l->t.nneeded; i++) {
        const char *dep = l->t.needed[i] < l->t.strsz ? l->t.strs + l->t.needed[i] : "";
        struct library *d = lib_get(dep, depth + 1, err);
        if (!d) {
            kprintf("[WARN] dynlink: %s needs %s\n", name, dep);
            goto unregister;
        }
        l->deps[l->ndeps++] = d;
    }

    /* Fix-ups, into its page images (the same in every process: it's always at l->base). */
    struct library *scope[MAX_SCOPE];
    struct library *self = l;
    int n = scope_of(&self, 1, scope);
    for (int pass = 0; pass < 2; pass++) {
        const struct rela *r = pass ? l->t.jmprel : l->t.rela;
        uint64_t count = pass ? l->t.njmprel : l->t.nrela;
        for (uint64_t i = 0; i < count; i++) {
            if ((uint32_t)r[i].info == R_X86_64_NONE) {
                continue;
            }
            uint64_t value;
            *err = resolve(&r[i], l->base, &l->t, scope, n, name, &value);
            if (*err) {
                goto unregister;
            }
            uint64_t page = r[i].offset / PAGE_SIZE, at = r[i].offset % PAGE_SIZE;
            if (page >= l->pages || !l->frames[page] || at > PAGE_SIZE - 8) {
                *err = -ENOEXEC;
                goto unregister;
            }
            *(uint64_t *)((uint8_t *)phys_to_virt(l->frames[page]) + at) = value;
        }
    }
    l->t.rela = l->t.jmprel = NULL;   /* (pointed into the image) */
    kprintf("       Library: %s at 0x%lx (%lu KiB)\n", name, l->base, l->pages * 4);
    kfree(image);
    return l;

unregister:
    libraries = l->next;
fail:
    if (l->frames) {
        for (uint64_t i = 0; i < l->pages; i++) {
            if (l->frames[i]) {
                pmm_free_page(l->frames[i]);
            }
        }
    }
    kfree(l->frames);
    kfree(l->page_flags);
    kfree(l);
    l = NULL;
out:
    kfree(image);
    return NULL;
}

static struct library *lib_get(const char *name, int depth, int *err) {
    for (struct library *l = libraries; l; l = l->next) {
        if (strcmp(l->name, name) == 0) {
            return l;
        }
    }
    bool plain = name[0] != '\0';
    for (const char *c = name; *c; c++) {
        plain &= *c != '/';   /* only names in /usr/lib */
    }
    if (depth > 8 || !plain) {
        *err = -ENOEXEC;
        return NULL;
    }
    return lib_load(name, depth, err);
}

/* Put a library into a process: its code shared, its data a copy of its own. */
static int map_library(uint64_t space, struct library *l) {
    for (uint64_t i = 0; i < l->pages; i++) {
        if (!l->frames[i]) {
            continue;
        }
        uint64_t va = l->base + i * PAGE_SIZE;
        uint32_t flags = VMM_USER | (l->page_flags[i] & PF_X ? VMM_EXEC : 0);
        uint64_t phys = l->frames[i];
        if (l->page_flags[i] & PF_W) {
            phys = pmm_alloc_page();
            if (!phys) {
                return -ENOMEM;
            }
            memcpy(phys_to_virt(phys), phys_to_virt(l->frames[i]), PAGE_SIZE);
            flags |= VMM_WRITE;
        } else {
            flags |= VMM_DEVICE;   /* the library's own page: never freed with the process */
        }
        if (!vmm_map_user(space, va, phys, flags)) {
            if (phys != l->frames[i]) {
                pmm_free_page(phys);
            }
            return -ENOMEM;
        }
    }
    return 0;
}

int dynlink_program(uint64_t space, const uint8_t *image, size_t size, uint64_t base) {
    struct tables t;
    int err = read_tables(image, size, &t, false);
    if (err) {
        return err;
    }
    mutex_lock(&lock);
    struct library *needed[MAX_DEPS];
    int nneeded = 0;
    for (int i = 0; i < t.nneeded && !err; i++) {
        const char *name = t.needed[i] < t.strsz ? t.strs + t.needed[i] : "";
        needed[nneeded] = lib_get(name, 0, &err);
        if (!needed[nneeded]) {
            kprintf("[WARN] dynlink: a program needs %s\n", name);
            err = err ? err : -ENOEXEC;
        } else {
            nneeded++;
        }
    }
    struct library *scope[MAX_SCOPE];
    int n = err ? 0 : scope_of(needed, nneeded, scope);
    for (int i = 0; i < n && !err; i++) {
        err = map_library(space, scope[i]);
    }
    mutex_unlock(&lock);

    /* The program's own fix-ups, into its pages. */
    for (int pass = 0; pass < 2 && !err; pass++) {
        const struct rela *r = pass ? t.jmprel : t.rela;
        uint64_t count = pass ? t.njmprel : t.nrela;
        for (uint64_t i = 0; i < count && !err; i++) {
            if ((uint32_t)r[i].info == R_X86_64_NONE) {
                continue;
            }
            uint64_t value, phys;
            err = resolve(&r[i], base, &t, scope, n, "a program", &value);
            if (err) {
                break;
            }
            uint64_t va = base + r[i].offset;
            if (va % 8 || !vmm_user_lookup(space, va, false, &phys)) {
                err = -ENOEXEC;
                break;
            }
            *(uint64_t *)phys_to_virt(phys) = value;
        }
    }
    return err;
}
