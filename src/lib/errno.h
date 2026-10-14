#pragma once

/* Error codes are part of the user ABI. Kernel functions return them negated. */
#include <viralos.h>

/* Accepts either sign. */
const char *strerror(int err);
