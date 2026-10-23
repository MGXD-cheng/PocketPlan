#pragma once

#include <stdbool.h>

/* Full screen landscape clock ("专注模式"). Tap anywhere to leave. */
void pp_focus_open(void);
void pp_focus_close(void);
bool pp_focus_active(void);
