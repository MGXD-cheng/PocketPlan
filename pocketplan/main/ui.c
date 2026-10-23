/*
 * UI shell: a compact header with the clock and network status, the tab
 * content area and a five button bottom navigation bar. Every page module
 * only has to build its widgets under the object it is handed.
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "clock.h"
#include "config.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "lvgl.h"
#include "pages.h"
#include "plans.h"
#include "countdowns.h"
#include "pp_fonts.h"

static const char *TAG = "ui";

#define HEADER_H 68
#define NAV_H 68
#define TAB_COUNT 6

static lv_obj_t *s_content;
static lv_obj_t *s_status;
static lv_obj_t *s_clock;
static lv_obj_t *s_title;
static lv_obj_t *s_nav;
static lv_obj_t *s_toast;
static lv_obj_t *s_nav_buttons[TAB_COUNT];
static lv_obj_t *s_page_roots[TAB_COUNT];

static int s_active = 0;
static char s_last_day[11];

typedef struct {
    const char *icon;
    const char *label;
    void (*create)(lv_obj_t *parent);
    void (*refresh)(void);
} tab_def_t;

static const tab_def_t s_tabs[TAB_COUNT] = {
    {LV_SYMBOL_LIST, "计划", pp_home_create, pp_home_refresh},
    {LV_SYMBOL_BELL, "倒数", pp_countdown_create, pp_countdown_refresh},
    {LV_SYMBOL_DIRECTORY, "文件", pp_files_create, pp_files_refresh},
    {LV_SYMBOL_AUDIO, "音乐", pp_music_create, pp_music_refresh},
    {LV_SYMBOL_EDIT, "AI", pp_ai_create, pp_ai_refresh},
    {LV_SYMBOL_SETTINGS, "设置", pp_settings_create, pp_settings_refresh},
};

static void toast_timer_cb(lv_timer_t *timer)
{
    lv_obj_t *label = (lv_obj_t *)lv_timer_get_user_data(timer);
    if (label) {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_delete(timer);
}

void pp_ui_toast(const char *text)
{
    if (s_toast == NULL) {
        return;
    }
    lvgl_port_lock(0);
    lv_label_set_text(s_toast, text);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    lv_timer_t *t = lv_timer_create(toast_timer_cb, 2600, s_toast);
    lv_timer_set_repeat_count(t, 1);
    lvgl_port_unlock();
}

static void update_clock(void)
{
    char buf[32];
    pp_clock_format(buf, sizeof(buf), "%H:%M");
    if (s_clock) {
        lv_label_set_text(s_clock, buf);
    }
    char day[11];
    pp_clock_today(day, sizeof(day));
    if (strcmp(day, s_last_day) != 0) {
        snprintf(s_last_day, sizeof(s_last_day), "%s", day);
        pp_plans_refresh();
        pp_countdown_refresh();
        pp_home_tick();
    }
}

static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    update_clock();
    pp_home_tick();
}

static void tab_clicked(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    pp_ui_show_tab(index);
}

void pp_ui_show_tab(int index)
{
    if (index < 0 || index >= TAB_COUNT) {
        return;
    }
    lvgl_port_lock(0);
    if (s_page_roots[index] == NULL) {
        s_page_roots[index] = lv_obj_create(s_content);
        lv_obj_set_size(s_page_roots[index], lv_pct(100), lv_pct(100));
        lv_obj_set_style_radius(s_page_roots[index], 0, 0);
        lv_obj_set_style_border_width(s_page_roots[index], 0, 0);
        lv_obj_set_style_pad_all(s_page_roots[index], 0, 0);
        lv_obj_set_style_bg_color(s_page_roots[index], lv_color_hex(0xFFFFFF), 0);
        s_tabs[index].create(s_page_roots[index]);
    }
    for (int i = 0; i < TAB_COUNT; i++) {
        if (s_page_roots[i] == NULL) {
            continue;
        }
        if (i == index) {
            lv_obj_clear_flag(s_page_roots[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_page_roots[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_bg_color(s_nav_buttons[i],
                                  lv_color_hex(i == index ? 0xE4E4E4 : 0xFFFFFF), 0);
        lv_obj_t *label = lv_obj_get_child(s_nav_buttons[i], 1);
        if (label) {
            lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);
        }
        lv_obj_t *icon = lv_obj_get_child(s_nav_buttons[i], 0);
        if (icon) {
            lv_obj_set_style_text_color(icon, lv_color_hex(0x000000), 0);
        }
    }
    s_active = index;
    lvgl_port_unlock();
    s_tabs[index].refresh();
}

int pp_ui_active_tab(void)
{
    return s_active;
}

void pp_ui_refresh_files(void)
{
    /* called from the background status task */
    if (s_page_roots[2] != NULL && s_active == 2) {
        pp_files_refresh();
    }
}

