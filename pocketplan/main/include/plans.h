#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

/* Daily planning board. The JSON in /lfs/plans.json is the source of truth;
 * this module adds "is it due today", streak and check-in bookkeeping. */
#define PP_PLANS_MAX 32
#define PP_CHECKIN_KEEP_DAYS 120

typedef struct {
    char id[12];
    char title[96];
    char note[160];
    char repeat[10];   /* daily | weekly | once | interval */
    char date[11];     /* once: YYYY-MM-DD */
    uint8_t weekdays;  /* weekly: bit0 = Monday ... bit6 = Sunday */
    uint16_t interval;
    bool archived;
    bool active_today;
    bool done_today;
    int streak;
    int total;
} pp_plan_t;

esp_err_t pp_plans_init(void);
/* Recompute today/active/streak. Call after the clock jumps (SNTP). */
void pp_plans_refresh(void);
int pp_plans_count(void);
const pp_plan_t *pp_plans_get(int index);
/* Aggregate counters for the header line. */
void pp_plans_stats(int *done, int *active, int *streak);

const char *pp_plans_today(void);

/* Toggle (or set) one check-in. date may be NULL for today. */
esp_err_t pp_plans_checkin(const char *id, const char *date, int done, bool *state_out);
/* Create or update from a JSON object body; fills new_id when created. */
esp_err_t pp_plans_upsert(const char *json, char *new_id, size_t new_id_size);
esp_err_t pp_plans_delete(const char *id);

/* Full JSON document with computed fields, malloc()ed. */
char *pp_plans_json(void);
/* Raw stored JSON (no computed fields), malloc()ed; used for backup. */
char *pp_plans_raw_json(void);
