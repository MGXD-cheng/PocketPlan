/*
 * plans.json model, check-in logic and streaks.
 *
 * A plan is "active" on a date depending on its repeat rule; a check-in is a
 * {date: true} entry. Streaks only look at days that had at least one active
 * plan, so a weekly plan does not break a daily chain on its off days.
 */
#include "plans.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "clock.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "store.h"

static const char *TAG = "plans";

static cJSON *s_root;          /* {"version":1,"plans":[...]} */
static pp_plan_t s_view[PP_PLANS_MAX];
static int s_view_count;
static char s_today[11];
static int s_streak;
static SemaphoreHandle_t s_lock;

static void refresh_locked(void);

static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

/* ---------------------------- date arithmetic --------------------------- */
static int days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

static bool parse_date(const char *s, int *y, int *m, int *d)
{
    if (s == NULL || strlen(s) < 10) {
        return false;
    }
    return sscanf(s, "%4d-%2d-%2d", y, m, d) == 3;
}

static int date_days(const char *s)
{
    int y = 1970, m = 1, d = 1;
    if (!parse_date(s, &y, &m, &d)) {
        return 0;
    }
    return days_from_civil(y, m, d);
}

static void days_to_date(int days, char *out, size_t out_size)
{
    time_t t = (time_t)days * 86400 + 43200;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, out_size, "%Y-%m-%d", &tm);
}

static int weekday_mon0(const char *date)
{
    return ((date_days(date) + 3) % 7 + 7) % 7;
}

/* ------------------------------- persistence ---------------------------- */
static void seed_if_empty(void)
{
    s_root = cJSON_CreateObject();
    cJSON_AddNumberToObject(s_root, "version", 1);
    cJSON *arr = cJSON_AddArrayToObject(s_root, "plans");
    char today[11];
    pp_clock_today(today, sizeof(today));
    const char *titles[] = {"Drink water", "Read 30 minutes", "Weekly review"};
    const char *repeats[] = {"daily", "daily", "weekly"};
    int weekdays[] = {0, 0, 6};
    for (int i = 0; i < 3; i++) {
        cJSON *p = cJSON_CreateObject();
        char id[8];
        snprintf(id, sizeof(id), "p%d", i + 1);
        cJSON_AddStringToObject(p, "id", id);
        cJSON_AddStringToObject(p, "title", titles[i]);
        cJSON_AddStringToObject(p, "note", "");
        cJSON_AddStringToObject(p, "repeat", repeats[i]);
        cJSON_AddNumberToObject(p, "weekdays", weekdays[i] ? (1 << weekdays[i]) : 0);
        cJSON_AddStringToObject(p, "date", "");
        cJSON_AddNumberToObject(p, "interval", 1);
        cJSON_AddStringToObject(p, "created", today);
        cJSON_AddBoolToObject(p, "archived", false);
        cJSON_AddObjectToObject(p, "checkins");
        cJSON_AddItemToArray(arr, p);
    }
}

static esp_err_t save_locked(void)
{
    char *text = cJSON_PrintUnformatted(s_root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = pp_data_write("plans.json", text, strlen(text));
    cJSON_free(text);
    return err;
}

/* Drop check-ins older than PP_CHECKIN_KEEP_DAYS so the file stays small.
 * Keys are collected first because deleting during cJSON_ArrayForEach on an
 * object invalidates the iteration. */
static void prune_checkins(void)
{
    int cutoff = date_days(s_today) - PP_CHECKIN_KEEP_DAYS;
    cJSON *plan;
    cJSON_ArrayForEach(plan, cJSON_GetObjectItemCaseSensitive(s_root, "plans")) {
        cJSON *checkins = cJSON_GetObjectItemCaseSensitive(plan, "checkins");
        if (!cJSON_IsObject(checkins)) {
            continue;
        }
        char stale[48][11];
        int stale_count = 0;
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, checkins) {
            if (stale_count < 48 && item->string && date_days(item->string) < cutoff) {
                snprintf(stale[stale_count++], sizeof(stale[0]), "%s", item->string);
            }
        }
        for (int i = 0; i < stale_count; i++) {
            cJSON_DeleteItemFromObjectCaseSensitive(checkins, stale[i]);
        }
    }
}

