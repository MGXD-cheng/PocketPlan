/* Countdown tab: one card per event, nearest first, the day count in a badge. */
#include <stdio.h>
#include <string.h>

#include "alarms.h"
#include "countdowns.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "pages.h"
#include "pp_fonts.h"
#include "ui.h"

#define C_BG 0xFFFFFF
#define C_CARD 0xFFFFFF
#define C_LINE 0xD0D0D0
#define C_EDGE 0xC8C8C8
#define C_TEXT 0x000000
#define C_FILL 0xE4E4E4

static lv_obj_t *s_root;
static lv_obj_t *s_list;
static lv_obj_t *s_next;

static void format_date(const pp_countdown_t *item, char *buf, size_t size)
{
    int y = 1970, m = 1, d = 1;
    sscanf(item->date, "%4d-%2d-%2d", &y, &m, &d);
    if (item->yearly) {
        snprintf(buf, size, "每年 %d月%d日", m, d);
    } else {
        snprintf(buf, size, "%d年%d月%d日", y, m, d);
    }
}

static const pp_countdown_t *find_item(const char *id)
{
    for (int i = 0; i < pp_countdowns_count(); i++) {
        const pp_countdown_t *candidate = pp_countdowns_get(i);
        if (candidate && strcmp(candidate->id, id) == 0) {
            return candidate;
        }
    }
    return NULL;
}

static void card_clicked(lv_event_t *e)
{
    const pp_countdown_t *item = find_item((const char *)lv_event_get_user_data(e));
    if (item == NULL) {
        return;
    }
    char buf[96];
    if (item->note[0]) {
        snprintf(buf, sizeof(buf), "%s", item->note);
    } else if (item->days_left == 0) {
        snprintf(buf, sizeof(buf), "%s 就是今天", item->title);
    } else if (item->days_left > 0) {
        snprintf(buf, sizeof(buf), "%s 还有 %d 天", item->title, item->days_left);
    } else {
        snprintf(buf, sizeof(buf), "%s 已过 %d 天", item->title, -item->days_left);
    }
    pp_ui_toast(buf);
}

