#pragma once

/*
 * Legacy 8259 PIC pair. We use the APIC instead, so the PICs are only
 * remapped (so a stray IRQ can't masquerade as a CPU exception) and masked.
 */
void pic_disable(void);
