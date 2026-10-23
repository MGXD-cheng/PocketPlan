/*
 * Alarms: a small list in alarms.json, a one second timer task that compares
 * the local wall clock, and a full screen ring overlay with the buzzer on
 * TCA9554 P7 (on/off only, so the ring is a beep pattern).
 */
#include "alarms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "cJSON.h"
#include "clock.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "pp_fonts.h"
#include "store.h"

static const char *TAG = "alarms";

#define RING_TIMEOUT_MS (60 * 1000)
#define BEEP_ON_MS 350
#define BEEP_OFF_MS 300

static cJSON *s_root;
static pp_alarm_t s_view[PP_ALARMS_MAX];
static int s_view_count;
static SemaphoreHandle_t s_lock;

static volatile bool s_ringing;
static char s_ring_time[6];
static char s_ring_label[48];
static lv_obj_t *s_overlay;
static TickType_t s_ring_started;
static char s_last_key[40];   /* "<date> <HH:MM> <id>" of the last ring */

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

static void copy_json_string(const cJSON *obj, const char *key, char *out, size_t out_size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) {
        snprintf(out, out_size, "%s", v->valuestring);
    }
}

/* ------------------------------- validation ----------------------------- */
static bool parse_hhmm(const char *text, int *hour, int *minute)
{
    if (text == NULL || strlen(text) != 5 || text[2] != ':') {
        return false;
    }
    int h = 0, m = 0;
    if (sscanf(text, "%2d:%2d", &h, &m) != 2) {
        return false;
    }
    if (h < 0 || h > 23 || m < 0 || m > 59) {
        return false;
    }
    *hour = h;
    *minute = m;
    return true;
}

static int weekday_index(void)
{
    char wd[4] = {0};
    pp_clock_format(wd, sizeof(wd), "%u"); /* 1 = Monday .. 7 = Sunday */
    int n = atoi(wd);
    return (n >= 1 && n <= 7) ? n - 1 : 0;
}

static bool matches_today(const cJSON *item)
{
    const cJSON *days = cJSON_GetObjectItemCaseSensitive(item, "days");
    int mask = cJSON_IsNumber(days) ? days->valueint : 0;
    if (mask <= 0) {
        return true; /* every day */
    }
    return (mask & (1 << weekday_index())) != 0;
}

