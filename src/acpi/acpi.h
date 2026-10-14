#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Common header of every ACPI System Description Table. */
struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* `rsdp` is the virtual address of the RSDP, as handed over by Limine. */
bool acpi_init(void *rsdp);

/* Find a table by its 4-character signature, e.g. "APIC" for the MADT. */
const struct acpi_sdt_header *acpi_find_table(const char *signature);

uint8_t acpi_revision(void);
