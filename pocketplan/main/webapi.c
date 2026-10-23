/*
 * REST API + embedded single page app. Everything here is registered before
 * the WebDAV wildcard handlers so "/" and the API paths win.
 */
#include "webapi.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "ai.h"
#include "alarms.h"
#include "autosleep.h"
#include "board.h"
#include "cJSON.h"
#include "clock.h"
#include "config.h"
#include "countdowns.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "focus.h"
#include "lvgl.h"
#include "net.h"
#include "pages.h"
#include "plans.h"
#include "shot.h"
#include "store.h"
#include "ui.h"

static const char *TAG = "webapi";

/* Embedded by CMake (EMBED_FILES "www/index.html"). */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

#define MAX_BODY 8192

static esp_err_t reply_json(httpd_req_t *req, const char *status, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, text ? text : "{}", HTTPD_RESP_USE_STRLEN);
    free(text);
    cJSON_Delete(root);
    return err;
}

static esp_err_t reply_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    return reply_json(req, status, root);
}

static esp_err_t reply_ok(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return reply_json(req, "200 OK", root);
}

static char *read_body(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY) {
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    if (buf == NULL) {
        return NULL;
    }
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = '\0';
    return buf;
}

static cJSON *parse_body(httpd_req_t *req)
{
    char *text = read_body(req);
    if (text == NULL) {
        return NULL;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    return root;
}

static void uri_unquote(char *s)
{
    char *d = s;
    for (const char *p = s; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], 0};
            *d++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else if (*p == '+') {
            *d++ = ' ';
        } else {
            *d++ = *p;
        }
    }
    *d = '\0';
}

static bool query_value(httpd_req_t *req, const char *key, char *out, size_t out_size)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char value[256];
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        return false;
    }
    snprintf(out, out_size, "%s", value);
    uri_unquote(out);
    return true;
}

/* Move the data files between SPIFFS and the SD card, then reboot so every
 * module reopens its files from the new location. */
static void migrate_task(void *arg)
{
    (void)arg;
    esp_err_t err = pp_store_migrate_data();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "migrate failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    vTaskDelete(NULL);
}

/* ------------------------------- GET / ---------------------------------- */
static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    size_t len = (size_t)(index_html_end - index_html_start);
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

