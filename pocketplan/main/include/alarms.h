#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

/* Daily / weekday alarms. The buzzer hangs off TCA9554 P7 so it can only be
 * pulsed, not tuned; the ring is a beep pattern with a full screen "停止"
 * overlay. Alarms are edited from the web page and listed on the 倒数 tab. */
#define PP_ALARMS_MAX 16

typedef struct {
    char id[8];
    char time[6];      /* "HH:MM", 24 hour local time */
    char label[48];
    uint8_t days;      /* bit0 = Monday .. bit6 = Sunday, 0 = every day */
    bool enabled;
    bool active_today; /* the weekday mask matches today */
} pp_alarm_t;

esp_err_t pp_alarms_init(void);
void pp_alarms_refresh(void);
int pp_alarms_count(void);
const pp_alarm_t *pp_alarms_get(int index);
/* Body: {"id"?,"time":"07:30","label":"起床","days":0,"enabled":true} */
esp_err_t pp_alarms_upsert(const char *json, char *new_id, size_t new_id_size);
esp_err_t pp_alarms_delete(const char *id);
esp_err_t pp_alarms_set_enabled(const char *id, bool enabled);
char *pp_alarms_json(void);

/* Ring state, used by the web API and the 倒数 page. */
bool pp_alarms_ringing(void);
void pp_alarm_stop(void);
/* Debug helper for the boot tour: ring once without waiting for the clock. */
void pp_alarms_demo_ring(void);
