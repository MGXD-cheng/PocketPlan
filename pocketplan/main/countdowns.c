/*
 * countdowns.json model. The file stores only what the user typed; the days
 * left, the "next occurrence" of a yearly event and the list order are all
 * derived on refresh, so nothing goes stale in flash.
 */
#include "countdowns.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "clock.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "store.h"

static const char *TAG = "countdown";

static cJSON *s_root;            /* {"version":1,"items":[...]} */
static pp_countdown_t s_view[PP_COUNTDOWNS_MAX];
static int s_view_count;
static char s_today[11];
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
    if (sscanf(s, "%4d-%2d-%2d", y, m, d) != 3) {
        return false;
    }
    return *m >= 1 && *m <= 12 && *d >= 1 && *d <= 31;
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

/* A yearly event repeats on its month/day: take this year's date, or next
 * year's when it has already passed. */
static int next_yearly(int target_days, int today_days)
{
    time_t t = (time_t)target_days * 86400 + 43200;
    struct tm tm_target;
    gmtime_r(&t, &tm_target);
    t = (time_t)today_days * 86400 + 43200;
    struct tm tm_today;
    gmtime_r(&t, &tm_today);
    for (int extra = 0; extra <= 1; extra++) {
        int cand = days_from_civil(tm_today.tm_year + 1900 + extra, tm_target.tm_mon + 1,
                                   tm_target.tm_mday);
        if (cand >= today_days) {
            return cand;
        }
    }
    return target_days;
}

/* ------------------------------- persistence ---------------------------- */
static esp_err_t save_locked(void)
{
    char *text = cJSON_PrintUnformatted(s_root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = pp_data_write("countdowns.json", text, strlen(text));
    cJSON_free(text);
    return err;
}

static void add_item(cJSON *arr, const char *id, const char *title, const char *date,
                     const char *note, bool yearly)
{
    cJSON *item = cJSON_CreateObject();
    if (item == NULL) {
        return;
    }
    cJSON_AddStringToObject(item, "id", id);
    cJSON_AddStringToObject(item, "title", title);
    cJSON_AddStringToObject(item, "date", date);
    cJSON_AddStringToObject(item, "note", note);
    cJSON_AddBoolToObject(item, "yearly", yearly);
    cJSON_AddItemToArray(arr, item);
}

/* Two well known dates so the tab is not empty on the very first boot; the
 * user can delete them from the web page like any other entry. */
static void seed_if_empty(void)
{
    s_root = cJSON_CreateObject();
    cJSON_AddNumberToObject(s_root, "version", 1);
    cJSON *arr = cJSON_AddArrayToObject(s_root, "items");
    if (arr == NULL) {
        return;
    }
    char date[11];
    pp_clock_today(date, sizeof(date));
    int y = 1970, m = 1, d = 1;
    parse_date(date, &y, &m, &d);
    snprintf(date, sizeof(date), "%04d-01-01", y);
    add_item(arr, "c1", "元旦", date, "新的一年", true);
    snprintf(date, sizeof(date), "%04d-10-01", y);
    add_item(arr, "c2", "国庆节", date, "长假", true);
}

/* ------------------------------- computed ------------------------------- */
static bool sorts_before(const pp_countdown_t *a, const pp_countdown_t *b)
{
    bool a_up = a->days_left >= 0;
    bool b_up = b->days_left >= 0;
    if (a_up != b_up) {
        return a_up;
    }
    return a_up ? (a->days_left < b->days_left) : (a->days_left > b->days_left);
}

static void sort_view(void)
{
    for (int i = 1; i < s_view_count; i++) {
        pp_countdown_t key = s_view[i];
        int j = i - 1;
        while (j >= 0 && sorts_before(&key, &s_view[j])) {
            s_view[j + 1] = s_view[j];
            j--;
        }
        s_view[j + 1] = key;
    }
}

static void refresh_locked(void)
{
    pp_clock_today(s_today, sizeof(s_today));
    int today = date_days(s_today);
    s_view_count = 0;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(s_root, "items");
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (s_view_count >= PP_COUNTDOWNS_MAX) {
            break;
        }
        pp_countdown_t *v = &s_view[s_view_count];
        memset(v, 0, sizeof(*v));
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *title = cJSON_GetObjectItemCaseSensitive(item, "title");
        const cJSON *date = cJSON_GetObjectItemCaseSensitive(item, "date");
        const cJSON *note = cJSON_GetObjectItemCaseSensitive(item, "note");
        const cJSON *yearly = cJSON_GetObjectItemCaseSensitive(item, "yearly");
        snprintf(v->id, sizeof(v->id), "%s", cJSON_IsString(id) ? id->valuestring : "");
        snprintf(v->title, sizeof(v->title), "%s",
                 cJSON_IsString(title) ? title->valuestring : "");
        snprintf(v->date, sizeof(v->date), "%s", cJSON_IsString(date) ? date->valuestring : "");
        snprintf(v->note, sizeof(v->note), "%s", cJSON_IsString(note) ? note->valuestring : "");
        v->yearly = cJSON_IsTrue(yearly);
        if (!parse_date(v->date, &(int){0}, &(int){0}, &(int){0})) {
            continue; /* unparsable date: hide instead of showing garbage */
        }
        int target = date_days(v->date);
        if (v->yearly) {
            target = next_yearly(target, today);
        }
        v->days_left = target - today;
        s_view_count++;
    }
    sort_view();
}

void pp_countdowns_refresh(void)
{
    lock();
    refresh_locked();
    unlock();
}