void pp_ui_set_status(const char *text)
{
    if (s_status == NULL) {
        return;
    }
    lvgl_port_lock(0);
    lv_label_set_text(s_status, text);
    lvgl_port_unlock();
}

void pp_ui_set_subtitle(const char *text)
{
    pp_ui_set_status(text);
}

void pp_ui_init(void)
{
    lvgl_port_lock(0);

    /* One monochrome font for the whole UI. The stock Montserrat default has
     * no CJK glyphs at all and the bundled Source Han subsets only carry a few
     * thousand hand-picked characters, which is why most Chinese text came out
     * as empty boxes. pp_font_16 is a 1bpp SimHei: pure black on white, with no
     * anti-aliased grey fringe. */
    lv_display_t *disp = pp_board_display();
    if (disp != NULL) {
        lv_theme_t *theme = lv_theme_default_init(disp, lv_color_hex(0x000000),
                                                  lv_color_hex(0x000000), false,
                                                  &pp_font_16);
        lv_display_set_theme(disp, theme);
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ------------------------------- header ------------------------------ */
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, lv_pct(100), HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(0xC8C8C8), 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_left(header, 16, 0);
    lv_obj_set_style_pad_right(header, 16, 0);
    lv_obj_set_style_pad_top(header, 8, 0);
    lv_obj_set_style_pad_bottom(header, 6, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *row = lv_obj_create(header);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 24);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(row);
    lv_label_set_text(s_title, "PocketPlan");
    lv_obj_set_style_text_color(s_title, lv_color_hex(0x000000), 0);

    s_clock = lv_label_create(row);
    lv_label_set_text(s_clock, "--:--");
    lv_obj_set_style_text_color(s_clock, lv_color_hex(0x000000), 0);

    s_status = lv_label_create(header);
    lv_label_set_text(s_status, "启动中...");
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x000000), 0);
    lv_obj_set_width(s_status, lv_pct(100));
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);

    /* ------------------------------- content ----------------------------- */
    s_content = lv_obj_create(scr);
    lv_obj_set_size(s_content, lv_pct(100), PP_LCD_V_RES - HEADER_H - NAV_H);
    lv_obj_align(s_content, LV_ALIGN_TOP_MID, 0, HEADER_H);
    lv_obj_set_style_radius(s_content, 0, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_set_style_bg_color(s_content, lv_color_hex(0xFFFFFF), 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    /* ----------------------------- bottom nav ---------------------------- */
    s_nav = lv_obj_create(scr);
    lv_obj_set_size(s_nav, lv_pct(100), NAV_H);
    lv_obj_align(s_nav, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(s_nav, 0, 0);
    lv_obj_set_style_border_width(s_nav, 1, 0);
    lv_obj_set_style_border_side(s_nav, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(s_nav, lv_color_hex(0xC8C8C8), 0);
    lv_obj_set_style_bg_color(s_nav, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(s_nav, 4, 0);
    lv_obj_set_style_pad_column(s_nav, 4, 0);
    lv_obj_set_flex_flow(s_nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_nav, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_nav, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(s_nav);
        lv_obj_set_size(btn, 74, 58);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 2, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(btn, tab_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, s_tabs[i].icon);
        lv_obj_set_style_text_color(icon, lv_color_hex(0x000000), 0);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, s_tabs[i].label);
        lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);

        s_nav_buttons[i] = btn;
        s_page_roots[i] = NULL;
    }

    /* -------------------------------- toast ------------------------------ */
    s_toast = lv_label_create(scr);
    lv_label_set_text(s_toast, "");
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0xE4E4E4), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_color(s_toast, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(s_toast, 10, 0);
    lv_obj_set_style_radius(s_toast, 8, 0);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, HEADER_H + 12);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    /* Still inside the LVGL lock: the display task is already running and
     * touching widgets from two contexts corrupts the invalidation areas. */
    pp_clock_today(s_last_day, sizeof(s_last_day));
    update_clock();

    lv_timer_t *timer = lv_timer_create(clock_timer_cb, 15000, NULL);
    if (timer) {
        lv_timer_set_repeat_count(timer, -1);
    }

    pp_ui_show_tab(0);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "shell ready");
}