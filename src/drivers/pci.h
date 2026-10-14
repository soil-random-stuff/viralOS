#pragma once

/*
 * PCI: finding devices and talking to their configuration space (the
 * legacy 0xCF8/0xCFC mechanism, which every PC chipset and QEMU has).
 */

#include <stdbool.h>
#include <stdint.h>

struct pci_device {
    uint8_t bus, slot, function;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if;
};

#define PCI_COMMAND          0x04
#define PCI_COMMAND_IO       0x0001
#define PCI_COMMAND_MEMORY   0x0002
#define PCI_COMMAND_MASTER   0x0004   /* the device may do DMA */
#define PCI_COMMAND_INTX_OFF 0x0400

uint32_t pci_read32(const struct pci_device *d, uint8_t offset);
uint16_t pci_read16(const struct pci_device *d, uint8_t offset);
uint8_t pci_read8(const struct pci_device *d, uint8_t offset);
void pci_write32(const struct pci_device *d, uint8_t offset, uint32_t value);
void pci_write16(const struct pci_device *d, uint8_t offset, uint16_t value);
void pci_write8(const struct pci_device *d, uint8_t offset, uint8_t value);

/* Scan every bus once (later calls are free). */
void pci_init(void);
/* The n-th device (0, 1, ...) with this class/subclass; false past the last. */
bool pci_find_class(uint8_t class_code, uint8_t subclass, int n, struct pci_device *out);
/* A memory BAR's physical address (64-bit BARs included); 0 if it isn't a memory BAR. */
uint64_t pci_bar_address(const struct pci_device *d, int bar);
/* An I/O port BAR's first port; 0 if it isn't one. */
uint16_t pci_bar_io(const struct pci_device *d, int bar);
/* How many bytes a memory BAR decodes (the all-ones probe); 0 if it isn't a memory BAR. */
uint64_t pci_bar_size(const struct pci_device *d, int bar);
int pci_device_count(void);
const struct pci_device *pci_device_at(int i);

/* The n-th device (0, 1, ...) with this vendor and device id; false past the last. */
bool pci_find_id(uint16_t vendor, uint16_t device, int n, struct pci_device *out);

#define PCI_CAP_PM     0x01
#define PCI_CAP_MSI    0x05
#define PCI_CAP_VENDOR 0x09
#define PCI_CAP_EXP    0x10
#define PCI_CAP_MSIX   0x11

/* Offset of the first capability with this id after `after` (0: from the start); 0 if none. */
uint8_t pci_find_capability(const struct pci_device *d, uint8_t id, uint8_t after);

/*
 * Interrupts by message (MSI-X if the device has it and `msix` allows it, else MSI), delivered to the
 * first CPU:
 * handler(arg) runs for each one, with the kernel lock held and interrupts off. Every MSI-X table
 * entry gets the same message. Turns the legacy interrupt line off. False if the device can't.
 */
bool pci_enable_msi(const struct pci_device *d, void (*handler)(void *arg), void *arg, bool msix);