/* ------------------------------- computed ------------------------------- */
static bool plan_active_on(const cJSON *plan, const char *date)
{
    const cJSON *archived = cJSON_GetObjectItemCaseSensitive(plan, "archived");
    if (cJSON_IsTrue(archived)) {
        return false;
    }
    const cJSON *repeat = cJSON_GetObjectItemCaseSensitive(plan, "repeat");
    const char *r = cJSON_IsString(repeat) ? repeat->valuestring : "daily";
    if (strcmp(r, "weekly") == 0) {
        const cJSON *wd = cJSON_GetObjectItemCaseSensitive(plan, "weekdays");
        int mask = cJSON_IsNumber(wd) ? wd->valueint : 0;
        return (mask & (1 << weekday_mon0(date))) != 0;
    }
    if (strcmp(r, "once") == 0) {
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(plan, "date");
        return cJSON_IsString(d) && strcmp(d->valuestring, date) == 0;
    }
    if (strcmp(r, "interval") == 0) {
        const cJSON *iv = cJSON_GetObjectItemCaseSensitive(plan, "interval");
        int n = cJSON_IsNumber(iv) ? iv->valueint : 1;
        if (n < 1) {
            n = 1;
        }
        const cJSON *created = cJSON_GetObjectItemCaseSensitive(plan, "created");
        int base = cJSON_IsString(created) ? date_days(created->valuestring) : 0;
        int diff = date_days(date) - base;
        return diff >= 0 && (diff % n) == 0;
    }
    return true; /* daily */
}

