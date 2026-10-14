#pragma once

/* Shared bits for the network card drivers (drivers/net/). */

#include <stdbool.h>
#include <stdint.h>

#include "lib/string.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "net/netdev.h"

/* Physically contiguous, zeroed memory for a card to read or write (below 4 GiB if `low`, for cards
   that only take 32-bit addresses). Returns the physical address, 0 if there's none. */
static inline uint64_t nic_dma_alloc(uint64_t bytes, bool low, void **virt) {
    uint64_t pages = (bytes + 4095) / 4096;
    uint64_t phys = pmm_alloc_contiguous(pages, low ? 0xFFFFFFFFull : ~0ull);
    if (phys) {
        *virt = phys_to_virt(phys);
        memset(*virt, 0, pages * 4096);
    }
    return phys;
}

/* The drivers' entry points (each finds its cards and registers them). */
void virtio_net_probe(void);
void e1000_probe(void);
void e1000e_probe(void);
void r8169_probe(void);
