#include "proc/usercopy.h"
#include "lib/errno.h"
#include "lib/string.h"
#include "mm/mm.h"
#include "mm/vmm.h"

/* Calls `fn` for each piece of [addr, addr+len) that lies within one page. */
static bool for_each_page(uint64_t space, uint64_t addr, size_t len, bool write,
                          void (*fn)(void *kaddr, size_t done, size_t n, void *ctx), void *ctx) {
    if (addr + len < addr || addr + len > USER_SPACE_END) {
        return false;
    }
    size_t done = 0;
    while (done < len) {
        uint64_t va = addr + done;
        size_t n = PAGE_SIZE - va % PAGE_SIZE;
        if (n > len - done) {
            n = len - done;
        }
        uint64_t phys;
        if (!vmm_user_lookup(space, va, write, &phys)) {
            return false;
        }
        fn(phys_to_virt(phys), done, n, ctx);
        done += n;
    }
    return true;
}

static void read_piece(void *kaddr, size_t done, size_t n, void *ctx) {
    memcpy((char *)ctx + done, kaddr, n);
}

static void write_piece(void *kaddr, size_t done, size_t n, void *ctx) {
    memcpy(kaddr, (const char *)ctx + done, n);
}

bool copy_from_user(uint64_t space, void *dst, uint64_t src, size_t len) {
    return for_each_page(space, src, len, false, read_piece, dst);
}

bool copy_to_user(uint64_t space, uint64_t dst, const void *src, size_t len) {
    return for_each_page(space, dst, len, true, write_piece, (void *)src);
}

int64_t copy_string_from_user(uint64_t space, char *dst, uint64_t src, size_t max) {
    for (size_t i = 0; i < max; i++) {
        if (!copy_from_user(space, &dst[i], src + i, 1)) {
            return -EFAULT;
        }
        if (dst[i] == '\0') {
            return (int64_t)i;
        }
    }
    return -ENAMETOOLONG;
}