static bool plan_done_on(const cJSON *plan, const char *date)
{
    const cJSON *checkins = cJSON_GetObjectItemCaseSensitive(plan, "checkins");
    if (!cJSON_IsObject(checkins)) {
        return false;
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(checkins, date);
    return cJSON_IsTrue(v);
}

static int plan_total(const cJSON *plan)
{
    const cJSON *checkins = cJSON_GetObjectItemCaseSensitive(plan, "checkins");
    if (!cJSON_IsObject(checkins)) {
        return 0;
    }
    int n = 0;
    const cJSON *v = NULL;
    cJSON_ArrayForEach(v, checkins) {
        if (cJSON_IsTrue(v)) {
            n++;
        }
    }
    return n;
}

/* Days in a row with this plan checked, ending today or yesterday. */
static int plan_streak(const cJSON *plan)
{
    int today = date_days(s_today);
    char date[11];
    days_to_date(today, date, sizeof(date));
    int start = (plan_active_on(plan, date) && !plan_done_on(plan, date)) ? today - 1 : today;
    int streak = 0;
    for (int i = 0; i < PP_CHECKIN_KEEP_DAYS; i++) {
        char d[11];
        days_to_date(start - i, d, sizeof(d));
        if (!plan_active_on(plan, d)) {
            continue;
        }
        if (plan_done_on(plan, d)) {
            streak++;
        } else {
            break;
        }
    }
    return streak;
}

static void refresh_locked(void)
{
    pp_clock_today(s_today, sizeof(s_today));
    s_view_count = 0;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(s_root, "plans");
    cJSON *plan = NULL;
    cJSON_ArrayForEach(plan, arr) {
        if (s_view_count >= PP_PLANS_MAX) {
            break;
        }
        pp_plan_t *v = &s_view[s_view_count++];
        memset(v, 0, sizeof(*v));
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(plan, "id");
        const cJSON *title = cJSON_GetObjectItemCaseSensitive(plan, "title");
        const cJSON *note = cJSON_GetObjectItemCaseSensitive(plan, "note");
        const cJSON *repeat = cJSON_GetObjectItemCaseSensitive(plan, "repeat");
        const cJSON *date = cJSON_GetObjectItemCaseSensitive(plan, "date");
        const cJSON *wd = cJSON_GetObjectItemCaseSensitive(plan, "weekdays");
        const cJSON *iv = cJSON_GetObjectItemCaseSensitive(plan, "interval");
        const cJSON *arch = cJSON_GetObjectItemCaseSensitive(plan, "archived");
        snprintf(v->id, sizeof(v->id), "%s", cJSON_IsString(id) ? id->valuestring : "");
        snprintf(v->title, sizeof(v->title), "%s",
                 cJSON_IsString(title) ? title->valuestring : "");
        snprintf(v->note, sizeof(v->note), "%s", cJSON_IsString(note) ? note->valuestring : "");
        snprintf(v->repeat, sizeof(v->repeat), "%s",
                 cJSON_IsString(repeat) ? repeat->valuestring : "daily");
        snprintf(v->date, sizeof(v->date), "%s", cJSON_IsString(date) ? date->valuestring : "");
        v->weekdays = (uint8_t)(cJSON_IsNumber(wd) ? wd->valueint : 0);
        v->interval = (uint16_t)(cJSON_IsNumber(iv) ? iv->valueint : 1);
        v->archived = cJSON_IsTrue(arch);
        v->active_today = plan_active_on(plan, s_today);
        v->done_today = plan_done_on(plan, s_today);
        v->total = plan_total(plan);
        v->streak = plan_streak(plan);
    }

    /* aggregate streak: consecutive days where all plans due that day were done */
    s_streak = 0;
    int today = date_days(s_today);
    char d[11];
    days_to_date(today, d, sizeof(d));
    int due_today = 0, done_today = 0;
    cJSON_ArrayForEach(plan, arr) {
        if (plan_active_on(plan, d)) {
            due_today++;
            if (plan_done_on(plan, d)) {
                done_today++;
            }
        }
    }
    int start = (due_today > 0 && done_today < due_today) ? today - 1 : today;
    for (int i = 0; i < PP_CHECKIN_KEEP_DAYS; i++) {
        days_to_date(start - i, d, sizeof(d));
        int due = 0, done = 0;
        cJSON_ArrayForEach(plan, arr) {
            if (plan_active_on(plan, d)) {
                due++;
                if (plan_done_on(plan, d)) {
                    done++;
                }
            }
        }
        if (due == 0) {
            continue;
        }
        if (done == due) {
            s_streak++;
        } else {
            break;
        }
    }
}

void pp_plans_refresh(void)
{
    lock();
    refresh_locked();
    unlock();
}

esp_err_t pp_plans_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    size_t len = 0;
    char *text = pp_data_read("plans.json", &len);
    if (text != NULL) {
        s_root = cJSON_Parse(text);
        free(text);
    }
    if (s_root == NULL) {
        ESP_LOGI(TAG, "creating plans.json with samples");
        seed_if_empty();
        refresh_locked();
        save_locked();
    }
    pp_plans_refresh();
    ESP_LOGI(TAG, "loaded %d plans, today %s", s_view_count, s_today);
    return ESP_OK;
}

int pp_plans_count(void)
{
    return s_view_count;
}

const pp_plan_t *pp_plans_get(int index)
{
    if (index < 0 || index >= s_view_count) {
        return NULL;
    }
    return &s_view[index];
}

void pp_plans_stats(int *done, int *active, int *streak)
{
    int d = 0, a = 0;
    for (int i = 0; i < s_view_count; i++) {
        if (s_view[i].active_today && !s_view[i].archived) {
            a++;
            if (s_view[i].done_today) {
                d++;
            }
        }
    }
    if (done) {
        *done = d;
    }
    if (active) {
        *active = a;
    }
    if (streak) {
        *streak = s_streak;
    }
}

const char *pp_plans_today(void)
{
    return s_today;
}

static cJSON *find_plan(const char *id)
{
    cJSON *plan = NULL;
    cJSON_ArrayForEach(plan, cJSON_GetObjectItemCaseSensitive(s_root, "plans")) {
        const cJSON *pid = cJSON_GetObjectItemCaseSensitive(plan, "id");
        if (cJSON_IsString(pid) && strcmp(pid->valuestring, id) == 0) {
            return plan;
        }
    }
    return NULL;
}

