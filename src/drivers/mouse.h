#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * PS/2 mouse on the 8042's second port (IRQ 12), with the IntelliMouse
 * scroll wheel when present. Under VMware or QEMU, the "vmmouse" backdoor
 * gives absolute coordinates instead, so the pointer simply follows the
 * host's mouse (no capturing it inside the window).
 *
 * Movement and clicks become OS_EVENT_MOUSE input events.
 */
void mouse_init(void);
/* Movement from another mouse (USB): screen directions (dy > 0 is down), OS_MOUSE_* buttons,
   wheel < 0 is up. */
void mouse_feed(int32_t dx, int32_t dy, uint32_t buttons, int32_t wheel);
bool mouse_present(void);
bool mouse_absolute(void);
