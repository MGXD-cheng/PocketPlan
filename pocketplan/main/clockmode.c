/*
 * Clock mode: a landscape wall clock. The panel is fixed in portrait and drawn in
 * RGB direct mode, so display rotation is unavailable; instead the stage is laid
 * out in landscape coordinates and every label is turned 90 degrees with a style
 * transform (the same trick as the focus page, which keeps 1bpp text crisp).
 *
 * It shows the time, the date, the next alarm and up to four countdowns - no
 * focus timer, no tabs. The colour scheme follows focus_dark. There is no exit
 * hint on screen (a clock should stay clean): three taps within 2.5 s leave.
 */
#include "clockmode.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "alarms.h"
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

#define MAX_LINES 4
#define TAP_TO_EXIT 3
#define TAP_WINDOW_MS 2500

#define CLOCK_V 132
#define DATE_V  204
#define ALARM_V 242
#define CD_TOP  292
#define CD_STEP  32

#define C_LIGHT_BG 0xFFFFFF
#define C_LIGHT_FG 0x000000
#define C_DARK_BG  0x000000
#define C_DARK_FG  0xFFFFFF

static lv_obj_t *s_root;
static lv_obj_t *s_clock;
static lv_obj_t *s_date;
static lv_obj_t *s_alarm;
static lv_obj_t *s_cd[MAX_LINES];
static lv_timer_t *s_timer;
static lv_color_t s_fg;
static char s_day[11];
static int s_taps;
static uint32_t s_tap_first;

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

static lv_obj_t *make_label(const lv_font_t *font, int32_t u, int32_t v)
{
    lv_obj_t *label = lv_label_create(s_root);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, s_fg, 0);
    place(label, u, v);
    return label;
}

