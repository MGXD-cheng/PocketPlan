#include "ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "store.h"

static const char *TAG = "ai";

#define MAX_REPLY (8 * 1024)
#define MAX_BODY  (16 * 1024)
#define HISTORY_LEN 1024

typedef struct {
    char role[12];
    char *text;
} hist_t;

static hist_t s_hist[PP_AI_HISTORY];
static int s_hist_count;
static SemaphoreHandle_t s_lock;
static bool s_last_error;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} buf_t;

static bool buf_append(buf_t *buf, const char *data, size_t len)
{
    if (buf->len + len + 1 > buf->cap) {
        size_t cap = buf->cap ? buf->cap : 2048;
        while (cap < buf->len + len + 1) {
            cap *= 2;
        }
        char *next = realloc(buf->data, cap);
        if (next == NULL) {
            return false;
        }
        buf->data = next;
        buf->cap = cap;
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return true;
}

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    buf_t *buf = (buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf) {
        buf_append(buf, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

/* Persist the rolling history so the conversation survives a reboot; the file
 * lives on the SD card when the "SD 卡缓存" switch is on. */
static void save_locked(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    for (int i = 0; i < s_hist_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "role", s_hist[i].role);
        cJSON_AddStringToObject(item, "content", s_hist[i].text);
        cJSON_AddItemToArray(items, item);
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text != NULL) {
        pp_data_write("ai_chat.json", text, strlen(text));
        cJSON_free(text);
    }
}

static void push_locked(const char *role, const char *content)
{
    if (role == NULL || content == NULL || content[0] == '\0') {
        return;
    }
    if (s_hist_count >= PP_AI_HISTORY) {
        free(s_hist[0].text);
        memmove(&s_hist[0], &s_hist[1], sizeof(hist_t) * (PP_AI_HISTORY - 1));
        s_hist_count--;
    }
    snprintf(s_hist[s_hist_count].role, sizeof(s_hist[s_hist_count].role), "%s", role);
    s_hist[s_hist_count].text = strndup(content, HISTORY_LEN);
    if (s_hist[s_hist_count].text) {
        s_hist_count++;
    }
}

static void load_locked(void)
{
    size_t len = 0;
    char *text = pp_data_read("ai_chat.json", &len);
    if (text == NULL) {
        return;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (root == NULL) {
        return;
    }
    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    int count = cJSON_GetArraySize(items);
    int first = count > PP_AI_HISTORY ? count - PP_AI_HISTORY : 0;
    for (int i = first; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(items, i);
        const cJSON *role = cJSON_GetObjectItemCaseSensitive(item, "role");
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
        if (cJSON_IsString(role) && cJSON_IsString(content)) {
            push_locked(role->valuestring, content->valuestring);
        }
    }
    cJSON_Delete(root);
}

esp_err_t pp_ai_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    load_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "loaded %d history messages", s_hist_count);
    return ESP_OK;
}

bool pp_ai_configured(void)
{
    return pp_config()->ai_key[0] != '\0';
}

void pp_ai_history_clear(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    for (int i = 0; i < s_hist_count; i++) {
        free(s_hist[i].text);
    }
    s_hist_count = 0;
    save_locked();
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

void pp_ai_history_push(const char *role, const char *content)
{
    if (role == NULL || content == NULL || content[0] == '\0') {
        return;
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    push_locked(role, content);
    save_locked();
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

int pp_ai_history_count(void)
{
    return s_hist_count;
}

const char *pp_ai_history_role(int index)
{
    if (index < 0 || index >= s_hist_count) {
        return NULL;
    }
    return s_hist[index].role;
}

const char *pp_ai_history_text(int index)
{
    if (index < 0 || index >= s_hist_count) {
        return NULL;
    }
    return s_hist[index].text;
}

static char *build_request(const char *message)
{
    pp_config_t *cfg = pp_config();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "model", cfg->ai_model[0] ? cfg->ai_model : PP_AI_MODEL_DEFAULT);
    cJSON_AddBoolToObject(root, "stream", false);
    cJSON_AddNumberToObject(root, "temperature", 0.6);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    if (cfg->ai_persona[0]) {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", cfg->ai_persona);
        cJSON_AddItemToArray(messages, sys);
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    for (int i = 0; i < s_hist_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "role", s_hist[i].role);
        cJSON_AddStringToObject(item, "content", s_hist[i].text);
        cJSON_AddItemToArray(messages, item);
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", message);
    cJSON_AddItemToArray(messages, user);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

bool pp_ai_last_error(void)
{
    return s_last_error;
}

char *pp_ai_send(const char *message)
{
    s_last_error = false;
    if (message == NULL || message[0] == '\0') {
        return NULL;
    }
    pp_config_t *cfg = pp_config();
    if (cfg->ai_key[0] == '\0') {
        s_last_error = true;
        return strdup("还没有配置 API Key。打开网页端「设置」，填入 DeepSeek 的 Key 即可。");
    }
    char *body = build_request(message);
    if (body == NULL) {
        return strdup("内存不足，无法发起请求。");
    }

    char url[192];
    size_t base_len = strlen(cfg->ai_base);
    if (base_len > 0 && cfg->ai_base[base_len - 1] == '/') {
        snprintf(url, sizeof(url), "%s%s", cfg->ai_base, "chat/completions");
    } else {
        snprintf(url, sizeof(url), "%s/%s", cfg->ai_base, "chat/completions");
    }

    buf_t resp = {0};
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 40000,
        .buffer_size = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .user_data = &resp,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(body);
        return strdup("创建 HTTP 客户端失败。");
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    char auth[224];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->ai_key);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "perform failed: %s", esp_err_to_name(err));
        s_last_error = true;
        free(resp.data);
        return strdup("网络请求失败：请检查 WiFi 和 API 地址。");
    }
    if (resp.data == NULL) {
        s_last_error = true;
        return strdup("没有收到回复。");
    }

    char *reply = NULL;
    cJSON *json = cJSON_Parse(resp.data);
    if (json != NULL) {
        cJSON *choices = cJSON_GetObjectItem(json, "choices");
        cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
        cJSON *msg = first ? cJSON_GetObjectItem(first, "message") : NULL;
        cJSON *content = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
        if (cJSON_IsString(content)) {
            reply = strndup(content->valuestring, MAX_REPLY);
        } else {
            cJSON *error = cJSON_GetObjectItem(json, "error");
            cJSON *emsg = error ? cJSON_GetObjectItem(error, "message") : NULL;
            if (cJSON_IsString(emsg)) {
                reply = strndup(emsg->valuestring, MAX_REPLY);
            }
        }
        cJSON_Delete(json);
    }
    if (reply == NULL) {
        s_last_error = true;
        if (status != 200) {
            char buf[160];
            snprintf(buf, sizeof(buf), "服务返回 HTTP %d：%.100s", status, resp.data);
            reply = strdup(buf);
        } else {
            reply = strdup("返回格式无法解析。");
        }
    }
    free(resp.data);
    (void)MAX_BODY;
    return reply;
}