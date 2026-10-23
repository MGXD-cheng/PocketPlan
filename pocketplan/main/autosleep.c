/*
 * Auto sleep: cut the backlight after N minutes without a touch.
 *
 * LVGL keeps an inactivity clock per display: every indev read stamps
 * disp->last_activity_time and lv_display_get_inactive_time() reports its age.
 * That gives both halves of the feature for free, with no touch driver changes:
 *   - idle for long enough -> backlight 0. The panel keeps refreshing and the
 *     indev keeps reading, so the very first tap after a sleep is still
 *     delivered (and does not have to be repeated),
 *   - inactivity age drops back to ~0 -> somebody touched the screen, so turn
 *     the light back on.
 * Nothing is suspended: Wi-Fi, the alarms and the web server keep running.
 */
#include "autosleep.h"

#include "alarms.h"
#include "board.h"
#include "clockmode.h"
#include "config.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "autosleep";

#define POLL_MS 500
/* A touch resets the inactivity clock to ~0, so anything below this while
 * asleep means "the user just touched the screen". */
#define WAKE_WINDOW_MS 700

static volatile bool s_asleep;
static int s_saved_brightness;

static void sleep_now(uint32_t idle)
{
    s_saved_brightness = pp_config()->brightness;
    pp_board_backlight(0);
    s_asleep = true;
    ESP_LOGI(TAG, "backlight off after %u s idle", (unsigned)(idle / 1000));
}

void pp_autosleep_wake(void)
{
    if (!s_asleep) {
        return;
    }
    int level = s_saved_brightness > 0 ? s_saved_brightness : pp_config()->brightness;
    pp_board_backlight(level);
    s_asleep = false;
    ESP_LOGI(TAG, "backlight on");
}

bool pp_autosleep_active(void)
{
    return s_asleep;
}

/* LVGL may only be touched from its own task, so read the inactivity clock
 * under the port lock; a short timeout keeps the poll from stalling if the UI
 * is in the middle of a long redraw. */
static uint32_t idle_ms(void)
{
    if (!lvgl_port_lock(100)) {
        return 0;
    }
    uint32_t idle = lv_display_get_inactive_time(NULL);
    lvgl_port_unlock();
    return idle;
}

static void sleep_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        pp_config_t *cfg = pp_config();
        uint32_t idle = idle_ms();

        /* A ringing alarm has to stay readable, and the wall clock page is an
         * always-on display by design, so neither counts as idle time. */
        if (pp_alarms_ringing() || pp_clockmode_active()) {
            pp_autosleep_wake();
            continue;
        }
        if (!cfg->auto_sleep) {
            pp_autosleep_wake();
            continue;
        }
        uint32_t limit = (uint32_t)cfg->sleep_min * 60u * 1000u;
        if (limit == 0) {
            limit = 60u * 1000u;
        }
        if (!s_asleep) {
            if (idle >= limit) {
                sleep_now(idle);
            }
        } else if (idle < WAKE_WINDOW_MS) {
            pp_autosleep_wake();
        }
    }
}

void pp_autosleep_init(void)
{
    /* The task only polls a counter, so it is cheap; it is created after the UI
     * exists because the inactivity clock lives on the display. */
    if (xTaskCreate(sleep_task, "pp_sleep", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the auto sleep task");
    }
}
