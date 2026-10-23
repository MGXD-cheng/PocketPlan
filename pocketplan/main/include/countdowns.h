#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

/* Countdown days ("倒数日"). /lfs/countdowns.json keeps the raw fields; the
 * number of days left is recomputed on every refresh, so the list stays right
 * after the clock jumps or the year rolls over. */
#define PP_COUNTDOWNS_MAX 24

typedef struct {
    char id[12];
    char title[96];
    char date[11];   /* target date, YYYY-MM-DD; for yearly only month/day matter */
    char note[160];
    bool yearly;
    int days_left;   /* 0 = today, negative = already past */
} pp_countdown_t;

esp_err_t pp_countdowns_init(void);
/* Recompute days_left and the sort order. Call after the clock jumps (SNTP). */
void pp_countdowns_refresh(void);
int pp_countdowns_count(void);
const pp_countdown_t *pp_countdowns_get(int index);

/* Create or update from a JSON object body; fills new_id when created. */
esp_err_t pp_countdowns_upsert(const char *json, char *new_id, size_t new_id_size);
esp_err_t pp_countdowns_delete(const char *id);

/* Full document with the computed days_left, malloc()ed. */
char *pp_countdowns_json(void);
