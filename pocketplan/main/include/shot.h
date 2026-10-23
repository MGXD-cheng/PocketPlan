#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Grabs the active LVGL framebuffer as a 1 bit PBM. Caller frees *out. */
esp_err_t pp_shot_pbm(uint8_t **out, size_t *out_len, uint32_t *out_w, uint32_t *out_h);

/* Same bitmap, base64 encoded on stdout between ---PBMBEGIN--- markers. */
void pp_shot_dump_serial(void);

/* Reads "shot" from stdin so the host can ask for a dump over the UART. */
void pp_shot_start_console(void);

/* Walks all five tabs and the reader once after boot, dumping each screen. */
void pp_shot_start_tour(void);