/* ------------------------------- persistence ---------------------------- */
static esp_err_t save_locked(void)
{
    char *text = cJSON_PrintUnformatted(s_root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = pp_data_write("alarms.json", text, strlen(text));
    cJSON_free(text);
    return err;
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

static void refresh_locked(void)
{
    s_view_count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(s_root, "items")) {
        if (s_view_count >= PP_ALARMS_MAX) {
            break;
        }
        pp_alarm_t *dst = &s_view[s_view_count];
        memset(dst, 0, sizeof(*dst));
        copy_json_string(item, "id", dst->id, sizeof(dst->id));
        copy_json_string(item, "time", dst->time, sizeof(dst->time));
        copy_json_string(item, "label", dst->label, sizeof(dst->label));
        const cJSON *days = cJSON_GetObjectItemCaseSensitive(item, "days");
        if (cJSON_IsNumber(days) && days->valueint > 0) {
            dst->days = (uint8_t)days->valueint;
        }
        const cJSON *en = cJSON_GetObjectItemCaseSensitive(item, "enabled");
        dst->enabled = !cJSON_IsBool(en) || cJSON_IsTrue(en);
        dst->active_today = matches_today(item);
        s_view_count++;
    }
    /* enabled first, then by time */
    for (int i = 1; i < s_view_count; i++) {
        pp_alarm_t key = s_view[i];
        int j = i - 1;
        while (j >= 0) {
            bool before = key.enabled != s_view[j].enabled
                              ? key.enabled
                              : (strcmp(key.time, s_view[j].time) < 0);
            if (!before) {
                break;
            }
            s_view[j + 1] = s_view[j];
            j--;
        }
        s_view[j + 1] = key;
    }
}

/* --------------------------------- ring --------------------------------- */
static void ring_clicked(lv_event_t *e)
{
    (void)e;
    pp_alarm_stop();
}

static void build_overlay(const char *time_text, const char *label)
{
    lv_obj_t *scr = lv_screen_active();
    s_overlay = lv_obj_create(scr);
    if (s_overlay == NULL) {
        return;
    }
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_overlay, 20, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_overlay);
    lv_label_set_text(title, "闹钟");
    lv_obj_set_style_text_font(title, &pp_font_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x000000), 0);

    lv_obj_t *clock = lv_label_create(s_overlay);
    lv_label_set_text(clock, time_text);
    lv_obj_set_style_text_font(clock, &pp_font_96, 0);
    lv_obj_set_style_text_color(clock, lv_color_hex(0x000000), 0);

    lv_obj_t *text = lv_label_create(s_overlay);
    lv_label_set_text(text, label[0] ? label : "时间到了");
    lv_obj_set_style_text_font(text, &pp_font_20, 0);
    lv_obj_set_style_text_color(text, lv_color_hex(0x000000), 0);
    lv_obj_set_width(text, lv_pct(90));
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);

    lv_obj_t *stop = lv_button_create(s_overlay);
    lv_obj_set_size(stop, 200, 64);
    lv_obj_set_style_radius(stop, 14, 0);
    lv_obj_set_style_bg_color(stop, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(stop, 0, 0);
    lv_obj_set_style_shadow_width(stop, 0, 0);
    lv_obj_add_event_cb(stop, ring_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(stop);
    lv_label_set_text(sl, "停止");
    lv_obj_set_style_text_font(sl, &pp_font_16, 0);
    lv_obj_set_style_text_color(sl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(sl);
}

static void ring_task(void *arg)
{
    (void)arg;
    bool on = false;
    while (s_ringing) {
        on = !on;
        pp_board_buzzer(on);
        vTaskDelay(pdMS_TO_TICKS(on ? BEEP_ON_MS : BEEP_OFF_MS));
    }
    pp_board_buzzer(false);
    vTaskDelete(NULL);
}

static void start_ring(const pp_alarm_t *alarm)
{
    if (s_ringing) {
        return;
    }
    s_ringing = true;
    snprintf(s_ring_time, sizeof(s_ring_time), "%s", alarm->time);
    snprintf(s_ring_label, sizeof(s_ring_label), "%s", alarm->label);
    s_ring_started = xTaskGetTickCount();
    lvgl_port_lock(0);
    build_overlay(s_ring_time, s_ring_label);
    lvgl_port_unlock();
    if (xTaskCreate(ring_task, "pp_ring", 2048, NULL, 6, NULL) != pdPASS) {
        pp_board_buzzer(false);
        s_ringing = false;
    }
    ESP_LOGI(TAG, "ringing for %s %s", s_ring_time, s_ring_label);
}

void pp_alarm_stop(void)
{
    if (!s_ringing) {
        return;
    }
    s_ringing = false;
    pp_board_buzzer(false);
    lvgl_port_lock(0);
    if (s_overlay != NULL) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
    }
    lvgl_port_unlock();
    ESP_LOGI(TAG, "alarm stopped");
}

bool pp_alarms_ringing(void)
{
    return s_ringing;
}

void pp_alarms_demo_ring(void)
{
    pp_alarm_t demo = {0};
    snprintf(demo.id, sizeof(demo.id), "demo");
    snprintf(demo.label, sizeof(demo.label), "起床");
    pp_clock_format(demo.time, sizeof(demo.time), "%H:%M");
    start_ring(&demo);
}

/* -------------------------------- timer task ---------------------------- */
static void alarm_task(void *arg)
{
    (void)arg;
    char now[8] = {0};
    char date[12] = {0};
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_ringing) {
            if (xTaskGetTickCount() - s_ring_started > pdMS_TO_TICKS(RING_TIMEOUT_MS)) {
                pp_alarm_stop();
            }
            continue;
        }
        if (!pp_clock_valid()) {
            continue;
        }
        pp_clock_format(now, sizeof(now), "%H:%M");
        pp_clock_format(date, sizeof(date), "%Y-%m-%d");
        lock();
        for (int i = 0; i < s_view_count; i++) {
            const pp_alarm_t *alarm = &s_view[i];
            if (!alarm->enabled || strcmp(alarm->time, now) != 0) {
                continue;
            }
            if (alarm->days != 0 && (alarm->days & (1 << weekday_index())) == 0) {
                continue;
            }
            char key[40];
            snprintf(key, sizeof(key), "%s %s %s", date, now, alarm->id);
            if (strcmp(key, s_last_key) == 0) {
                continue;
            }
            snprintf(s_last_key, sizeof(s_last_key), "%s", key);
            start_ring(alarm);
            break;
        }
        unlock();
    }
}

/* ---------------------------------- API --------------------------------- */
esp_err_t pp_alarms_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    size_t len = 0;
    char *text = pp_data_read("alarms.json", &len);
    if (text != NULL) {
        s_root = cJSON_Parse(text);
        free(text);
    }
    if (s_root == NULL) {
        s_root = cJSON_CreateObject();
        cJSON_AddNumberToObject(s_root, "version", 1);
        cJSON_AddArrayToObject(s_root, "items");
        refresh_locked();
        save_locked();
    }
    pp_alarms_refresh();
    if (xTaskCreate(alarm_task, "pp_alarm", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the alarm timer");
    }
    ESP_LOGI(TAG, "loaded %d alarms", s_view_count);
    return ESP_OK;
}

void pp_alarms_refresh(void)
{
    lock();
    refresh_locked();
    unlock();
}

int pp_alarms_count(void)
{
    return s_view_count;
}

const pp_alarm_t *pp_alarms_get(int index)
{
    if (index < 0 || index >= s_view_count) {
        return NULL;
    }
    return &s_view[index];
}

