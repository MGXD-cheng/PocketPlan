/*
 * Focus mode: a landscape clock. The panel itself is fixed in portrait and the
 * RGB frame buffer is drawn in direct mode, so LVGL's display rotation (which
 * needs matrix rotation for direct mode) is not available here. Instead the
 * stage is laid out in landscape coordinates and every label is turned 90
 * degrees with a style transform: at exactly 90 degrees the sampling lands on
 * pixel centres, so the 1bpp text stays crisp.
 *
 * The page honours two settings: focus_dark (black background, white text) and
 * focus_cd (id of the countdown shown under the clock).
 */
#include "focus.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "clock.h"
#include "config.h"
#include "countdowns.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "pp_fonts.h"

#define LAND_W PP_LCD_V_RES /* 640: stage width once turned sideways */
#define LAND_H PP_LCD_H_RES /* 480: stage height */
#define ROT 900             /* clockwise, 0.1 degree units */

#define C_LIGHT_BG 0xFFFFFF
#define C_LIGHT_FG 0x000000
#define C_DARK_BG  0x000000
#define C_DARK_FG  0xFFFFFF

static lv_obj_t *s_overlay;
static lv_obj_t *s_clock;
static lv_obj_t *s_date;
static lv_obj_t *s_elapsed;
static lv_obj_t *s_cd;
static lv_timer_t *s_timer;
static time_t s_start;
static lv_color_t s_fg;

/* Landscape (u, v) inside the 640x480 stage -> offset from the portrait screen
 * centre. Turning the stage 90 degrees clockwise sends stage-right downwards
 * and stage-down to the left. */
static void place(lv_obj_t *obj, int32_t u, int32_t v)
{
    int32_t ox = -(v - LAND_H / 2);
    int32_t oy = (u - LAND_W / 2);
    lv_obj_align(obj, LV_ALIGN_CENTER, ox, oy);
    lv_obj_set_style_transform_rotation(obj, ROT, 0);
    lv_obj_set_style_transform_pivot_x(obj, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(obj, LV_PCT(50), 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, const char *text,
                            int32_t u, int32_t v)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, s_fg, 0);
    place(label, u, v);
    return label;
}

static void set_if_changed(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

/* "<title> · 还有 12 天" for the countdown picked in the web settings. */
static void update_countdown(void)
{
    const pp_config_t *cfg = pp_config();
    const pp_countdown_t *found = NULL;
    if (cfg->focus_cd[0] != '\0') {
        for (int i = 0; i < pp_countdowns_count(); i++) {
            const pp_countdown_t *item = pp_countdowns_get(i);
            if (item != NULL && strcmp(item->id, cfg->focus_cd) == 0) {
                found = item;
                break;
            }
        }
    }
    if (found == NULL) {
        lv_obj_add_flag(s_cd, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char buf[160];
    if (found->days_left == 0) {
        snprintf(buf, sizeof(buf), "%s · 就是今天", found->title);
    } else if (found->days_left == 1) {
        snprintf(buf, sizeof(buf), "%s · 明天", found->title);
    } else if (found->days_left > 0) {
        snprintf(buf, sizeof(buf), "%s · 还有 %d 天", found->title, found->days_left);
    } else {
        snprintf(buf, sizeof(buf), "%s · 已过 %d 天", found->title, -found->days_left);
    }
    set_if_changed(s_cd, buf);
    lv_obj_clear_flag(s_cd, LV_OBJ_FLAG_HIDDEN);
}

static void update(void)
{
    char buf[32];
    pp_clock_format(buf, sizeof(buf), "%H:%M");
    set_if_changed(s_clock, buf);

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    static const char *const k_weekdays[] = {"周日", "周一", "周二", "周三",
                                             "周四", "周五", "周六"};
    snprintf(buf, sizeof(buf), "%d月%d日 %s", tm_now.tm_mon + 1, tm_now.tm_mday,
             k_weekdays[tm_now.tm_wday % 7]);
    set_if_changed(s_date, buf);

    long secs = (long)(now - s_start);
    if (secs < 0) {
        secs = 0;
    }
    if (secs >= 3600) {
        snprintf(buf, sizeof(buf), "已专注 %ld:%02ld:%02ld", secs / 3600, (secs / 60) % 60,
                 secs % 60);
    } else {
        snprintf(buf, sizeof(buf), "已专注 %02ld:%02ld", (secs / 60) % 60, secs % 60);
    }
    set_if_changed(s_elapsed, buf);

    update_countdown();
}

static void timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update();
}

static void close_async(void *unused)
{
    (void)unused;
    pp_focus_close();
}

static void overlay_clicked(lv_event_t *e)
{
    (void)e;
    /* Deleting the object from inside its own event handler is not allowed. */
    lv_async_call(close_async, NULL);
}

bool pp_focus_active(void)
{
    return s_overlay != NULL;
}

void pp_focus_open(void)
{
    if (s_overlay != NULL) {
        return;
    }
    const pp_config_t *cfg = pp_config();
    const bool dark = cfg->focus_dark;
    s_fg = lv_color_hex(dark ? C_DARK_FG : C_LIGHT_FG);

    s_start = time(NULL);
    lvgl_port_lock(0);
    s_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_center(s_overlay);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(dark ? C_DARK_BG : C_LIGHT_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_overlay, overlay_clicked, LV_EVENT_CLICKED, NULL);

    s_clock = make_label(s_overlay, &pp_font_96, "--:--", LAND_W / 2, 145);
    s_cd = make_label(s_overlay, &pp_font_20, "", LAND_W / 2, 208);
    s_date = make_label(s_overlay, &pp_font_20, "", LAND_W / 2, 258);
    s_elapsed = make_label(s_overlay, &pp_font_20, "", LAND_W / 2, 308);
    make_label(s_overlay, &pp_font_16, "轻触屏幕退出", LAND_W / 2, 378);
    make_label(s_overlay, &pp_font_16, "专注", 56, 44);

    update();
    s_timer = lv_timer_create(timer_cb, 1000, NULL);
    if (s_timer) {
        lv_timer_set_repeat_count(s_timer, -1);
    }
    lvgl_port_unlock();
}

void pp_focus_close(void)
{
    if (s_overlay == NULL) {
        return;
    }
    lvgl_port_lock(0);
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    lv_obj_delete(s_overlay);
    s_overlay = NULL;
    s_clock = NULL;
    s_date = NULL;
    s_elapsed = NULL;
    s_cd = NULL;
    lvgl_port_unlock();
}
