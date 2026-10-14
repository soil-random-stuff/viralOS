#pragma once

/*
 * Turning the machine off and restarting it.
 *
 * Off: ACPI sleep state S5. The FADT says where the PM1 control registers
 * are; the value to write ("SLP_TYP") is in the DSDT's \_S5 object, which
 * we find by looking for its bytes rather than running an AML interpreter
 * (the usual shortcut; it works on QEMU and most PCs).
 *
 * Restart: the FADT's reset register if there is one, then the keyboard
 * controller's reset line, then a triple fault.
 *
 * Doesn't return on success; returns -errno if the machine stayed on.
 */
int power_off_or_restart(int how);   /* OS_POWER_* */