esp_err_t pp_plans_checkin(const char *id, const char *date, int done, bool *state_out)
{
    char today[11];
    if (date == NULL || date[0] == '\0') {
        pp_clock_today(today, sizeof(today));
        date = today;
    }
    lock();
    cJSON *plan = find_plan(id);
    cJSON *checkins = plan ? cJSON_GetObjectItemCaseSensitive(plan, "checkins") : NULL;
    if (checkins == NULL || !cJSON_IsObject(checkins)) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    cJSON *existing = cJSON_GetObjectItemCaseSensitive(checkins, date);
    bool now_done = done > 0 ? true : (done < 0 ? !cJSON_IsTrue(existing) : false);
    if (now_done) {
        if (existing) {
            cJSON_ReplaceItemInObjectCaseSensitive(checkins, date, cJSON_CreateTrue());
        } else {
            cJSON_AddTrueToObject(checkins, date);
        }
    } else if (existing) {
        cJSON_DeleteItemFromObjectCaseSensitive(checkins, date);
    }
    prune_checkins();
    esp_err_t err = save_locked();
    refresh_locked();
    unlock();
    if (state_out) {
        *state_out = now_done;
    }
    return err;
}

static void copy_json_string(const cJSON *obj, const char *key, char *out, size_t out_size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) {
        snprintf(out, out_size, "%s", v->valuestring);
    }
}

esp_err_t pp_plans_upsert(const char *json, char *new_id, size_t new_id_size)
{
    cJSON *body = cJSON_Parse(json);
    if (body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char title[96] = {0};
    copy_json_string(body, "title", title, sizeof(title));
    if (title[0] == '\0') {
        cJSON_Delete(body);
        return ESP_ERR_INVALID_ARG;
    }
    char id[12] = {0};
    copy_json_string(body, "id", id, sizeof(id));

    lock();
    cJSON *plan = id[0] ? find_plan(id) : NULL;
    bool created = false;
    if (plan == NULL) {
        plan = cJSON_CreateObject();
        created = true;
        int max_n = 0;
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(s_root, "plans")) {
            const cJSON *pid = cJSON_GetObjectItemCaseSensitive(it, "id");
            if (cJSON_IsString(pid) && pid->valuestring[0] == 'p') {
                int n = atoi(pid->valuestring + 1);
                if (n > max_n) {
                    max_n = n;
                }
            }
        }
        if (max_n >= 9999) {
            cJSON_Delete(plan);
            unlock();
            cJSON_Delete(body);
            return ESP_ERR_NO_MEM;
        }
        snprintf(id, sizeof(id), "p%d", max_n + 1);
        cJSON_AddStringToObject(plan, "id", id);
        cJSON_AddStringToObject(plan, "created", s_today);
        cJSON_AddObjectToObject(plan, "checkins");
        cJSON_AddBoolToObject(plan, "archived", false);
    }

    char note[160] = {0};
    copy_json_string(body, "note", note, sizeof(note));
    char repeat[10] = "daily";
    copy_json_string(body, "repeat", repeat, sizeof(repeat));
    if (strcmp(repeat, "weekly") && strcmp(repeat, "once") && strcmp(repeat, "interval")) {
        snprintf(repeat, sizeof(repeat), "daily");
    }
    const cJSON *wd = cJSON_GetObjectItemCaseSensitive(body, "weekdays");
    int mask = 0;
    if (cJSON_IsNumber(wd)) {
        mask = wd->valueint;
    } else if (cJSON_IsArray(wd)) {
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, wd) {
            if (cJSON_IsNumber(it) && it->valueint >= 0 && it->valueint < 7) {
                mask |= 1 << it->valueint;
            }
        }
    }
    char date[11] = {0};
    copy_json_string(body, "date", date, sizeof(date));
    const cJSON *iv = cJSON_GetObjectItemCaseSensitive(body, "interval");
    int interval = cJSON_IsNumber(iv) ? iv->valueint : 1;
    if (interval < 1) {
        interval = 1;
    }
    if (interval > 365) {
        interval = 365;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(plan, "title");
    cJSON_AddStringToObject(plan, "title", title);
    cJSON_DeleteItemFromObjectCaseSensitive(plan, "note");
    cJSON_AddStringToObject(plan, "note", note);
    cJSON_DeleteItemFromObjectCaseSensitive(plan, "repeat");
    cJSON_AddStringToObject(plan, "repeat", repeat);
    cJSON_DeleteItemFromObjectCaseSensitive(plan, "weekdays");
    cJSON_AddNumberToObject(plan, "weekdays", mask & 0x7F);
    cJSON_DeleteItemFromObjectCaseSensitive(plan, "date");
    cJSON_AddStringToObject(plan, "date", date);
    cJSON_DeleteItemFromObjectCaseSensitive(plan, "interval");
    cJSON_AddNumberToObject(plan, "interval", interval);

    if (created) {
        cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(s_root, "plans"), plan);
        if (new_id && new_id_size) {
            snprintf(new_id, new_id_size, "%s", id);
        }
    }
    esp_err_t err = save_locked();
    refresh_locked();
    unlock();
    cJSON_Delete(body);
    return err;
}

