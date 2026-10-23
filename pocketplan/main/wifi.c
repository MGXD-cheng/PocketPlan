/*
 * Wi-Fi bring-up: always starts a SoftAP so the device is reachable at
 * 192.168.4.1 for provisioning, and additionally joins the stored network when
 * credentials exist.  This mirrors the "connect to the hotspot, open the page"
 * flow the user asked for.
 */
#include "net.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "net";

#define NVS_NS "pp"

static char s_ap_ssid[33];
static char s_saved_ssid[33];
static char s_ap_ip[16] = "192.168.4.1";
static char s_sta_ip[16] = "0.0.0.0";
static volatile bool s_sta_connected;
static volatile bool s_reconnect_pending;

/* Reconnect from a task instead of blocking the system event loop, and back off
 * exponentially: a router that is switched off for the night must not turn into
 * a connect attempt every three seconds (each one briefly costs internal RAM and
 * floods the log). Reset on every successful association. */
#define PP_RECONNECT_MIN_MS 3000
#define PP_RECONNECT_MAX_MS 60000
static volatile int s_backoff_ms = PP_RECONNECT_MIN_MS;

static void reconnect_task(void *arg)
{
    int delay_ms = (int)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    s_reconnect_pending = false;
    if (!s_sta_connected) {
        esp_wifi_connect();
    }
    vTaskDelete(NULL);
}

static void load_credentials(void)
{
    nvs_handle_t h;
    s_saved_ssid[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_saved_ssid);
    if (nvs_get_str(h, "ssid", s_saved_ssid, &len) != ESP_OK) {
        s_saved_ssid[0] = '\0';
    }
    nvs_close(h);
}

const char *pp_net_saved_ssid(void)
{
    return s_saved_ssid;
}

esp_err_t pp_net_set_credentials(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(s_saved_ssid, sizeof(s_saved_ssid), "%s", ssid);

    wifi_config_t cfg = {0};
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", password ? password : "");
    cfg.sta.threshold.authmode = (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_LOGI(TAG, "joining '%s'", ssid);

    /* STA_START already fires esp_wifi_connect() from the NVS copy of these very
     * credentials, so the station is usually mid-handshake by the time we get
     * here - and a connecting station rejects esp_wifi_set_config() with
     * ESP_ERR_WIFI_STATE (that was the "sta is connecting, cannot set config"
     * pair of errors on every boot). If the driver already holds exactly what we
     * want, do nothing but make sure a connect is in flight. */
    wifi_config_t current = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &current) == ESP_OK &&
        strcmp((const char *)current.sta.ssid, ssid) == 0 &&
        strcmp((const char *)current.sta.password, (password ? password : "")) == 0) {
        /* Already connecting with these very credentials; calling connect() again
         * would only log "sta is connecting, return error". The disconnect path
         * and the reconnect task below keep the link alive. */
        return ESP_OK;
    }

    /* Different credentials: stop the radio, hand over the config, start again.
     * The driver only accepts a new STA config while idle, and stopping is the
     * one call that reliably gets it there (disconnect() bounces right off a
     * connect in progress). STA_START reconnects with the new config. */
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(50));
    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set STA config failed: %s", esp_err_to_name(err));
    }
    esp_wifi_start();
    if (err == ESP_OK) {
        esp_wifi_connect();
    }
    return err;
}

static void apply_saved_credentials(void)
{
    nvs_handle_t h;
    char pass[65] = {0};
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(pass);
        if (nvs_get_str(h, "pass", pass, &len) != ESP_OK) {
            pass[0] = '\0';
        }
        nvs_close(h);
    }
    if (s_saved_ssid[0] == '\0') {
        ESP_LOGW(TAG, "no stored credentials, hotspot only");
        return;
    }
    pp_net_set_credentials(s_saved_ssid, pass);
}

bool pp_net_sta_connected(void)
{
    return s_sta_connected;
}

const char *pp_net_ap_ssid(void)
{
    return s_ap_ssid;
}

void pp_net_ap_ip(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s", s_ap_ip);
}

void pp_net_sta_ip(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s", s_sta_ip);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_saved_ssid[0]) {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            s_sta_connected = false;
            snprintf(s_sta_ip, sizeof(s_sta_ip), "0.0.0.0");
            if (s_saved_ssid[0] && !s_reconnect_pending) {
                s_reconnect_pending = true;
                int delay_ms = s_backoff_ms;
                if (s_backoff_ms < PP_RECONNECT_MAX_MS) {
                    s_backoff_ms *= 2;
                    if (s_backoff_ms > PP_RECONNECT_MAX_MS) {
                        s_backoff_ms = PP_RECONNECT_MAX_MS;
                    }
                }
                ESP_LOGW(TAG, "station disconnected, retrying in %d s", delay_ms / 1000);
                xTaskCreate(reconnect_task, "pp_reconnect", 2560,
                            (void *)(intptr_t)delay_ms, 4, NULL);
            }
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "a client joined the hotspot");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_sta_connected = true;
        s_backoff_ms = PP_RECONNECT_MIN_MS;
        ESP_LOGI(TAG, "station got ip %s", s_sta_ip);
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
}

esp_err_t pp_net_start(void)
{
    ESP_RETURN_ON_FALSE(esp_netif_init() == ESP_OK, ESP_FAIL, TAG, "netif init");
    ESP_RETURN_ON_FALSE(esp_event_loop_create_default() == ESP_OK, ESP_FAIL, TAG, "event loop");

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL));

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "PocketPlan-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_cfg = {0};
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s", s_ap_ssid);
    ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    snprintf((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), "%s", PP_AP_PASSWORD);
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    /* Do not preload a STA config here: esp_wifi_start() would immediately begin
     * connecting with it, and the credential hand-over that follows could no
     * longer touch the station config. apply_saved_credentials() sets the
     * credentials right after start instead. */
    load_credentials();

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "hotspot '%s' up at %s", s_ap_ssid, s_ap_ip);
    if (s_saved_ssid[0]) {
        apply_saved_credentials();
    }
    return ESP_OK;
}