static void set_if_changed(lv_obj_t *label, const char *text)
{
    if (label != NULL && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

/* "<title> · 还有 12 天" - the same wording the focus page uses. */
static void countdown_text(const pp_countdown_t *cd, char *buf, size_t size)
{
    if (cd->days_left > 0) {
        snprintf(buf, size, "%s · 还有 %d 天", cd->title, cd->days_left);
    } else if (cd->days_left == 0) {
        snprintf(buf, size, "%s · 就是今天", cd->title);
    } else {
        snprintf(buf, size, "%s · 已过 %d 天", cd->title, -cd->days_left);
    }
}

static void update_countdowns(void)
{
    for (int i = 0; i < MAX_LINES; i++) {
        if (s_cd[i] == NULL) {
            continue;
        }
        const pp_countdown_t *cd = (i < pp_countdowns_count()) ? pp_countdowns_get(i) : NULL;
        if (cd == NULL) {
            lv_obj_add_flag(s_cd[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        char buf[176];
        countdown_text(cd, buf, sizeof(buf));
        set_if_changed(s_cd[i], buf);
        lv_obj_clear_flag(s_cd[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Earliest enabled alarm that is still ahead: today, tomorrow or later this
 * week. Returns false when nothing is enabled. */
static bool next_alarm_text(char *buf, size_t size)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    const int now_min = tm_now.tm_hour * 60 + tm_now.tm_min;
    const int today = (tm_now.tm_wday + 6) % 7; /* Monday = 0 */

    int best_day = 99;
    int best_min = 0;
    const char *best_label = NULL;
    for (int i = 0; i < pp_alarms_count(); i++) {
        const pp_alarm_t *a = pp_alarms_get(i);
        if (a == NULL || !a->enabled) {
            continue;
        }
        int hour = 0;
        int minute = 0;
        if (sscanf(a->time, "%d:%d", &hour, &minute) != 2) {
            continue;
        }
        const int amin = hour * 60 + minute;
        for (int d = 0; d < 8; d++) {
            const int idx = (today + d) % 7;
            if (a->days != 0 && (a->days & (1u << idx)) == 0) {
                continue;
            }
            if (d == 0 && amin <= now_min) {
                continue;
            }
            if (d < best_day || (d == best_day && amin < best_min)) {
                best_day = d;
                best_min = amin;
                best_label = a->label;
            }
            break; /* that was this alarm's earliest occurrence */
        }
    }
    if (best_day == 99) {
        return false;
    }
    static const char *const k_days[] = {"一", "二", "三", "四", "五", "六", "日"};
    if (best_day == 0) {
        snprintf(buf, size, "闹钟 %02d:%02d · %s", best_min / 60, best_min % 60, best_label);
    } else if (best_day == 1) {
        snprintf(buf, size, "闹钟 明天 %02d:%02d · %s", best_min / 60, best_min % 60,
                 best_label);
    } else {
        const int idx = (today + best_day) % 7;
        snprintf(buf, size, "闹钟 周%s %02d:%02d · %s", k_days[idx], best_min / 60,
                 best_min % 60, best_label);
    }
    return true;
}

static void update_alarm(void)
{
    char buf[128];
    if (s_alarm == NULL) {
        return;
    }
    if (next_alarm_text(buf, sizeof(buf))) {
        set_if_changed(s_alarm, buf);
        lv_obj_clear_flag(s_alarm, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_alarm, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update(void)
{
    char buf[32];
    pp_clock_format(buf, sizeof(buf), "%H:%M");
    set_if_changed(s_clock, buf);

    char day[11];
    pp_clock_today(day, sizeof(day));
    if (strcmp(day, s_day) != 0) {
        /* Midnight rollover while the clock is up: recompute the day counts. */
        snprintf(s_day, sizeof(s_day), "%s", day);
        pp_countdowns_refresh();
        update_countdowns();
    }

    struct tm tm_now;
    time_t now = time(NULL);
    localtime_r(&now, &tm_now);
    static const char *const k_weekdays[] = {"周日", "周一", "周二", "周三",
                                             "周四", "周五", "周六"};
    snprintf(buf, sizeof(buf), "%d月%d日 %s", tm_now.tm_mon + 1, tm_now.tm_mday,
             k_weekdays[tm_now.tm_wday % 7]);
    set_if_changed(s_date, buf);

    update_alarm();
}

static void timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update();
}

static void close_async(void *unused)
{
    (void)unused;
    pp_clockmode_close();
}

/* No exit hint on screen, so leaving takes three taps: two accidental taps while
 * dusting the screen do nothing, and a deliberate triple tap always works. */
static void root_clicked(lv_event_t *e)
{
    (void)e;
    const uint32_t now = lv_tick_get();
    if (s_taps == 0 || (uint32_t)(now - s_tap_first) > TAP_WINDOW_MS) {
        s_taps = 1;
        s_tap_first = now;
        return;
    }
    s_taps++;
    if (s_taps >= TAP_TO_EXIT) {
        s_taps = 0;
        /* Deleting the object from inside its own handler is not allowed. */
        lv_async_call(close_async, NULL);
    }
}

bool pp_clockmode_active(void)
{
    return s_root != NULL;
}

void pp_clockmode_open(void)
{
    if (s_root != NULL) {
        return;
    }
    const bool dark = pp_config()->focus_dark;
    s_fg = lv_color_hex(dark ? C_DARK_FG : C_LIGHT_FG);
    s_taps = 0;
    s_tap_first = 0;

    lvgl_port_lock(0);
    s_root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_center(s_root);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(dark ? C_DARK_BG : C_LIGHT_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_root, root_clicked, LV_EVENT_CLICKED, NULL);

    s_clock = make_label(&pp_font_96, LAND_W / 2, CLOCK_V);
    lv_label_set_text(s_clock, "--:--");
    s_date = make_label(&pp_font_20, LAND_W / 2, DATE_V);
    s_alarm = make_label(&pp_font_20, LAND_W / 2, ALARM_V);
    for (int i = 0; i < MAX_LINES; i++) {
        s_cd[i] = make_label(&pp_font_20, LAND_W / 2, CD_TOP + i * CD_STEP);
        lv_obj_add_flag(s_cd[i], LV_OBJ_FLAG_HIDDEN);
    }

    pp_clock_today(s_day, sizeof(s_day));
    update_countdowns();
    update();

    s_timer = lv_timer_create(timer_cb, 1000, NULL);
    if (s_timer != NULL) {
        lv_timer_set_repeat_count(s_timer, -1);
    }
    lvgl_port_unlock();
}

void pp_clockmode_close(void)
{
    if (s_root == NULL) {
        return;
    }
    lvgl_port_lock(0);
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    lv_obj_delete(s_root);
    s_root = NULL;
    s_clock = NULL;
    s_date = NULL;
    s_alarm = NULL;
    for (int i = 0; i < MAX_LINES; i++) {
        s_cd[i] = NULL;
    }
    lvgl_port_unlock();
}
