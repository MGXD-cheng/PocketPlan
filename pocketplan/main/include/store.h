#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

/* Small internal filesystem (SPIFFS on the "storage" partition) that keeps the
 * data which must survive without an SD card: config.json and plans.json. */
#define PP_LFS       "/lfs"
#define PP_CONF_PATH PP_LFS "/config.json"
#define PP_PLANS_PATH PP_LFS "/plans.json"
#define PP_COUNTDOWN_PATH PP_LFS "/countdowns.json"

/* Live data (plans, countdowns, alarms, AI chat) can be kept on the SD card
 * instead of the internal SPIFFS partition; config.json always stays internal
 * because it holds the switch itself. */
#define PP_DATA_DIR_SD "/sdcard/.pocketplan"

bool pp_data_on_sd(void);
void pp_data_path(char *out, size_t size, const char *name);
/* Read/write one of the live data files. When the SD cache is enabled but the
 * card is gone the write is refused instead of silently landing on SPIFFS. */
char *pp_data_read(const char *name, size_t *len_out);
esp_err_t pp_data_write(const char *name, const char *data, size_t len);
/* Move the data files to whichever directory the current setting points at. */
esp_err_t pp_store_migrate_data(void);

esp_err_t pp_store_init(void);
bool pp_store_ready(void);
uint32_t pp_store_free_kb(void);
uint32_t pp_store_total_kb(void);

/* Returns a malloc()ed, NUL terminated copy of the file, or NULL. */
char *pp_store_read(const char *path, size_t *len_out);
/* tmp -> fflush -> fsync -> rename, so a power cut never leaves half a file. */
esp_err_t pp_store_write_atomic(const char *path, const char *data, size_t len);
esp_err_t pp_store_remove(const char *path);
