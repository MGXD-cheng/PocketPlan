/*
 * Device configuration, persisted as JSON in /lfs/config.json so the web page
 * can edit it and a user can back it up by downloading one file.
 */
#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "store.h"

static const char *TAG = "config";

static pp_config_t s_cfg;

static void set_defaults(pp_config_t *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->ai_base, sizeof(c->ai_base), "%s", PP_AI_BASE_DEFAULT);
    snprintf(c->ai_model, sizeof(c->ai_model), "%s", PP_AI_MODEL_DEFAULT);
    snprintf(c->ai_persona, sizeof(c->ai_persona), "%s",
             "You are PocketPlan, a calm and concise daily planning assistant. "
             "Answer in the same language as the user, in at most 120 words.");
    snprintf(c->tz, sizeof(c->tz), "%s", "CST-8");
    c->brightness = 80;
    c->font_size = 16;
    c->line_space = 6;
    c->music_ready = false;
    c->focus_dark = false;
    c->focus_cd[0] = '\0';
    c->cache_sd = false;
    c->auto_sleep = true;
    c->sleep_min = 5;
}

static void read_string(const cJSON *root, const char *key, char *out, size_t out_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(out, out_size, "%s", item->valuestring);
    }
}

esp_err_t pp_config_load(void)
{
    set_defaults(&s_cfg);
    size_t len = 0;
    char *text = pp_store_read(PP_CONF_PATH, &len);
    if (text == NULL) {
        ESP_LOGI(TAG, "no config file, writing defaults");
        pp_config_save();
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (root == NULL) {
        ESP_LOGW(TAG, "config.json is not valid JSON, using defaults");
        pp_config_save();
        return ESP_FAIL;
    }
    read_string(root, "ai_base", s_cfg.ai_base, sizeof(s_cfg.ai_base));
    read_string(root, "ai_key", s_cfg.ai_key, sizeof(s_cfg.ai_key));
    read_string(root, "ai_model", s_cfg.ai_model, sizeof(s_cfg.ai_model));
    read_string(root, "ai_persona", s_cfg.ai_persona, sizeof(s_cfg.ai_persona));
    read_string(root, "tz", s_cfg.tz, sizeof(s_cfg.tz));

    const cJSON *b = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    if (cJSON_IsNumber(b)) {
        int v = b->valueint;
        s_cfg.brightness = (uint8_t)(v < 10 ? 10 : (v > 100 ? 100 : v));
    }
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(root, "font_size");
    if (cJSON_IsNumber(f)) {
        /* 16 = normal UI font, 20 = large reader font; anything older (14/16)
         * loads as 16. */
        s_cfg.font_size = (f->valueint >= 20) ? 20 : 16;
    }
    const cJSON *ls = cJSON_GetObjectItemCaseSensitive(root, "line_space");
    if (cJSON_IsNumber(ls)) {
        int v = ls->valueint;
        s_cfg.line_space = (uint8_t)(v < 0 ? 0 : (v > 16 ? 16 : v));
    }
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "music_ready");
    if (cJSON_IsBool(m)) {
        s_cfg.music_ready = cJSON_IsTrue(m);
    }
    const cJSON *fd = cJSON_GetObjectItemCaseSensitive(root, "focus_dark");
    if (cJSON_IsBool(fd)) {
        s_cfg.focus_dark = cJSON_IsTrue(fd);
    }
    read_string(root, "focus_cd", s_cfg.focus_cd, sizeof(s_cfg.focus_cd));
    const cJSON *cs = cJSON_GetObjectItemCaseSensitive(root, "cache_sd");
    if (cJSON_IsBool(cs)) {
        s_cfg.cache_sd = cJSON_IsTrue(cs);
    }
    const cJSON *as = cJSON_GetObjectItemCaseSensitive(root, "auto_sleep");
    if (cJSON_IsBool(as)) {
        s_cfg.auto_sleep = cJSON_IsTrue(as);
    }
    const cJSON *sm = cJSON_GetObjectItemCaseSensitive(root, "sleep_min");
    if (cJSON_IsNumber(sm)) {
        int v = sm->valueint;
        s_cfg.sleep_min = (uint8_t)(v < 1 ? 1 : (v > 60 ? 60 : v));
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "loaded config (ai model %s, key %s)", s_cfg.ai_model,
             s_cfg.ai_key[0] ? "set" : "unset");
    return ESP_OK;
}

esp_err_t pp_config_save(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "ai_base", s_cfg.ai_base);
    cJSON_AddStringToObject(root, "ai_key", s_cfg.ai_key);
    cJSON_AddStringToObject(root, "ai_model", s_cfg.ai_model);
    cJSON_AddStringToObject(root, "ai_persona", s_cfg.ai_persona);
    cJSON_AddStringToObject(root, "tz", s_cfg.tz);
    cJSON_AddNumberToObject(root, "brightness", s_cfg.brightness);
    cJSON_AddNumberToObject(root, "font_size", s_cfg.font_size);
    cJSON_AddNumberToObject(root, "line_space", s_cfg.line_space);
    cJSON_AddBoolToObject(root, "music_ready", s_cfg.music_ready);
    cJSON_AddBoolToObject(root, "focus_dark", s_cfg.focus_dark);
    cJSON_AddStringToObject(root, "focus_cd", s_cfg.focus_cd);
    cJSON_AddBoolToObject(root, "cache_sd", s_cfg.cache_sd);
    cJSON_AddBoolToObject(root, "auto_sleep", s_cfg.auto_sleep);
    cJSON_AddNumberToObject(root, "sleep_min", s_cfg.sleep_min);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = pp_store_write_atomic(PP_CONF_PATH, text, strlen(text));
    cJSON_free(text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot save config: %s", esp_err_to_name(err));
    }
    return err;
}

pp_config_t *pp_config(void)
{
    return &s_cfg;
}

void pp_config_reset(void)
{
    set_defaults(&s_cfg);
    pp_config_save();
}
