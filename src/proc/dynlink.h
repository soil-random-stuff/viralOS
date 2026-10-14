#pragma once

/*
 * Shared libraries: /usr/lib/lib*.so, used by programs (position-independent executables that
 * name the libraries they need, ELF DT_NEEDED).
 *
 * Each library is loaded once, at an address of its own that's the same in every process, and its
 * references (to itself and to the libraries it needs) are fixed up then. So its code pages are
 * shared by every process that uses it, read-only; each process gets its own copy of the library's
 * data. When a program starts, only the program's own references into the libraries are fixed up.
 * There's no lazy binding and no symbol interposition: a library always uses its own definitions
 * (linked -Bsymbolic).
 */

#include <stddef.h>
#include <stdint.h>

/* The program in `image` was loaded into `space` at `base` and has a dynamic section: load and map
   the libraries it needs and fix up its references. 0 or -errno (the reason is logged). */
int dynlink_program(uint64_t space, const uint8_t *image, size_t size, uint64_t base);
