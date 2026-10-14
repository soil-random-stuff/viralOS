/*
 * Minimal ACPI table discovery: RSDP -> XSDT (or RSDT on ACPI 1.0) -> tables.
 * The tables live in ACPI memory regions, which the HHDM maps and the PMM
 * never hands out, so pointers into them stay valid.
 */

#include <stddef.h>

#include "acpi/acpi.h"
#include "lib/string.h"
#include "mm/mm.h"

struct rsdp {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;       /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    uint32_t rsdt_address;
    /* ACPI 2.0+ */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

static const struct acpi_sdt_header *root;  /* XSDT or RSDT */
static bool use_xsdt;
static uint8_t revision;

static bool checksum_ok(const void *data, size_t length) {
    const uint8_t *p = data;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum += p[i];
    }
    return sum == 0;
}

bool acpi_init(void *rsdp_ptr) {
    const struct rsdp *rsdp = rsdp_ptr;
    if (!rsdp || memcmp(rsdp->signature, "RSD PTR ", 8) != 0 || !checksum_ok(rsdp, 20)) {
        return false;
    }
    revision = rsdp->revision;

    if (rsdp->revision >= 2 && rsdp->xsdt_address && checksum_ok(rsdp, rsdp->length)) {
        root = phys_to_virt(rsdp->xsdt_address);
        use_xsdt = true;
    } else {
        root = phys_to_virt(rsdp->rsdt_address);
        use_xsdt = false;
    }
    return checksum_ok(root, root->length);
}

const struct acpi_sdt_header *acpi_find_table(const char *signature) {
    if (!root) {
        return NULL;
    }
    size_t entry_size = use_xsdt ? 8 : 4;
    size_t count = (root->length - sizeof(*root)) / entry_size;
    const uint8_t *entries = (const uint8_t *)(root + 1);

    for (size_t i = 0; i < count; i++) {
        uint64_t phys;
        if (use_xsdt) {
            memcpy(&phys, entries + i * 8, 8);  /* entries are not 8-byte aligned */
        } else {
            uint32_t p32;
            memcpy(&p32, entries + i * 4, 4);
            phys = p32;
        }
        const struct acpi_sdt_header *table = phys_to_virt(phys);
        if (memcmp(table->signature, signature, 4) == 0 && checksum_ok(table, table->length)) {
            return table;
        }
    }
    return NULL;
}

uint8_t acpi_revision(void) {
    return revision;
}
