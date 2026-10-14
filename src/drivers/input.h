#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <viralos.h>

/*
 * Input events (keys, mouse) for the program that owns the display.
 * Drivers push from their interrupt handlers; with no owner, events are
 * dropped.
 */
void input_set_owner(uint64_t pid);   /* 0 = nobody */
uint64_t input_owner(void);

void input_push(struct os_input_event *event);   /* fills in time_ms */
void input_wake_readers(void);   /* e.g. so a killed reader notices */
bool input_pending(void);        /* events waiting (for poll) */

/*
 * Copy up to `max` events into `out`. Waits until there is at least one,
 * or timeout_ms passes (-1 = forever, 0 = don't wait). Returns the number
 * of events, 0 on timeout, or -EINTR if the caller was killed.
 */
int input_read(struct os_input_event *out, int max, int64_t timeout_ms);
