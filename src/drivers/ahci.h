#pragma once

/* SATA disks on AHCI controllers ("sata0", ...). */

void ahci_init(void);       /* find the controllers and register their disks */
void ahci_shutdown(void);   /* before power off: flush the disks' write caches */
