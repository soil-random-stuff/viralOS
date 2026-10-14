#pragma once

#include <stdbool.h>
#include <stdint.h>

/* PS/2 keyboard on IRQ 1, US layout. */
void keyboard_init(void);

/* Blocks the calling thread until a key is pressed. */
char keyboard_getchar(void);
bool keyboard_try_getchar(char *out);

/*
 * Like keyboard_getchar, but gives up (returning false) once `abort()`
 * returns true; checked whenever the reader wakes up.
 */
bool keyboard_getchar_unless(char *out, bool (*abort)(void));

/*
 * Ctrl+C first goes to `handler` (from the interrupt). If it returns true
 * (it stopped a program) the key is swallowed and all readers are woken so
 * a killed one notices; otherwise Ctrl+C is delivered as character 3.
 *
 * Arrow keys, Home, End and Delete arrive as ANSI escape sequences
 * ("\x1b[A" for up, ...), like on a terminal.
 */
void keyboard_set_ctrl_c_handler(bool (*handler)(void));

/* Wake every thread waiting for a key (e.g. so a killed one can notice). */
void keyboard_wake_readers(void);

/*
 * A key from another keyboard (USB): `key` in OS_KEY_* terms, that is the
 * scancode set 1 make code, | 0x100 for E0-prefixed keys. Same handling as
 * PS/2 keys (layout, modifiers, Ctrl+C, events for the display owner).
 */
void keyboard_feed(uint16_t key, bool pressed);

/* OS_MOD_* bits for the modifier keys held right now. */
uint32_t keyboard_modifiers(void);
