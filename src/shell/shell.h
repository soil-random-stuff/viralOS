#pragma once

/*
 * The kernel console: a command line built into the kernel, for debugging
 * and as a fallback when the user shell (/usr/bin/shell) isn't running.
 * Never returns.
 */
__attribute__((noreturn)) void shell_run(void);
