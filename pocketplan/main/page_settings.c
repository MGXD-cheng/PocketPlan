/* Settings: reader typography, backlight, network and storage overview. */
#include <stdio.h>
#include <string.h>

#include "autosleep.h"
#include "board.h"
#include "clock.h"
#include "clockmode.h"
#include "config.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_system.h"
#include "focus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "net.h"
#include "pages.h"
#include "store.h"
#include "pp_fonts.h"
#include "ui.h"

#define C_BG 0xFFFFFF
#define C_CARD 0xFFFFFF
#define C_TEXT 0x000000
#define C_DIM 0x000000
#define C_ACCENT 0x000000
/* The panel is monochrome: a grey chip or a grey outline simply disappears,
 * so every control is drawn in pure black and white. */
#define C_CHIP 0x000000
#define C_EDGE 0x000000

static lv_obj_t *s_root;
static lv_obj_t *s_font_large;
static lv_obj_t *s_font_small;
static lv_obj_t *s_net_ssid;
static lv_obj_t *s_net_ip;
static lv_obj_t *s_net_ap;
static lv_obj_t *s_sd_free;
static lv_obj_t *s_lfs_free;
static lv_obj_t *s_ai_state;
static lv_obj_t *s_brightness;
static lv_obj_t *s_line_space;
static lv_obj_t *s_version;
static lv_obj_t *s_dark_chip;
static lv_obj_t *s_cache_chip;
static lv_obj_t *s_sleep_chip;
static lv_obj_t *s_sleep_min_chip;

static lv_obj_t *add_section(const char *title)
{
    lv_obj_t *card = lv_obj_create(s_root);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(card, LV_SIZE_CONTENT);

    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_hex(C_ACCENT), 0);
    return card;
}

static lv_obj_t *add_row(lv_obj_t *card, const char *key)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 40);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, key);
    lv_obj_set_style_text_color(label, lv_color_hex(C_TEXT), 0);

    lv_obj_t *value = lv_label_create(row);
    lv_label_set_text(value, "-");
    lv_obj_set_style_text_color(value, lv_color_hex(C_DIM), 0);
    lv_obj_set_flex_grow(value, 1);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    return value;
}

/* The selected size is shown by filling the chip black, so its label has to
 * turn white - black text on a black chip is invisible on a 1-bit panel. */
static void font_mark(int size)
{
    bool large = (size >= 20);
    lv_obj_set_style_bg_color(s_font_small, lv_color_hex(large ? 0xFFFFFF : C_CHIP), 0);
    lv_obj_set_style_bg_color(s_font_large, lv_color_hex(large ? C_CHIP : 0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_font_small, large ? 2 : 0, 0);
    lv_obj_set_style_border_width(s_font_large, large ? 0 : 2, 0);
    lv_obj_set_style_border_color(s_font_small, lv_color_hex(C_EDGE), 0);
    lv_obj_set_style_border_color(s_font_large, lv_color_hex(C_EDGE), 0);
    lv_obj_t *small = lv_obj_get_child(s_font_small, 0);
    lv_obj_t *large_lbl = lv_obj_get_child(s_font_large, 0);
    if (small != NULL) {
        lv_obj_set_style_text_color(small, lv_color_hex(large ? 0x000000 : 0xFFFFFF), 0);
    }
    if (large_lbl != NULL) {
        lv_obj_set_style_text_color(large_lbl, lv_color_hex(large ? 0xFFFFFF : 0x000000), 0);
    }
}

static void font_clicked(lv_event_t *e)
{
    int size = (int)(intptr_t)lv_event_get_user_data(e);
    pp_config_t *cfg = pp_config();
    cfg->font_size = (uint8_t)size;
    pp_config_save();
    pp_ui_toast(size >= 20 ? "字号已设为 20" : "字号已设为 16");
    font_mark(size);
}

static lv_obj_t *make_chip(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 96, 40);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(C_EDGE), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(C_ACCENT), 0);
    lv_obj_center(label);
    return btn;
}

static void brightness_changed(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int value = (int)lv_slider_get_value(slider);
    pp_board_backlight(value);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        pp_config()->brightness = (uint8_t)value;
        pp_config_save();
    }
}

static void line_space_changed(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int value = (int)lv_slider_get_value(slider);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        pp_config()->line_space = (uint8_t)value;
        pp_config_save();
        pp_ui_toast("行距已保存");
    }
}

