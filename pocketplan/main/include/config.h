#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

#define PP_AI_BASE_DEFAULT  "https://api.deepseek.com/v1"
#define PP_AI_MODEL_DEFAULT "deepseek-chat"

typedef struct {
    /* AI */
    char ai_base[128];
    char ai_key[192];
    char ai_model[64];
    char ai_persona[160];
    /* clock */
    char tz[40];
    /* screen */
    uint8_t brightness;   /* 10..100 */
    uint8_t font_size;    /* 16 = UI font, 20 = large reader font */
    uint8_t line_space;   /* extra px between reader lines */
    bool music_ready;     /* PCM5102A DAC wired up */
    /* focus mode */
    bool focus_dark;      /* dark clock face instead of white */
    char focus_cd[12];    /* countdown id shown in focus mode, "" = none */
    /* storage */
    bool cache_sd;        /* keep plans/countdowns/alarms/AI chat on the SD card */
    /* network */
    bool webdav;          /* expose the card over WebDAV; off by default */
    /* power */
    bool auto_sleep;      /* switch the backlight off when nobody touches it */
    uint8_t sleep_min;    /* idle minutes before that happens, 1..60 */
} pp_config_t;

esp_err_t pp_config_load(void);
esp_err_t pp_config_save(void);
pp_config_t *pp_config(void);

/* Rebuild config.json from the defaults (used by the factory reset). */
void pp_config_reset(void);
