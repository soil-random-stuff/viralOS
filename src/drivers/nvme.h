#pragma once

/* NVMe SSDs: every namespace becomes a disk ("nvme0", ...). */

void nvme_init(void);       /* find the controllers and register their disks (may take seconds) */
void nvme_shutdown(void);   /* before power off: let them save what they hold */