static esp_err_t h_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ----------------------------- GET /api/status -------------------------- */
static esp_err_t h_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", "PocketPlan 1.0");

    uint32_t total = 0, free_kb = 0;
    if (pp_board_sd_mounted()) {
        pp_board_sd_usage(&total, &free_kb);
    }
    cJSON *sd = cJSON_AddObjectToObject(root, "sd");
    cJSON_AddBoolToObject(sd, "mounted", pp_board_sd_mounted());
    cJSON_AddNumberToObject(sd, "total_kb", total);
    cJSON_AddNumberToObject(sd, "free_kb", free_kb);

    cJSON *lfs = cJSON_AddObjectToObject(root, "lfs");
    cJSON_AddBoolToObject(lfs, "ready", pp_store_ready());
    cJSON_AddNumberToObject(lfs, "free_kb", pp_store_free_kb());

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    char ip[32] = {0};
    pp_net_sta_ip(ip, sizeof(ip));
    cJSON_AddBoolToObject(wifi, "sta", pp_net_sta_connected());
    cJSON_AddStringToObject(wifi, "ssid", pp_net_saved_ssid());
    cJSON_AddStringToObject(wifi, "ip", ip);
    pp_net_ap_ip(ip, sizeof(ip));
    cJSON_AddStringToObject(wifi, "ap_ssid", pp_net_ap_ssid());
    cJSON_AddStringToObject(wifi, "ap_ip", ip);

    cJSON *mem = cJSON_AddObjectToObject(root, "mem");
    cJSON_AddNumberToObject(mem, "internal_free",
                            (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(
        mem, "internal_largest",
        (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(mem, "psram_free",
                            (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    char now[32];
    pp_clock_format(now, sizeof(now), "%Y-%m-%d %H:%M");
    cJSON_AddStringToObject(root, "time", now);
    cJSON_AddStringToObject(root, "today", pp_plans_today());
    cJSON_AddBoolToObject(root, "focus", pp_focus_active());
    cJSON_AddBoolToObject(root, "focus_dark", pp_config()->focus_dark);
    cJSON_AddBoolToObject(root, "data_on_sd", pp_data_on_sd());
    cJSON_AddBoolToObject(root, "alarm_ringing", pp_alarms_ringing());
    cJSON_AddBoolToObject(root, "asleep", pp_autosleep_active());
    return reply_json(req, "200 OK", root);
}

/* ------------------------------ /api/plans ------------------------------ */
static esp_err_t h_plans_get(httpd_req_t *req)
{
    char *text = pp_plans_json();
    if (text == NULL) {
        return reply_error(req, "500 Internal Server Error", "plans unavailable");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    return err;
}

static esp_err_t h_plans_post(httpd_req_t *req)
{
    cJSON *body = parse_body(req);
    if (body == NULL) {
        return reply_error(req, "400 Bad Request", "invalid json");
    }
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (text == NULL) {
        return reply_error(req, "500 Internal Server Error", "out of memory");
    }
    char new_id[12] = {0};
    esp_err_t err = pp_plans_upsert(text, new_id, sizeof(new_id));
    free(text);
    if (err != ESP_OK) {
        return reply_error(req, "500 Internal Server Error", "save failed");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "id", new_id);
    return reply_json(req, "200 OK", root);
}

static esp_err_t h_plans_delete(httpd_req_t *req)
{
    char id[16];
    if (!query_value(req, "id", id, sizeof(id)) || id[0] == '\0') {
        return reply_error(req, "400 Bad Request", "id required");
    }
    esp_err_t err = pp_plans_delete(id);
    if (err == ESP_ERR_NOT_FOUND) {
        return reply_error(req, "404 Not Found", "no such plan");
    }
    if (err != ESP_OK) {
        return reply_error(req, "500 Internal Server Error", "delete failed");
    }
    return reply_ok(req);
}

/* ----------------------------- /api/checkin ----------------------------- */
static esp_err_t h_checkin(httpd_req_t *req)
{
    cJSON *body = parse_body(req);
    if (body == NULL) {
        return reply_error(req, "400 Bad Request", "invalid json");
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(body, "id");
    const cJSON *date = cJSON_GetObjectItemCaseSensitive(body, "date");
    const cJSON *done = cJSON_GetObjectItemCaseSensitive(body, "done");
    if (!cJSON_IsString(id)) {
        cJSON_Delete(body);
        return reply_error(req, "400 Bad Request", "id required");
    }
    bool state = false;
    esp_err_t err = pp_plans_checkin(id->valuestring, cJSON_IsString(date) ? date->valuestring : NULL,
                                      cJSON_IsBool(done) && cJSON_IsTrue(done) ? 1 : 0, &state);
    cJSON_Delete(body);
    if (err == ESP_ERR_NOT_FOUND) {
        return reply_error(req, "404 Not Found", "no such plan");
    }
    if (err != ESP_OK) {
        return reply_error(req, "500 Internal Server Error", "checkin failed");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "done", state);
    return reply_json(req, "200 OK", root);
}

/* --------------------------- /api/countdowns ---------------------------- */
static esp_err_t h_countdowns_get(httpd_req_t *req)
{
    char *text = pp_countdowns_json();
    if (text == NULL) {
        return reply_error(req, "500 Internal Server Error", "countdowns unavailable");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    return err;
}

static esp_err_t h_countdowns_post(httpd_req_t *req)
{
    cJSON *body = parse_body(req);
    if (body == NULL) {
        return reply_error(req, "400 Bad Request", "invalid json");
    }
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (text == NULL) {
        return reply_error(req, "500 Internal Server Error", "out of memory");
    }
    char new_id[12] = {0};
    esp_err_t err = pp_countdowns_upsert(text, new_id, sizeof(new_id));
    free(text);
    if (err == ESP_ERR_NOT_FOUND) {
        return reply_error(req, "404 Not Found", "no such countdown");
    }
    if (err == ESP_ERR_NO_MEM) {
        return reply_error(req, "507 Insufficient Storage", "countdown list full");
    }
    if (err != ESP_OK) {
        return reply_error(req, "400 Bad Request", "title and date (YYYY-MM-DD) required");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "id", new_id);
    pp_countdown_refresh();
    return reply_json(req, "200 OK", root);
}

static esp_err_t h_countdowns_delete(httpd_req_t *req)
{
    char id[16];
    if (!query_value(req, "id", id, sizeof(id)) || id[0] == '\0') {
        return reply_error(req, "400 Bad Request", "id required");
    }
    esp_err_t err = pp_countdowns_delete(id);
    if (err == ESP_ERR_NOT_FOUND) {
        return reply_error(req, "404 Not Found", "no such countdown");
    }
    if (err != ESP_OK) {
        return reply_error(req, "500 Internal Server Error", "delete failed");
    }
    pp_countdown_refresh();
    return reply_ok(req);
}

/* ------------------------------ /api/focus ------------------------------ */
static esp_err_t h_focus(httpd_req_t *req)
{
    cJSON *body = parse_body(req);
    if (body == NULL) {
        return reply_error(req, "400 Bad Request", "invalid json");
    }
    const cJSON *on = cJSON_GetObjectItemCaseSensitive(body, "on");
    bool enable = !cJSON_IsBool(on) || cJSON_IsTrue(on);
    cJSON_Delete(body);
    if (enable) {
        pp_focus_open();
    } else {
        pp_focus_close();
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "focus", pp_focus_active());
    return reply_json(req, "200 OK", root);
}

/* ----------------------------- /api/alarms ------------------------------ */
static esp_err_t h_alarms_get(httpd_req_t *req)
{
    char *text = pp_alarms_json();
    if (text == NULL) {
        return reply_error(req, "500 Internal Server Error", "alarms unavailable");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    return err;
}

static esp_err_t h_alarms_post(httpd_req_t *req)
{
    char *text = read_body(req);
    if (text == NULL) {
        return reply_error(req, "400 Bad Request", "invalid json");
    }
    char id[16] = {0};
    esp_err_t err = pp_alarms_upsert(text, id, sizeof(id));
    free(text);
    if (err == ESP_ERR_INVALID_ARG) {
        return reply_error(req, "400 Bad Request", "time must be HH:MM");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return reply_error(req, "404 Not Found", "no such alarm");
    }
    if (err == ESP_ERR_NO_MEM) {
        return reply_error(req, "409 Conflict", "too many alarms");
    }
    if (err != ESP_OK) {
        return reply_error(req, "500 Internal Server Error", "save failed");
    }
    pp_countdown_refresh();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "id", id);
    return reply_json(req, "200 OK", root);
}

static esp_err_t h_alarms_delete(httpd_req_t *req)
{
    char id[16] = {0};
    if (!query_value(req, "id", id, sizeof(id)) || id[0] == '\0') {
        return reply_error(req, "400 Bad Request", "id required");
    }
    esp_err_t err = pp_alarms_delete(id);
    if (err == ESP_ERR_NOT_FOUND) {
        return reply_error(req, "404 Not Found", "no such alarm");
    }
    if (err != ESP_OK) {
        return reply_error(req, "500 Internal Server Error", "delete failed");
    }
    pp_countdown_refresh();
    return reply_ok(req);
}

static esp_err_t h_alarm_stop(httpd_req_t *req)
{
    pp_alarm_stop();
    return reply_ok(req);
}

/* ----------------------------- /api/config ------------------------------ */
static esp_err_t h_config_get(httpd_req_t *req)
{
    pp_config_t *cfg = pp_config();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ai_base", cfg->ai_base);
    cJSON_AddStringToObject(root, "ai_model", cfg->ai_model);
    cJSON_AddStringToObject(root, "ai_persona", cfg->ai_persona);
    cJSON_AddBoolToObject(root, "key_set", cfg->ai_key[0] != '\0');
    cJSON_AddStringToObject(root, "tz", cfg->tz);
    cJSON_AddNumberToObject(root, "brightness", cfg->brightness);
    cJSON_AddNumberToObject(root, "font_size", cfg->font_size);
    cJSON_AddNumberToObject(root, "line_space", cfg->line_space);
    cJSON_AddBoolToObject(root, "music_ready", cfg->music_ready);
    cJSON_AddBoolToObject(root, "focus_dark", cfg->focus_dark);
    cJSON_AddStringToObject(root, "focus_cd", cfg->focus_cd);
    cJSON_AddBoolToObject(root, "cache_sd", cfg->cache_sd);
    cJSON_AddBoolToObject(root, "auto_sleep", cfg->auto_sleep);
    cJSON_AddNumberToObject(root, "sleep_min", cfg->sleep_min);
    cJSON_AddBoolToObject(root, "data_on_sd", pp_data_on_sd());
    return reply_json(req, "200 OK", root);
}

static void copy_if_string(const cJSON *body, const char *key, char *dest, size_t dest_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(body, key);
    if (cJSON_IsString(item) && item->valuestring[0] != '\0') {
        snprintf(dest, dest_size, "%s", item->valuestring);
    }
}

static esp_err_t h_config_post(httpd_req_t *req)
{
    cJSON *body = parse_body(req);
    if (body == NULL) {
        return reply_error(req, "400 Bad Request", "invalid json");
    }
    pp_config_t *cfg = pp_config();
    copy_if_string(body, "ai_base", cfg->ai_base, sizeof(cfg->ai_base));
    copy_if_string(body, "ai_key", cfg->ai_key, sizeof(cfg->ai_key));
    copy_if_string(body, "ai_model", cfg->ai_model, sizeof(cfg->ai_model));
    copy_if_string(body, "ai_persona", cfg->ai_persona, sizeof(cfg->ai_persona));
    copy_if_string(body, "tz", cfg->tz, sizeof(cfg->tz));

    const cJSON *bri = cJSON_GetObjectItemCaseSensitive(body, "brightness");
    if (cJSON_IsNumber(bri) && bri->valueint >= 10 && bri->valueint <= 100) {
        cfg->brightness = (uint8_t)bri->valueint;
    }
    const cJSON *fs = cJSON_GetObjectItemCaseSensitive(body, "font_size");
    if (cJSON_IsNumber(fs) && (fs->valueint == 16 || fs->valueint == 20)) {
        cfg->font_size = (uint8_t)fs->valueint;
    }
    const cJSON *ls = cJSON_GetObjectItemCaseSensitive(body, "line_space");
    if (cJSON_IsNumber(ls) && ls->valueint >= 0 && ls->valueint <= 16) {
        cfg->line_space = (uint8_t)ls->valueint;
    }
    const cJSON *mr = cJSON_GetObjectItemCaseSensitive(body, "music_ready");
    if (cJSON_IsBool(mr)) {
        cfg->music_ready = cJSON_IsTrue(mr);
    }
    const cJSON *dark = cJSON_GetObjectItemCaseSensitive(body, "focus_dark");
    if (cJSON_IsBool(dark)) {
        cfg->focus_dark = cJSON_IsTrue(dark);
    }
    /* An empty string clears the selection, so this one cannot use
     * copy_if_string(). */
    const cJSON *fcd = cJSON_GetObjectItemCaseSensitive(body, "focus_cd");
    if (cJSON_IsString(fcd)) {
        snprintf(cfg->focus_cd, sizeof(cfg->focus_cd), "%s", fcd->valuestring);
    }
    bool wake = false;
    const cJSON *asleep = cJSON_GetObjectItemCaseSensitive(body, "auto_sleep");
    if (cJSON_IsBool(asleep)) {
        cfg->auto_sleep = cJSON_IsTrue(asleep);
        wake = !cfg->auto_sleep;
    }
    const cJSON *smin = cJSON_GetObjectItemCaseSensitive(body, "sleep_min");
    if (cJSON_IsNumber(smin) && smin->valueint >= 1 && smin->valueint <= 60) {
        cfg->sleep_min = (uint8_t)smin->valueint;
    }
    bool move_data = false;
    const cJSON *cache = cJSON_GetObjectItemCaseSensitive(body, "cache_sd");
    if (cJSON_IsBool(cache) && cJSON_IsTrue(cache) != cfg->cache_sd) {
        cfg->cache_sd = cJSON_IsTrue(cache);
        move_data = true;
    }
    cJSON_Delete(body);

    esp_err_t err = pp_config_save();
    if (err != ESP_OK) {
        return reply_error(req, "500 Internal Server Error", "save failed");
    }
    if (wake) {
        pp_autosleep_wake();
    }
    /* While the screen is asleep the backlight must stay off, otherwise a
     * settings save from the web page would light it up for no reason. */
    if (!pp_autosleep_active()) {
        pp_board_backlight(cfg->brightness);
    }
    pp_clock_apply_tz();
    if (move_data) {
        /* Answer first, then move the files and reboot: the browser sees a
         * clean 200 instead of a dropped connection. */
        esp_err_t res = reply_ok(req);
        if (xTaskCreate(migrate_task, "pp_cache", 4096, NULL, 3, NULL) != pdPASS) {
            esp_restart();
        }
        return res;
    }
    return reply_ok(req);
}

/* -------------------------------- /api/ai ------------------------------- */
/* The cached conversation, so the web chat shows the same history as the
 * device after a reload. */
static esp_err_t h_ai_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "configured", pp_ai_configured());
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    for (int i = 0; i < pp_ai_history_count(); i++) {
        const char *role = pp_ai_history_role(i);
        const char *text = pp_ai_history_text(i);
        if (text == NULL) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "role", role ? role : "assistant");
        cJSON_AddStringToObject(item, "text", text);
        cJSON_AddItemToArray(items, item);
    }
    cJSON_AddBoolToObject(root, "data_on_sd", pp_data_on_sd());
    return reply_json(req, "200 OK", root);
}

static esp_err_t h_ai(httpd_req_t *req)
{
    cJSON *body = parse_body(req);
    if (body == NULL) {
        return reply_error(req, "400 Bad Request", "invalid json");
    }
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(body, "message");
    if (!cJSON_IsString(msg) || msg->valuestring[0] == '\0') {
        cJSON_Delete(body);
        return reply_error(req, "400 Bad Request", "message required");
    }
    char *message = strdup(msg->valuestring);
    cJSON_Delete(body);
    if (message == NULL) {
        return reply_error(req, "500 Internal Server Error", "out of memory");
    }
    char *reply = pp_ai_send(message);
    bool error = pp_ai_last_error();
    if (reply && !error) {
        pp_ai_history_push("user", message);
        pp_ai_history_push("assistant", reply);
    }
    free(message);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", reply != NULL && !error);
    cJSON_AddStringToObject(root, "reply", reply ? reply : "没有回复");
    free(reply);
    return reply_json(req, "200 OK", root);
}

/* ------------------------------- /api/files ----------------------------- */
static esp_err_t h_files(httpd_req_t *req)
{
    char path[256] = {0};
    query_value(req, "path", path, sizeof(path));
    char *rel = path;
    while (*rel == '/') {
        rel++;
    }
    if (strstr(rel, "..") != NULL) {
        return reply_error(req, "400 Bad Request", "bad path");
    }
    memmove(path, rel, strlen(rel) + 1);
    char full[400];
    if (path[0]) {
        snprintf(full, sizeof(full), "/sdcard/%s", path);
    } else {
        snprintf(full, sizeof(full), "/sdcard");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "path", path);
    cJSON *entries = cJSON_AddArrayToObject(root, "entries");
    DIR *dir = opendir(full);
    if (dir == NULL) {
        return reply_json(req, "200 OK", root);
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        char item_path[400];
        snprintf(item_path, sizeof(item_path), "%s/%s", full, de->d_name);
        struct stat st;
        if (stat(item_path, &st) != 0) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", de->d_name);
        cJSON_AddBoolToObject(item, "dir", S_ISDIR(st.st_mode));
        cJSON_AddNumberToObject(item, "size", (double)st.st_size);
        cJSON_AddNumberToObject(item, "mtime", (double)st.st_mtime);
        cJSON_AddItemToArray(entries, item);
    }
    closedir(dir);
    return reply_json(req, "200 OK", root);
}

/* ------------------------------- /api/wifi ------------------------------ */
static esp_err_t h_wifi_json(httpd_req_t *req)
{
    cJSON *body = parse_body(req);
    if (body == NULL) {
        return reply_error(req, "400 Bad Request", "invalid json");
    }
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(body, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(body, "password");
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') {
        cJSON_Delete(body);
        return reply_error(req, "400 Bad Request", "ssid required");
    }
    esp_err_t err = pp_net_set_credentials(ssid->valuestring,
                                           cJSON_IsString(pass) ? pass->valuestring : "");
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return reply_error(req, "500 Internal Server Error", "connect failed");
    }
    return reply_ok(req);
}


/* ---------------------------- /api/screenshot ---------------------------- */
/* Debug aid: returns the screen as a 1 bit PBM. ?serial=1 streams the same
 * bitmap over the UART instead, which is far faster than HTTP on this board. */
static esp_err_t h_screenshot(httpd_req_t *req)
{
    char query[64];
    char flag[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "serial", flag, sizeof(flag)) == ESP_OK) {
        pp_shot_dump_serial();
        return reply_ok(req);
    }

    uint8_t *img = NULL;
    size_t len = 0;
    uint32_t w = 0, h = 0;
    esp_err_t err = pp_shot_pbm(&img, &len, &w, &h);
    if (err != ESP_OK) {
        return reply_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    httpd_resp_set_type(req, "image/x-portable-bitmap");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send(req, (const char *)img, (ssize_t)len);
    free(img);
    ESP_LOGI(TAG, "screenshot %ux%u (%u bytes): %s", (unsigned)w, (unsigned)h,
             (unsigned)len, esp_err_to_name(err));
    return err;
}

esp_err_t pp_webapi_register(httpd_handle_t server)
{
    static const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/index.html", .method = HTTP_GET, .handler = h_index},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = h_favicon},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/plans", .method = HTTP_GET, .handler = h_plans_get},
        {.uri = "/api/plans", .method = HTTP_POST, .handler = h_plans_post},
        {.uri = "/api/plans", .method = HTTP_DELETE, .handler = h_plans_delete},
        {.uri = "/api/checkin", .method = HTTP_POST, .handler = h_checkin},
        {.uri = "/api/countdowns", .method = HTTP_GET, .handler = h_countdowns_get},
        {.uri = "/api/countdowns", .method = HTTP_POST, .handler = h_countdowns_post},
        {.uri = "/api/countdowns", .method = HTTP_DELETE, .handler = h_countdowns_delete},
        {.uri = "/api/focus", .method = HTTP_POST, .handler = h_focus},
        {.uri = "/api/alarms", .method = HTTP_GET, .handler = h_alarms_get},
        {.uri = "/api/alarms", .method = HTTP_POST, .handler = h_alarms_post},
        {.uri = "/api/alarms", .method = HTTP_DELETE, .handler = h_alarms_delete},
        {.uri = "/api/alarms/stop", .method = HTTP_POST, .handler = h_alarm_stop},
        {.uri = "/api/config", .method = HTTP_GET, .handler = h_config_get},
        {.uri = "/api/config", .method = HTTP_POST, .handler = h_config_post},
        {.uri = "/api/ai", .method = HTTP_GET, .handler = h_ai_get},
        {.uri = "/api/ai", .method = HTTP_POST, .handler = h_ai},
        {.uri = "/api/files", .method = HTTP_GET, .handler = h_files},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi_json},
        {.uri = "/api/screenshot", .method = HTTP_GET, .handler = h_screenshot},
    };
    int ok = 0;
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err == ESP_OK) {
            ok++;
        } else {
            ESP_LOGW(TAG, "register %s failed: %s", handlers[i].uri, esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "web app and API registered (%d handlers)", ok);
    return ok > 0 ? ESP_OK : ESP_FAIL;
}