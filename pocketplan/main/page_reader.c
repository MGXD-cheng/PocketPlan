/* Full screen text reader with tap-to-turn pages. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "pages.h"
#include "pp_fonts.h"
#include "reader.h"
#include "ui.h"

#define C_TEXT 0x000000
#define C_DIM 0x000000
#define C_ACCENT 0x000000

#define TOP_H 56
#define BOT_H 64
#define TEXT_W 448
#define TEXT_H 496
#define PAGE_BUF 12288
#define HISTORY_MAX 512

static lv_obj_t *s_overlay;
static lv_obj_t *s_title;
static lv_obj_t *s_text;
static lv_obj_t *s_percent;
static lv_obj_t *s_prev_btn;

static pp_book_t *s_book;
static char *s_page_buf;
static int32_t s_off;
static int32_t s_next;
static int32_t s_history[HISTORY_MAX];
static int s_hist_len;
static char s_path[400];

static const lv_font_t *reader_font(void)
{
    pp_config_t *cfg = pp_config();
    /* 1bpp SimHei in two sizes; both are pure black with no grey edges. */
    return (cfg->font_size >= 20) ? &pp_font_20 : &pp_font_16;
}

static void update_percent(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d%%", pp_reader_percent(s_book, s_off));
    lv_label_set_text(s_percent, buf);
    if (s_prev_btn) {
        if (s_off <= 0) {
            lv_obj_add_state(s_prev_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(s_prev_btn, LV_STATE_DISABLED);
        }
    }
}

static void render_at(int32_t start)
{
    if (s_book == NULL || s_page_buf == NULL) {
        return;
    }
    int32_t next = start;
    bool ok = pp_reader_page(s_book, start, reader_font(), TEXT_W, TEXT_H,
                             pp_config()->line_space, s_page_buf, PAGE_BUF, &next);
    lvgl_port_lock(0);
    /* The page was measured with reader_font(), so the label must use it too. */
    lv_obj_set_style_text_font(s_text, reader_font(), 0);
    lv_obj_set_style_text_line_space(s_text, pp_config()->line_space, 0);
    if (ok && s_page_buf[0]) {
        lv_label_set_text(s_text, s_page_buf);
        s_off = start;
        s_next = next;
    } else {
        lv_label_set_text(s_text, "已经是最后一页了");
        s_off = start;
        s_next = start;
    }
    update_percent();
    lv_obj_scroll_to_y(s_text, 0, LV_ANIM_OFF);
    lvgl_port_unlock();
    pp_reader_save_pos(s_path, s_off);
}

static void next_page(void)
{
    if (s_next <= s_off) {
        pp_ui_toast("已经是最后一页");
        return;
    }
    if (s_hist_len < HISTORY_MAX) {
        s_history[s_hist_len++] = s_off;
    }
    render_at(s_next);
}

static void prev_page(void)
{
    if (s_hist_len > 0) {
        render_at(s_history[--s_hist_len]);
        return;
    }
    if (s_off <= 0) {
        return;
    }
    /* No history (e.g. resumed reading): walk forward from the start. */
    int32_t best = 0;
    int32_t cursor = 0;
    char *tmp = s_page_buf;
    int32_t guard = 0;
    while (cursor < s_off && guard++ < 100000) {
        int32_t next = cursor;
        if (!pp_reader_page(s_book, cursor, reader_font(), TEXT_W, TEXT_H,
                            pp_config()->line_space, tmp, PAGE_BUF, &next)) {
            break;
        }
        if (next >= s_off) {
            break;
        }
        best = cursor;
        cursor = next;
    }
    render_at(best);
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    next_page();
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    prev_page();
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    pp_reader_ui_close();
}

static void text_clicked(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_SHORT_CLICKED) {
        lv_indev_t *indev = lv_indev_active();
        if (indev == NULL) {
            next_page();
            return;
        }
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        lv_area_t area;
        lv_obj_get_coords(s_text, &area);
        if (point.x < area.x1 + (area.x2 - area.x1) / 3) {
            prev_page();
        } else {
            next_page();
        }
    }
}

