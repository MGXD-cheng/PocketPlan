/*
 * PocketPlan boot.
 *
 * Boot order matters: the ST7701S is driven over a bit-banged 3-wire SPI that
 * borrows GPIO1/GPIO2, which are exactly the SDMMC CMD/CLK lines. The panel is
 * therefore brought up first (and never touched again), after which the SD
 * driver is free to take those pins over.
 */
#include <stdio.h>
#include <string.h>

#include "ai.h"
#include "alarms.h"
#include "autosleep.h"
#include "board.h"
#include "clock.h"
#include "config.h"
#include "countdowns.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pages.h"
#include "plans.h"
#include "shot.h"
#include "store.h"
#include "ui.h"
#include "webdav.h"

static const char *TAG = "pocketplan";

/* PCF85063 backup battery keeps the clock across a power cycle, so all the
 * periodic task needs to do is keep the header fresh and re-scan the SD card
 * while the file page is on screen. */
/* Below these the board stops working in strange ways: SDMMC DMA buffers and the
 * Wi-Fi stack both need large contiguous internal blocks, so a slow leak shows
 * up first as "sdmmc_read_blocks failed" rather than as an allocation error. */
#define PP_HEAP_WARN_BYTES (32 * 1024)
#define PP_BLOCK_WARN_BYTES (16 * 1024)

/* Cheap health line once a minute, so a leak is visible in the log long before
 * it turns into a random failure. */
static void status_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t largest =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "heap: internal %u B (largest %u B), psram %u KB, lfs %u KB free",
                 (unsigned)internal, (unsigned)largest, (unsigned)(psram / 1024),
                 (unsigned)pp_store_free_kb());
        if (internal < PP_HEAP_WARN_BYTES || largest < PP_BLOCK_WARN_BYTES) {
            ESP_LOGW(TAG, "internal heap low, largest block %u B: free the reader cache",
                     (unsigned)largest);
        }
    }
}

/* Holding BOOT wipes the saved Wi-Fi credentials: the single most useful
 * recovery action when the device is carried to a new network. */
static void button_task(void *arg)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << 0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    int held = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(0) == 0) {
            held++;
            if (held == 30) {
                pp_ui_toast("Wi-Fi 凭据已清除，重启中");
                nvs_handle_t h;
                if (nvs_open("pp", NVS_READWRITE, &h) == ESP_OK) {
                    nvs_erase_all(h);
                    nvs_commit(h);
                    nvs_close(h);
                }
                ESP_LOGW(TAG, "wifi credentials erased by BOOT hold");
                vTaskDelay(pdMS_TO_TICKS(600));
                esp_restart();
            }
        } else {
            held = 0;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "PocketPlan booting (built %s %s)", __DATE__, __TIME__);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    if (pp_board_init() != ESP_OK) {
        ESP_LOGE(TAG, "board init failed");
    } else {
        ESP_LOGI(TAG, "display ready");
    }

    if (pp_board_sd_init() == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted");
    } else {
        ESP_LOGW(TAG, "SD card not available, running without storage");
    }

    /* Internal filesystem first: config and plans live there. */
    if (pp_store_init() != ESP_OK) {
        ESP_LOGW(TAG, "internal storage unavailable, settings will not persist");
    }
    pp_config_load();
    pp_clock_init();
    pp_plans_init();
    pp_countdowns_init();
    pp_alarms_init();

    /* Bring the radio up before the UI claims the internal heap: Wi-Fi needs a
     * large contiguous DMA-capable block and cannot fall back to PSRAM. */
    if (pp_net_start() != ESP_OK) {
        ESP_LOGE(TAG, "network init failed");
    } else {
        char ap_ip[16];
        pp_net_ap_ip(ap_ip, sizeof(ap_ip));
        ESP_LOGI(TAG, "AP '%s' at http://%s/", pp_net_ap_ssid(), ap_ip);
    }
    pp_clock_start_sntp();

    pp_ai_init();

    pp_ui_init();
    pp_reader_ui_init();
    /* Needs the display, because it reads LVGL's inactivity clock. */
    pp_autosleep_init();

    uint32_t total_kb = 0, free_kb = 0;
    pp_board_sd_usage(&total_kb, &free_kb);
    if (pp_board_sd_mounted()) {
        char sub[96];
        snprintf(sub, sizeof(sub), "microSD %.1f GB free of %.1f GB", free_kb / 1024.0 / 1024.0,
                 total_kb / 1024.0 / 1024.0);
        pp_ui_set_subtitle(sub);
    } else {
        pp_ui_set_subtitle("microSD not mounted - insert a FAT32/exFAT card");
    }

    esp_err_t web_err = pp_webdav_start();
    if (web_err != ESP_OK) {
        ESP_LOGE(TAG, "web server failed: %s", esp_err_to_name(web_err));
        pp_ui_set_status("Web server failed to start");
    }

    xTaskCreate(status_task, "pp_status", 4096, NULL, 3, NULL);
    xTaskCreate(button_task, "pp_button", 3072, NULL, 3, NULL);
    /* Serial side channel: type "shot" on the UART to dump the screen. */
    pp_shot_start_console();
    pp_shot_start_tour();

    ESP_LOGI(TAG, "ready");
}