#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <esp_err.h>

/* Local time handling: timezone from config, the PCF85063 RTC on the board and
 * SNTP once Wi-Fi is up. Everything else in the firmware just calls time(). */
void pp_clock_init(void);
/* True once the wall clock looks like a real date (>= 2024). */
bool pp_clock_valid(void);
/* Start SNTP if it is not running yet; safe to call repeatedly. */
void pp_clock_start_sntp(void);
/* Re-apply config.tz to the C library (after the web UI changes it). */
void pp_clock_apply_tz(void);
/* Copy "YYYY-MM-DD" of today into out. */
void pp_clock_today(char *out, size_t out_size);
/* Copy the formatted local time; strftime-style fmt. */
void pp_clock_format(char *out, size_t out_size, const char *fmt);
/* Read/write the PCF85063; both return ESP_ERR_NOT_FOUND when absent. */
esp_err_t pp_clock_read_rtc(struct tm *out);
esp_err_t pp_clock_write_rtc(const struct tm *in);
