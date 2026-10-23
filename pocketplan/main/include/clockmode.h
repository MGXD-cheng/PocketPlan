#pragma once

#include <stdbool.h>

/* Full screen wall clock ("时钟模式"): time, date and the countdowns, nothing
 * else. Tap anywhere to leave. Honours the focus_dark setting. */
void pp_clockmode_open(void);
void pp_clockmode_close(void);
bool pp_clockmode_active(void);