static int next_id(void)
{
    int max = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(s_root, "items")) {
        const cJSON *pid = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (cJSON_IsString(pid) && pid->valuestring[0] == 'a') {
            int n = atoi(pid->valuestring + 1);
            if (n > max) {
                max = n;
            }
        }
    }
    return max + 1;
}

esp_err_t pp_alarms_upsert(const char *json, char *new_id, size_t new_id_size)
{
    cJSON *body = cJSON_Parse(json);
    if (body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *time = cJSON_GetObjectItemCaseSensitive(body, "time");
    int hour = 0, minute = 0;
    if (!cJSON_IsString(time) || !parse_hhmm(time->valuestring, &hour, &minute)) {
        cJSON_Delete(body);
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(body, "id");
    const char *id_text = cJSON_IsString(id) ? id->valuestring : NULL;

    lock();
    cJSON *item = NULL;
    if (id_text && id_text[0]) {
        item = find_item(id_text);
        if (item == NULL) {
            unlock();
            cJSON_Delete(body);
            return ESP_ERR_NOT_FOUND;
        }
    } else {
        cJSON *items = cJSON_GetObjectItemCaseSensitive(s_root, "items");
        int count = cJSON_GetArraySize(items);
        if (count >= PP_ALARMS_MAX) {
            unlock();
            cJSON_Delete(body);
            return ESP_ERR_NO_MEM;
        }
        item = cJSON_CreateObject();
        char nid[8];
        snprintf(nid, sizeof(nid), "a%d", next_id());
        cJSON_AddStringToObject(item, "id", nid);
        cJSON_AddItemToArray(items, item);
    }

    cJSON *old = cJSON_Duplicate(item, true);
    cJSON_DeleteItemFromObjectCaseSensitive(item, "time");
    cJSON_AddStringToObject(item, "time", time->valuestring);
    const cJSON *label = cJSON_GetObjectItemCaseSensitive(body, "label");
    cJSON_DeleteItemFromObjectCaseSensitive(item, "label");
    cJSON_AddStringToObject(item, "label",
                            cJSON_IsString(label) ? label->valuestring : "闹钟");
    const cJSON *days = cJSON_GetObjectItemCaseSensitive(body, "days");
    int mask = cJSON_IsNumber(days) ? days->valueint : 0;
    if (mask < 0) {
        mask = 0;
    }
    if (mask > 0x7F) {
        mask = 0x7F;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(item, "days");
    cJSON_AddNumberToObject(item, "days", mask);
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(body, "enabled");
    cJSON_DeleteItemFromObjectCaseSensitive(item, "enabled");
    cJSON_AddBoolToObject(item, "enabled", !cJSON_IsBool(en) || cJSON_IsTrue(en));

    esp_err_t err = save_locked();
    refresh_locked();
    if (new_id && new_id_size) {
        const cJSON *pid = cJSON_GetObjectItemCaseSensitive(item, "id");
        snprintf(new_id, new_id_size, "%s", cJSON_IsString(pid) ? pid->valuestring : "");
    }
    unlock();
    cJSON_Delete(old);
    cJSON_Delete(body);
    return err;
}

esp_err_t pp_alarms_delete(const char *id)
{
    lock();
    cJSON *items = cJSON_GetObjectItemCaseSensitive(s_root, "items");
    /* cJSON has no delete-by-pointer: find the index and delete that one. */
    int index = -1;
    for (int i = 0; i < cJSON_GetArraySize(items); i++) {
        cJSON *candidate = cJSON_GetArrayItem(items, i);
        const cJSON *pid = cJSON_GetObjectItemCaseSensitive(candidate, "id");
        if (cJSON_IsString(pid) && strcmp(pid->valuestring, id) == 0) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    cJSON_DeleteItemFromArray(items, index);
    esp_err_t err = save_locked();
    refresh_locked();
    unlock();
    return err;
}

esp_err_t pp_alarms_set_enabled(const char *id, bool enabled)
{
    lock();
    cJSON *item = find_item(id);
    if (item == NULL) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(item, "enabled");
    cJSON_AddBoolToObject(item, "enabled", enabled);
    esp_err_t err = save_locked();
    refresh_locked();
    unlock();
    return err;
}

char *pp_alarms_json(void)
{
    lock();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        unlock();
        return NULL;
    }
    char now[8];
    pp_clock_format(now, sizeof(now), "%H:%M");
    cJSON_AddStringToObject(root, "now", now);
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    for (int i = 0; i < s_view_count; i++) {
        const pp_alarm_t *alarm = &s_view[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", alarm->id);
        cJSON_AddStringToObject(item, "time", alarm->time);
        cJSON_AddStringToObject(item, "label", alarm->label);
        cJSON_AddNumberToObject(item, "days", alarm->days);
        cJSON_AddBoolToObject(item, "enabled", alarm->enabled);
        cJSON_AddBoolToObject(item, "active_today", alarm->active_today);
        cJSON_AddItemToArray(items, item);
    }
    cJSON_AddBoolToObject(root, "ringing", s_ringing);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    unlock();
    return text;
}
