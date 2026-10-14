#pragma once

/* <stdio.h> for lwext4: its debug output is switched off (CONFIG_DEBUG_PRINTF 0); this is all it needs. */
#define printf(...) ((void)0)
#define fflush(f)   ((void)0)