esp_err_t pp_plans_delete(const char *id)
{
    lock();
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(s_root, "plans");
    int count = cJSON_GetArraySize(arr);
    int found = -1;
    for (int i = 0; i < count; i++) {
        cJSON *plan = cJSON_GetArrayItem(arr, i);
        const cJSON *pid = cJSON_GetObjectItemCaseSensitive(plan, "id");
        if (cJSON_IsString(pid) && strcmp(pid->valuestring, id) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    cJSON *detached = cJSON_DetachItemFromArray(arr, found);
    cJSON_Delete(detached);
    esp_err_t err = save_locked();
    refresh_locked();
    unlock();
    return err;
}

char *pp_plans_json(void)
{
    lock();
    refresh_locked();
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "today", s_today);
    int done = 0, active = 0, streak = 0;
    pp_plans_stats(&done, &active, &streak);
    cJSON_AddNumberToObject(out, "done", done);
    cJSON_AddNumberToObject(out, "active", active);
    cJSON_AddNumberToObject(out, "streak", streak);
    cJSON *arr = cJSON_AddArrayToObject(out, "plans");
    for (int i = 0; i < s_view_count; i++) {
        const pp_plan_t *v = &s_view[i];
        cJSON *copy = cJSON_CreateObject();
        cJSON_AddStringToObject(copy, "id", v->id);
        cJSON_AddStringToObject(copy, "title", v->title);
        cJSON_AddStringToObject(copy, "note", v->note);
        cJSON_AddStringToObject(copy, "repeat", v->repeat);
        cJSON_AddNumberToObject(copy, "weekdays", v->weekdays);
        cJSON_AddStringToObject(copy, "date", v->date);
        cJSON_AddNumberToObject(copy, "interval", v->interval);
        cJSON_AddBoolToObject(copy, "archived", v->archived);
        cJSON_AddBoolToObject(copy, "active_today", v->active_today);
        cJSON_AddBoolToObject(copy, "done_today", v->done_today);
        cJSON_AddNumberToObject(copy, "streak", v->streak);
        cJSON_AddNumberToObject(copy, "total", v->total);
        cJSON *plan = find_plan(v->id);
        cJSON *checkins = plan ? cJSON_GetObjectItemCaseSensitive(plan, "checkins") : NULL;
        cJSON_AddItemToObject(copy, "checkins", checkins ? cJSON_Duplicate(checkins, 1)
                                                         : cJSON_CreateObject());
        cJSON_AddItemToArray(arr, copy);
    }
    char *text = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    unlock();
    return text;
}

char *pp_plans_raw_json(void)
{
    lock();
    char *text = cJSON_PrintUnformatted(s_root);
    unlock();
    return text;
}