esp_err_t pp_countdowns_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    size_t len = 0;
    char *text = pp_data_read("countdowns.json", &len);
    if (text != NULL) {
        s_root = cJSON_Parse(text);
        free(text);
    }
    if (s_root == NULL) {
        ESP_LOGI(TAG, "creating countdowns.json with samples");
        seed_if_empty();
        refresh_locked();
        save_locked();
    }
    pp_countdowns_refresh();
    ESP_LOGI(TAG, "loaded %d countdowns, today %s", s_view_count, s_today);
    return ESP_OK;
}

int pp_countdowns_count(void)
{
    return s_view_count;
}

const pp_countdown_t *pp_countdowns_get(int index)
{
    if (index < 0 || index >= s_view_count) {
        return NULL;
    }
    return &s_view[index];
}

static void copy_json_string(const cJSON *obj, const char *key, char *out, size_t out_size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) {
        snprintf(out, out_size, "%s", v->valuestring);
    }
}

static cJSON *find_item(const char *id)
{
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(s_root, "items")) {
        const cJSON *pid = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (cJSON_IsString(pid) && strcmp(pid->valuestring, id) == 0) {
            return item;
        }
    }
    return NULL;
}

esp_err_t pp_countdowns_upsert(const char *json, char *new_id, size_t new_id_size)
{
    cJSON *body = cJSON_Parse(json);
    if (body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char title[96] = {0};
    copy_json_string(body, "title", title, sizeof(title));
    char date[11] = {0};
    copy_json_string(body, "date", date, sizeof(date));
    if (title[0] == '\0' || !parse_date(date, &(int){0}, &(int){0}, &(int){0})) {
        cJSON_Delete(body);
        return ESP_ERR_INVALID_ARG;
    }
    char note[160] = {0};
    copy_json_string(body, "note", note, sizeof(note));
    const cJSON *yearly = cJSON_GetObjectItemCaseSensitive(body, "yearly");
    bool is_yearly = cJSON_IsTrue(yearly);
    char id[12] = {0};
    copy_json_string(body, "id", id, sizeof(id));

    lock();
    cJSON *item = id[0] ? find_item(id) : NULL;
    if (item == NULL) {
        cJSON *arr = cJSON_GetObjectItemCaseSensitive(s_root, "items");
        if (cJSON_GetArraySize(arr) >= PP_COUNTDOWNS_MAX) {
            unlock();
            cJSON_Delete(body);
            return ESP_ERR_NO_MEM;
        }
        int max_n = 0;
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, arr) {
            const cJSON *pid = cJSON_GetObjectItemCaseSensitive(it, "id");
            if (cJSON_IsString(pid) && pid->valuestring[0] == 'c') {
                int n = atoi(pid->valuestring + 1);
                if (n > max_n) {
                    max_n = n;
                }
            }
        }
        if (max_n >= 9999) {
            unlock();
            cJSON_Delete(body);
            return ESP_ERR_NO_MEM;
        }
        snprintf(id, sizeof(id), "c%d", max_n + 1);
        item = cJSON_CreateObject();
        if (item == NULL) {
            unlock();
            cJSON_Delete(body);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(item, "id", id);
        cJSON_AddItemToArray(arr, item);
    }

    cJSON_DeleteItemFromObjectCaseSensitive(item, "title");
    cJSON_AddStringToObject(item, "title", title);
    cJSON_DeleteItemFromObjectCaseSensitive(item, "date");
    cJSON_AddStringToObject(item, "date", date);
    cJSON_DeleteItemFromObjectCaseSensitive(item, "note");
    cJSON_AddStringToObject(item, "note", note);
    cJSON_DeleteItemFromObjectCaseSensitive(item, "yearly");
    cJSON_AddBoolToObject(item, "yearly", is_yearly);
    if (new_id && new_id_size) {
        const cJSON *pid = cJSON_GetObjectItemCaseSensitive(item, "id");
        snprintf(new_id, new_id_size, "%s", cJSON_IsString(pid) ? pid->valuestring : id);
    }
    esp_err_t err = save_locked();
    refresh_locked();
    unlock();
    cJSON_Delete(body);
    return err;
}

esp_err_t pp_countdowns_delete(const char *id)
{
    lock();
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(s_root, "items");
    int count = cJSON_GetArraySize(arr);
    int found = -1;
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        const cJSON *pid = cJSON_GetObjectItemCaseSensitive(item, "id");
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

char *pp_countdowns_json(void)
{
    lock();
    refresh_locked();
    cJSON *out = cJSON_CreateObject();
    if (out == NULL) {
        unlock();
        return NULL;
    }
    cJSON_AddStringToObject(out, "today", s_today);
    cJSON *arr = cJSON_AddArrayToObject(out, "items");
    for (int i = 0; i < s_view_count && arr != NULL; i++) {
        const pp_countdown_t *v = &s_view[i];
        cJSON *copy = cJSON_CreateObject();
        if (copy == NULL) {
            break;
        }
        cJSON_AddStringToObject(copy, "id", v->id);
        cJSON_AddStringToObject(copy, "title", v->title);
        cJSON_AddStringToObject(copy, "date", v->date);
        cJSON_AddStringToObject(copy, "note", v->note);
        cJSON_AddBoolToObject(copy, "yearly", v->yearly);
        cJSON_AddNumberToObject(copy, "days_left", v->days_left);
        cJSON_AddItemToArray(arr, copy);
    }
    char *text = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    unlock();
    return text;
}