static void build_badge(lv_obj_t *card, const pp_countdown_t *item)
{
    bool past = item->days_left < 0;
    bool soon = !past && item->days_left <= 7;
    lv_obj_t *badge = lv_obj_create(card);
    lv_obj_set_size(badge, 84, 64);
    lv_obj_set_style_radius(badge, 12, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(past ? C_CARD : (soon ? 0x000000 : C_FILL)), 0);
    lv_obj_set_style_border_width(badge, past ? 1 : 0, 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(C_EDGE), 0);
    lv_obj_set_style_pad_all(badge, 2, 0);
    lv_obj_set_flex_flow(badge, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(badge, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *num = lv_label_create(badge);
    char buf[16];
    if (past) {
        snprintf(buf, sizeof(buf), "已过");
    } else {
        snprintf(buf, sizeof(buf), "%d", item->days_left);
    }
    lv_label_set_text(num, buf);
    lv_obj_set_style_text_font(num, &pp_font_32, 0);
    lv_obj_set_style_text_color(num, lv_color_hex(soon ? 0xFFFFFF : C_TEXT), 0);

    lv_obj_t *unit = lv_label_create(badge);
    if (past) {
        snprintf(buf, sizeof(buf), "%d 天", -item->days_left);
    } else if (item->days_left == 0) {
        snprintf(buf, sizeof(buf), "今天");
    } else if (item->days_left == 1) {
        snprintf(buf, sizeof(buf), "明天");
    } else {
        snprintf(buf, sizeof(buf), "天");
    }
    lv_label_set_text(unit, buf);
    lv_obj_set_style_text_color(unit, lv_color_hex(soon ? 0xFFFFFF : C_TEXT), 0);
}

static void build_card(lv_obj_t *parent, const pp_countdown_t *item)
{
    bool past = item->days_left < 0;
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, 84);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card,
                                 lv_color_hex(past ? C_EDGE
                                                   : (!past && item->days_left <= 7 ? 0x000000
                                                                                   : C_LINE)),
                                 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_column(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    /* The view array is rebuilt on every refresh, so the callback keeps an id
     * copy instead of a pointer into it. */
    static char id_bufs[PP_COUNTDOWNS_MAX][12];
    static int id_slot;
    char *id = id_bufs[id_slot++ % PP_COUNTDOWNS_MAX];
    snprintf(id, 12, "%s", item->id);
    lv_obj_add_event_cb(card, card_clicked, LV_EVENT_CLICKED, id);

    build_badge(card, item);

    lv_obj_t *col = lv_obj_create(card);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, lv_pct(100));
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_top(col, 10, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(col);
    lv_label_set_text(title, item->title);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    char date[32];
    format_date(item, date, sizeof(date));
    lv_obj_t *sub = lv_label_create(col);
    lv_label_set_text(sub, date);
    lv_obj_set_style_text_color(sub, lv_color_hex(C_TEXT), 0);
    lv_obj_set_width(sub, lv_pct(100));
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
}

/* The weekdays an alarm is armed for, "每天" when the mask is empty. */
static void repeat_text(uint8_t days, char *buf, size_t size)
{
    if (days == 0) {
        snprintf(buf, size, "每天");
        return;
    }
    static const char *const k_days[] = {"一", "二", "三", "四", "五", "六", "日"};
    size_t used = 0;
    used += snprintf(buf + used, size - used, "周");
    for (int i = 0; i < 7 && used + 3 < size; i++) {
        if (days & (1 << i)) {
            used += snprintf(buf + used, size - used, "%s", k_days[i]);
        }
    }
}

static void alarm_clicked(lv_event_t *e)
{
    const char *id = (const char *)lv_event_get_user_data(e);
    bool next = true;
    for (int i = 0; i < pp_alarms_count(); i++) {
        const pp_alarm_t *item = pp_alarms_get(i);
        if (item != NULL && strcmp(item->id, id) == 0) {
            next = !item->enabled;
            break;
        }
    }
    pp_alarms_set_enabled(id, next);
    pp_countdown_refresh();
}

static void build_alarm_row(lv_obj_t *parent, const pp_alarm_t *alarm)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 52);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_left(row, 12, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    /* The view array is rebuilt on every refresh, so copy the id. */
    static char id_bufs[PP_ALARMS_MAX][8];
    static int id_slot;
    char *id = id_bufs[id_slot++ % PP_ALARMS_MAX];
    snprintf(id, 8, "%s", alarm->id);
    lv_obj_add_event_cb(row, alarm_clicked, LV_EVENT_CLICKED, id);

    lv_obj_t *time = lv_label_create(row);
    lv_label_set_text(time, alarm->time);
    lv_obj_set_style_text_font(time, &pp_font_20, 0);
    lv_obj_set_style_text_color(time, lv_color_hex(C_TEXT), 0);

    lv_obj_t *col = lv_obj_create(row);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, lv_pct(100));
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);

    char repeat[32];
    repeat_text(alarm->days, repeat, sizeof(repeat));
    char sub[96];
    if (alarm->label[0]) {
        snprintf(sub, sizeof(sub), "%s · %s", alarm->label, repeat);
    } else {
        snprintf(sub, sizeof(sub), "%s", repeat);
    }
    lv_obj_t *name = lv_label_create(col);
    lv_label_set_text(name, sub);
    lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT), 0);
    lv_obj_set_width(name, lv_pct(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

    lv_obj_t *chip = lv_obj_create(row);
    lv_obj_set_size(chip, 48, 28);
    lv_obj_set_style_radius(chip, 14, 0);
    lv_obj_set_style_bg_color(chip,
                              lv_color_hex(alarm->enabled ? 0x000000 : C_FILL), 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_style_pad_all(chip, 0, 0);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *cl = lv_label_create(chip);
    lv_label_set_text(cl, alarm->enabled ? "开" : "关");
    lv_obj_set_style_text_color(cl,
                                lv_color_hex(alarm->enabled ? 0xFFFFFF : C_TEXT), 0);
    lv_obj_center(cl);
}

static void refresh_head(void)
{
    int count = pp_countdowns_count();
    char buf[64];
    const pp_countdown_t *next = NULL;
    for (int i = 0; i < count; i++) {
        const pp_countdown_t *item = pp_countdowns_get(i);
        if (item && item->days_left >= 0) {
            next = item;
            break;
        }
    }
    if (next == NULL) {
        snprintf(buf, sizeof(buf), count ? "全部已过" : "还没有");
    } else if (next->days_left == 0) {
        snprintf(buf, sizeof(buf), "今天：%s", next->title);
    } else {
        snprintf(buf, sizeof(buf), "最近 %d 天", next->days_left);
    }
    lv_label_set_text(s_next, buf);
}

static void build_list(void)
{
    lv_obj_clean(s_list);

    /* Alarms share this tab: a compact row per alarm, tap to arm/disarm. */
    lv_obj_t *alarm_title = lv_label_create(s_list);
    lv_label_set_text(alarm_title, "闹钟");
    lv_obj_set_style_text_color(alarm_title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_pad_top(alarm_title, 4, 0);

    int alarms = pp_alarms_count();
    for (int i = 0; i < alarms; i++) {
        const pp_alarm_t *alarm = pp_alarms_get(i);
        if (alarm != NULL) {
            build_alarm_row(s_list, alarm);
        }
    }
    if (alarms == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, "还没有闹钟（在网页端「倒数」标签里添加）");
        lv_obj_set_style_text_color(empty, lv_color_hex(C_TEXT), 0);
        lv_obj_set_width(empty, lv_pct(100));
    }

    lv_obj_t *cd_title = lv_label_create(s_list);
    lv_label_set_text(cd_title, "倒数日");
    lv_obj_set_style_text_color(cd_title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_pad_top(cd_title, 12, 0);

    int count = pp_countdowns_count();
    for (int i = 0; i < count; i++) {
        const pp_countdown_t *item = pp_countdowns_get(i);
        if (item != NULL) {
            build_card(s_list, item);
        }
    }
    if (count == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, "还没有倒数日\n\n在网页端「倒数」标签里添加");
        lv_obj_set_style_text_color(empty, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty, lv_pct(100));
        lv_obj_set_style_pad_top(empty, 40, 0);
    }
}

void pp_countdown_refresh(void)
{
    pp_countdowns_refresh();
    pp_alarms_refresh();
    if (s_root == NULL) {
        return;
    }
    lvgl_port_lock(0);
    refresh_head();
    build_list();
    lvgl_port_unlock();
}

void pp_countdown_create(lv_obj_t *parent)
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

    lv_obj_t *title = lv_label_create(head);
    lv_label_set_text(title, "倒数日");
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);

    s_next = lv_label_create(head);
    lv_label_set_text(s_next, "还没有");
    lv_obj_set_style_text_color(s_next, lv_color_hex(C_TEXT), 0);

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

    refresh_head();
    build_list();
}
