#pragma once

#include <stdbool.h>
#include <stdint.h>

void pp_ui_init(void);
void pp_ui_refresh_files(void);

/* Status line in the top bar (network state, WebDAV address, ...) */
void pp_ui_set_status(const char *text);
/* Second line, shown under the title in the header block */
void pp_ui_set_subtitle(const char *text);

/* Bottom tab navigation: 0 plans, 1 countdowns, 2 files, 3 music, 4 AI,
 * 5 settings. */
void pp_ui_show_tab(int index);
int pp_ui_active_tab(void);
/* Floating message shown for a couple of seconds. */
void pp_ui_toast(const char *text);
