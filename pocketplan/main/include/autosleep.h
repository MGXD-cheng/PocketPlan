#pragma once

#include <stdbool.h>

/* Auto sleep: after N minutes without a touch the backlight is switched off
 * and the first touch turns it back on. Wi-Fi, alarms and the web server keep
 * running, so the device still answers on the network while the screen is
 * dark. Controlled from 设置 > 屏幕 > 自动休眠 and from the web page. */
void pp_autosleep_init(void);
bool pp_autosleep_active(void);
void pp_autosleep_wake(void);
