#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

/* Plain-text book opened from the SD card. The file stays on disk: pages are
 * decoded from a small line buffer in PSRAM, so book size is not limited by the
 * heap. Pagination uses the real LVGL font metrics. */
typedef struct pp_book pp_book_t;

pp_book_t *pp_reader_open(const char *path);
void pp_reader_close(pp_book_t *book);

const char *pp_reader_title(const pp_book_t *book);
size_t pp_reader_length(const pp_book_t *book);
bool pp_reader_encoding_is_gbk(const pp_book_t *book);

/* Build one page starting at byte offset start. Returns false when the offset
 * is at/after the end. next_off receives the offset of the following page. */
bool pp_reader_page(pp_book_t *book, int32_t start, const lv_font_t *font, int32_t width,
                    int32_t height, int32_t line_space, char *out, size_t out_size,
                    int32_t *next_off);

/* Remember the reading position per file (NVS). */
int32_t pp_reader_load_pos(const char *path);
void pp_reader_save_pos(const char *path, int32_t offset);

int pp_reader_percent(const pp_book_t *book, int32_t offset);