static void chip_mark(lv_obj_t *btn, bool on)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(on ? C_CHIP : 0xFFFFFF), 0);
    lv_obj_set_style_border_width(btn, on ? 0 : 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(C_EDGE), 0);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label != NULL) {
        lv_obj_set_style_text_color(label, lv_color_hex(on ? 0xFFFFFF : C_ACCENT), 0);
    }
}

static void chip_set(lv_obj_t *btn, const char *text, bool on)
{
    chip_mark(btn, on);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label != NULL) {
        lv_label_set_text(label, text);
    }
}

/* 自动休眠: a plain on/off chip plus a chip that cycles through the idle
 * timeouts. Both live in the 屏幕 section, right under the brightness slider. */
static const uint8_t kSleepChoices[] = {1, 3, 5, 10, 30};

static void sleep_min_mark(void)
{
    char text[24];
    snprintf(text, sizeof(text), "每 %u 分", (unsigned)pp_config()->sleep_min);
    chip_set(s_sleep_min_chip, text, false);
}

static void sleep_clicked(lv_event_t *e)
{
    (void)e;
    pp_config_t *cfg = pp_config();
    cfg->auto_sleep = !cfg->auto_sleep;
    pp_config_save();
    chip_set(s_sleep_chip, cfg->auto_sleep ? "开" : "关", cfg->auto_sleep);
    if (!cfg->auto_sleep) {
        pp_autosleep_wake();
    }
    pp_ui_toast(cfg->auto_sleep ? "自动休眠已开启" : "自动休眠已关闭");
}

static void sleep_min_clicked(lv_event_t *e)
{
    (void)e;
    pp_config_t *cfg = pp_config();
    const int count = (int)(sizeof(kSleepChoices) / sizeof(kSleepChoices[0]));
    int index = 0;
    for (int i = 0; i < count; i++) {
        if (kSleepChoices[i] == cfg->sleep_min) {
            index = i;
            break;
        }
    }
    index = (index + 1) % count;
    cfg->sleep_min = kSleepChoices[index];
    pp_config_save();
    sleep_min_mark();
    pp_ui_toast("无操作后自动熄屏的时间已修改");
}

static void focus_clicked(lv_event_t *e)
{
    (void)e;
    pp_focus_open();
}

static void clockmode_clicked(lv_event_t *e)
{
    (void)e;
    pp_clockmode_open();
}

static void dark_clicked(lv_event_t *e)
{
    (void)e;
    pp_config_t *cfg = pp_config();
    cfg->focus_dark = !cfg->focus_dark;
    pp_config_save();
    chip_set(s_dark_chip, cfg->focus_dark ? "暗色" : "亮色", cfg->focus_dark);
    pp_ui_toast(cfg->focus_dark ? "专注模式已切到暗色" : "专注模式已切到亮色");
}

/* Moving the data files between SPIFFS and the SD card is file I/O, so run it
 * off the LVGL task and reboot when the switch has really taken effect. */
