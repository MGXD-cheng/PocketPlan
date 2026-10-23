/* Daily plan board: one card per plan, tap to check in. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "pages.h"
#include "clock.h"
#include "plans.h"
#include "ui.h"

#define C_BG 0xFFFFFF
#define C_CARD 0xFFFFFF
#define C_LINE 0xD0D0D0
#define C_TEXT 0x000000
#define C_DIM 0x000000
#define C_ACCENT 0x000000
#define C_DONE 0x000000

static lv_obj_t *s_root;
static lv_obj_t *s_date;
static lv_obj_t *s_stats;
static lv_obj_t *s_list;
static char s_day[11];

static const char *const k_weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

static void set_label(lv_obj_t *label, const char *text)
{
    if (label) {
        lv_label_set_text(label, text);
    }
}

static void refresh_labels(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char buf[64];
    snprintf(buf, sizeof(buf), "%d月%d日 %s", tm_now.tm_mon + 1, tm_now.tm_mday,
             k_weekdays[tm_now.tm_wday % 7]);
    set_label(s_date, buf);

    int done = 0, active = 0, streak = 0;
    pp_plans_stats(&done, &active, &streak);
    snprintf(buf, sizeof(buf), "完成 %d/%d", done, active);
    set_label(s_stats, buf);
}

static void card_clicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    const char *id = (const char *)lv_event_get_user_data(e);
    bool state = false;
    esp_err_t err = pp_plans_checkin(id, NULL, -1, &state);
    if (err != ESP_OK) {
        pp_ui_toast("操作失败");
        return;
    }
    pp_ui_toast(state ? "已打卡" : "已取消打卡");
    pp_home_refresh();
}

static const char *plan_subtitle(const pp_plan_t *plan, char *buf, size_t size)
{
    const char *repeat = plan->repeat[0] ? plan->repeat : "daily";
    if (strcmp(repeat, "weekly") == 0) {
        snprintf(buf, size, "每周 %d 天 · 连续 %d 天", __builtin_popcount(plan->weekdays),
                 plan->streak);
    } else if (strcmp(repeat, "once") == 0) {
        snprintf(buf, size, "一次 · %s", plan->date);
    } else if (strcmp(repeat, "interval") == 0) {
        snprintf(buf, size, "每 %d 天 · 连续 %d 天", plan->interval, plan->streak);
    } else {
        snprintf(buf, size, "每天 · 连续 %d 天", plan->streak);
    }
    return buf;
}

static void build_card(lv_obj_t *parent, const pp_plan_t *plan)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, 76);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(plan->done_today ? 0xE4E4E4 : C_CARD), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_column(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    static char id_bufs[PP_PLANS_MAX][12];
    static int id_slot;
    int slot = id_slot++ % PP_PLANS_MAX;
    snprintf(id_bufs[slot], sizeof(id_bufs[slot]), "%s", plan->id);
    lv_obj_add_event_cb(card, card_clicked, LV_EVENT_CLICKED, id_bufs[slot]);

    lv_obj_t *circle = lv_obj_create(card);
    lv_obj_set_size(circle, 44, 44);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    /* A checked row is a solid black disc with a white tick: the old version
     * was a white disc inside a black ring, so the ring read as a white gap
     * around the tick. Nothing but pure black and white on this panel. */
    lv_obj_set_style_bg_color(circle, lv_color_hex(plan->done_today ? C_DONE : 0xFFFFFF), 0);
    lv_obj_set_style_border_width(circle, plan->done_today ? 0 : 2, 0);
    /* The panel is 1-bit, so a light ring simply disappears: an empty circle
     * has to be drawn in black too. */
    lv_obj_set_style_border_color(circle, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(circle, 0, 0);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_CLICKABLE);
    if (plan->done_today) {
        lv_obj_t *ok = lv_label_create(circle);
        lv_label_set_text(ok, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(ok, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(ok);
    }

    lv_obj_t *col = lv_obj_create(card);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, lv_pct(100));
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_top(col, 6, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(col);
    lv_label_set_text(title, plan->title);
    lv_obj_set_style_text_color(title, lv_color_hex(plan->done_today ? C_DIM : C_TEXT), 0);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    char sub[96];
    lv_obj_t *sub_label = lv_label_create(col);
    lv_label_set_text(sub_label, plan_subtitle(plan, sub, sizeof(sub)));
    lv_obj_set_style_text_color(sub_label, lv_color_hex(C_DIM), 0);
    lv_obj_set_width(sub_label, lv_pct(100));
    lv_label_set_long_mode(sub_label, LV_LABEL_LONG_DOT);
}

static void build_list(void)
{
    lv_obj_clean(s_list);
    int count = pp_plans_count();
    int shown = 0;
    for (int i = 0; i < count; i++) {
        const pp_plan_t *plan = pp_plans_get(i);
        if (plan == NULL || plan->archived) {
            continue;
        }
        build_card(s_list, plan);
        shown++;
    }
    if (shown == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, "今天没有计划\n\n在网页端添加，或检查日期设置");
        lv_obj_set_style_text_color(empty, lv_color_hex(C_DIM), 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty, lv_pct(100));
        lv_obj_set_style_pad_top(empty, 60, 0);
    }
}

void pp_home_refresh(void)
{
    if (s_root == NULL) {
        return;
    }
    lvgl_port_lock(0);
    refresh_labels();
    build_list();
    lvgl_port_unlock();
}

void pp_home_tick(void)
{
    char day[11];
    pp_clock_today(day, sizeof(day));
    if (strcmp(day, s_day) != 0) {
        snprintf(s_day, sizeof(s_day), "%s", day);
        pp_plans_refresh();
        pp_home_refresh();
        return;
    }
    if (s_root && !lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN)) {
        lvgl_port_lock(0);
        refresh_labels();
        lvgl_port_unlock();
    }
}

void pp_home_create(lv_obj_t *parent)
{
    s_root = parent;
    lv_obj_set_style_bg_color(s_root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head = lv_obj_create(s_root);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_height(head, 52);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_left(head, 16, 0);
    lv_obj_set_style_pad_right(head, 16, 0);
    lv_obj_set_style_pad_top(head, 12, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    s_date = lv_label_create(head);
    lv_obj_set_style_text_color(s_date, lv_color_hex(C_TEXT), 0);

    s_stats = lv_label_create(head);
    lv_obj_set_style_text_color(s_stats, lv_color_hex(C_ACCENT), 0);

    s_list = lv_obj_create(s_root);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_left(s_list, 16, 0);
    lv_obj_set_style_pad_right(s_list, 16, 0);
    lv_obj_set_style_pad_bottom(s_list, 16, 0);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    pp_clock_today(s_day, sizeof(s_day));
    refresh_labels();
    build_list();
}