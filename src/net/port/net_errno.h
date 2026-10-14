#pragma once

/* lwIP's sockets report errors through `errno`: the system's error numbers (Linux's values), kept
   per thread so that two programs in socket calls at once don't mix theirs up. */

#include <viralos.h>

int *net_errno_location(void);
#define errno (*net_errno_location())