static void cache_task(void *arg)
{
    (void)arg;
    esp_err_t err = pp_store_migrate_data();
    if (err != ESP_OK) {
        ESP_LOGW("page_settings", "migrate failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    vTaskDelete(NULL);
}

static void cache_clicked(lv_event_t *e)
{
    (void)e;
    pp_config_t *cfg = pp_config();
    if (!cfg->cache_sd && !pp_board_sd_mounted()) {
        pp_ui_toast("SD 卡未挂载，无法缓存");
        return;
    }
    cfg->cache_sd = !cfg->cache_sd;
    pp_config_save();
    chip_set(s_cache_chip, cfg->cache_sd ? "开" : "关", cfg->cache_sd);
    pp_ui_toast(cfg->cache_sd ? "正在把数据搬到 SD 卡，稍后重启"
                              : "正在把数据搬回内部存储，稍后重启");
    if (xTaskCreate(cache_task, "pp_cache", 4096, NULL, 3, NULL) != pdPASS) {
        esp_restart();
    }
}

static void reboot_clicked(lv_event_t *e)
{
    (void)e;
    esp_restart();
}

static void refresh_values(void)
{
    pp_config_t *cfg = pp_config();
    char buf[96];

    snprintf(buf, sizeof(buf), "%s", pp_net_saved_ssid()[0] ? pp_net_saved_ssid() : "未配置");
    lv_label_set_text(s_net_ssid, buf);
    char ip[32];
    if (pp_net_sta_connected()) {
        pp_net_sta_ip(ip, sizeof(ip));
    } else {
        pp_net_ap_ip(ip, sizeof(ip));
    }
    snprintf(buf, sizeof(buf), "%s", ip);
    lv_label_set_text(s_net_ip, buf);
    snprintf(buf, sizeof(buf), "%s", pp_net_ap_ssid());
    lv_label_set_text(s_net_ap, buf);

    uint32_t total = 0, free_kb = 0;
    if (pp_board_sd_mounted()) {
        pp_board_sd_usage(&total, &free_kb);
        snprintf(buf, sizeof(buf), "剩 %lu MB", (unsigned long)(free_kb / 1024));
    } else {
        snprintf(buf, sizeof(buf), "未挂载");
    }
    lv_label_set_text(s_sd_free, buf);

    if (pp_store_ready()) {
        snprintf(buf, sizeof(buf), "剩 %lu KB", (unsigned long)pp_store_free_kb());
    } else {
        snprintf(buf, sizeof(buf), "未就绪");
    }
    lv_label_set_text(s_lfs_free, buf);

    lv_label_set_text(s_ai_state, cfg->ai_key[0] ? "已配置" : "未配置（网页端填写）");
}

void pp_settings_refresh(void)
{
    if (s_root == NULL) {
        return;
    }
    pp_config_t *cfg = pp_config();
    lvgl_port_lock(0);
    refresh_values();
    lv_slider_set_value(s_brightness, cfg->brightness, LV_ANIM_OFF);
    lv_slider_set_value(s_line_space, cfg->line_space, LV_ANIM_OFF);
    font_mark(cfg->font_size);
    chip_set(s_dark_chip, cfg->focus_dark ? "暗色" : "亮色", cfg->focus_dark);
    chip_set(s_cache_chip, cfg->cache_sd ? "开" : "关", cfg->cache_sd);
    chip_set(s_sleep_chip, cfg->auto_sleep ? "开" : "关", cfg->auto_sleep);
    sleep_min_mark();
    lvgl_port_unlock();
}

void pp_settings_create(lv_obj_t *parent)
{
    s_root = parent;
    lv_obj_set_style_bg_color(s_root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(s_root, 12, 0);
    lv_obj_set_style_pad_row(s_root, 10, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_root, LV_DIR_VER);

    /* reading */
    lv_obj_t *card = add_section("阅读");
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *fl = lv_label_create(row);
    lv_label_set_text(fl, "字号");
    lv_obj_set_style_text_color(fl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_flex_grow(fl, 1);
    s_font_small = make_chip(row, "16", font_clicked, (void *)(intptr_t)16);
    s_font_large = make_chip(row, "20", font_clicked, (void *)(intptr_t)20);

    row = lv_obj_create(card);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ll = lv_label_create(row);
    lv_label_set_text(ll, "行距");
    lv_obj_set_style_text_color(ll, lv_color_hex(C_TEXT), 0);
    lv_obj_set_width(ll, 60);
    s_line_space = lv_slider_create(row);
    lv_obj_set_flex_grow(s_line_space, 1);
    lv_slider_set_range(s_line_space, 0, 16);
    lv_slider_set_value(s_line_space, pp_config()->line_space, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_line_space, line_space_changed, LV_EVENT_ALL, NULL);

    /* display */
    card = add_section("屏幕");
    row = lv_obj_create(card);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bl = lv_label_create(row);
    lv_label_set_text(bl, "亮度");
    lv_obj_set_style_text_color(bl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_width(bl, 60);
    s_brightness = lv_slider_create(row);
    lv_obj_set_flex_grow(s_brightness, 1);
    lv_slider_set_range(s_brightness, 10, 100);
    lv_slider_set_value(s_brightness, pp_config()->brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_brightness, brightness_changed, LV_EVENT_ALL, NULL);

    row = lv_obj_create(card);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *sl = lv_label_create(row);
    lv_label_set_text(sl, "自动休眠");
    lv_obj_set_style_text_color(sl, lv_color_hex(C_TEXT), 0);
    s_sleep_chip = make_chip(row, "开", sleep_clicked, NULL);
    lv_obj_set_width(s_sleep_chip, 80);

    row = lv_obj_create(card);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *sml = lv_label_create(row);
    lv_label_set_text(sml, "休眠时间");
    lv_obj_set_style_text_color(sml, lv_color_hex(C_TEXT), 0);
    s_sleep_min_chip = make_chip(row, "每 5 分", sleep_min_clicked, NULL);
    lv_obj_set_width(s_sleep_min_chip, 104);

    lv_obj_t *sleep_hint = lv_label_create(card);
    lv_label_set_text(sleep_hint, "熄屏后轻触屏幕唤醒；闹钟响铃和时钟模式不会熄屏");
    lv_obj_set_style_text_color(sleep_hint, lv_color_hex(C_DIM), 0);
    lv_obj_set_width(sleep_hint, lv_pct(100));
    lv_label_set_long_mode(sleep_hint, LV_LABEL_LONG_WRAP);

    /* focus */
    card = add_section("专注");
    row = lv_obj_create(card);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 44);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *focus_btn = make_chip(row, "进入专注模式", focus_clicked, NULL);
    lv_obj_set_width(focus_btn, 160);
    lv_obj_t *clock_btn = make_chip(row, "时钟模式", clockmode_clicked, NULL);
    lv_obj_set_width(clock_btn, 110);

    row = lv_obj_create(card);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *dl = lv_label_create(row);
    lv_label_set_text(dl, "暗色时钟");
    lv_obj_set_style_text_color(dl, lv_color_hex(C_TEXT), 0);
    s_dark_chip = make_chip(row, "亮色", dark_clicked, NULL);
    lv_obj_set_width(s_dark_chip, 80);

    lv_obj_t *focus_hint = lv_label_create(card);
    lv_label_set_text(focus_hint, "专注：横屏全屏时钟 + 已专注计时，轻触退出；时钟：横屏，只有时间、下一个闹钟和倒数日，轻触 3 下退出");
    lv_obj_set_style_text_color(focus_hint, lv_color_hex(C_DIM), 0);
    lv_obj_set_width(focus_hint, lv_pct(100));
    lv_label_set_long_mode(focus_hint, LV_LABEL_LONG_WRAP);

    /* network */
    card = add_section("网络");
    s_net_ssid = add_row(card, "WiFi");
    s_net_ip = add_row(card, "IP");
    s_net_ap = add_row(card, "热点");
    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, "长按 BOOT 键 3 秒可清除 WiFi 并重启");
    lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), 0);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    /* storage */
    card = add_section("存储");
    s_sd_free = add_row(card, "SD 卡");
    s_lfs_free = add_row(card, "内部");
    row = lv_obj_create(card);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *cl = lv_label_create(row);
    lv_label_set_text(cl, "数据存 SD 卡");
    lv_obj_set_style_text_color(cl, lv_color_hex(C_TEXT), 0);
    s_cache_chip = make_chip(row, "关", cache_clicked, NULL);
    lv_obj_set_width(s_cache_chip, 80);

    lv_obj_t *cache_hint = lv_label_create(card);
    lv_label_set_text(cache_hint, "聊天、计划、倒数日、闹钟改存到 SD 卡；切换后自动重启");
    lv_obj_set_style_text_color(cache_hint, lv_color_hex(C_DIM), 0);
    lv_obj_set_width(cache_hint, lv_pct(100));
    lv_label_set_long_mode(cache_hint, LV_LABEL_LONG_WRAP);

    /* AI */
    card = add_section("AI");
    s_ai_state = add_row(card, "API Key");
    lv_obj_t *ai_hint = lv_label_create(card);
    lv_label_set_text(ai_hint, "在网页端「设置」里填写 DeepSeek 兼容的 Key");
    lv_obj_set_style_text_color(ai_hint, lv_color_hex(C_DIM), 0);
    lv_obj_set_width(ai_hint, lv_pct(100));
    lv_label_set_long_mode(ai_hint, LV_LABEL_LONG_WRAP);

    /* about */
    card = add_section("关于");
    s_version = add_row(card, "固件");
    lv_label_set_text(s_version, "PocketPlan 1.0");
    lv_obj_t *reboot = make_chip(card, "重启设备", reboot_clicked, NULL);
    lv_obj_set_width(reboot, 140);

    pp_settings_refresh();
}
