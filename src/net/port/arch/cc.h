#pragma once

/* lwIP's compiler/platform glue for the kernel (see lwip/arch.h). */

#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#include "lib/kprintf.h"
#include "lib/panic.h"

#define LWIP_NO_INTTYPES_H 1
#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "lu"

#define LWIP_NO_CTYPE_H  1
#define LWIP_NO_UNISTD_H 1
typedef long ssize_t;
#define SSIZE_MAX LONG_MAX

#define BYTE_ORDER LITTLE_ENDIAN

#define LWIP_PLATFORM_DIAG(x)   do { kprintf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) panic("lwip: assertion \"%s\" failed at %s:%d", x, __FILE__, __LINE__)

uint32_t net_random(void);
#define LWIP_RAND() net_random()

/* Socket errors come back as the system's own error numbers (net_errno.h). */
#define LWIP_ERRNO_INCLUDE "net_errno.h"