static lv_obj_t *make_bar(lv_obj_t *parent, int32_t height, bool bottom)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, lv_pct(100), height);
    if (bottom) {
        lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    } else {
        lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    }
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_left(bar, 12, 0);
    lv_obj_set_style_pad_right(bar, 12, 0);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, 44);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xE4E4E4), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 14, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(C_ACCENT), 0);
    lv_obj_center(label);
    return btn;
}

void pp_reader_ui_open(const char *path)
{
    if (path == NULL) {
        return;
    }
    if (s_overlay == NULL) {
        pp_ui_toast("阅读器未初始化");
        return;
    }
    snprintf(s_path, sizeof(s_path), "%s", path);
    pp_book_t *book = pp_reader_open(path);
    if (book == NULL) {
        pp_ui_toast("打开失败：文件过大或不支持的编码");
        return;
    }
    if (s_book) {
        pp_reader_close(s_book);
    }
    s_book = book;
    s_hist_len = 0;
    s_off = 0;
    s_next = 0;

    lvgl_port_lock(0);
    lv_label_set_text(s_title, pp_reader_title(s_book));
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    lvgl_port_unlock();

    int32_t pos = pp_reader_load_pos(path);
    if (pos < 0 || (size_t)pos >= pp_reader_length(s_book)) {
        pos = 0;
    }
    render_at(pos);
}

bool pp_reader_ui_open_now(void)
{
    return s_overlay != NULL && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

void pp_reader_ui_close(void)
{
    if (s_book) {
        pp_reader_save_pos(s_path, s_off);
        pp_reader_close(s_book);
        s_book = NULL;
    }
    lvgl_port_lock(0);
    if (s_overlay) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_text, "");
    }
    lvgl_port_unlock();
}

void pp_reader_ui_init(void)
{
    if (s_page_buf == NULL) {
        s_page_buf = heap_caps_malloc(PAGE_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_page_buf == NULL) {
            s_page_buf = malloc(PAGE_BUF);
        }
    }

    lvgl_port_lock(0);
    s_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *top = make_bar(s_overlay, TOP_H, false);
    lv_obj_t *back = make_button(top, LV_SYMBOL_LEFT " 返回", back_cb);
    lv_obj_set_width(back, 100);

    s_title = lv_label_create(top);
    lv_label_set_text(s_title, "");
    lv_obj_set_style_text_color(s_title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_flex_grow(s_title, 1);
    lv_obj_set_style_pad_left(s_title, 8, 0);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);

    s_percent = lv_label_create(top);
    lv_label_set_text(s_percent, "0%");
    lv_obj_set_style_text_color(s_percent, lv_color_hex(C_DIM), 0);

    s_text = lv_label_create(s_overlay);
    lv_label_set_text(s_text, "");
    lv_obj_set_size(s_text, lv_pct(100), TEXT_H + 8);
    lv_obj_align(s_text, LV_ALIGN_TOP_MID, 0, TOP_H + 4);
    lv_obj_set_style_pad_left(s_text, 16, 0);
    lv_obj_set_style_pad_right(s_text, 16, 0);
    lv_obj_set_style_text_color(s_text, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(s_text, reader_font(), 0);
    lv_obj_set_style_text_line_space(s_text, pp_config()->line_space, 0);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(s_text, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_text, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_text, text_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bottom = make_bar(s_overlay, BOT_H, true);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    s_prev_btn = make_button(bottom, LV_SYMBOL_LEFT " 上一页", prev_cb);
    lv_obj_set_width(s_prev_btn, 150);
    lv_obj_t *next = make_button(bottom, "下一页 " LV_SYMBOL_RIGHT, next_cb);
    lv_obj_set_width(next, 150);

    lvgl_port_unlock();
}