/* Chat page: bubbles, a one line input and the LVGL keyboard. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "config.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "pages.h"
#include "ui.h"

#define C_BG 0xFFFFFF
#define C_TEXT 0x000000
#define C_DIM 0x000000
#define C_ACCENT 0x000000

#define KB_H 240
/* 480 px screen minus the 20 px chat padding: 400 leaves room for the bubble
 * border and keeps roughly 24 CJK characters per line. 320 was so narrow that
 * a trailing "。" ended up alone on its own line. */
#define BUBBLE_MAX 400

static lv_obj_t *s_root;
static lv_obj_t *s_chat;
static lv_obj_t *s_input;
static lv_obj_t *s_send;
static lv_obj_t *s_kb;
static lv_obj_t *s_input_row;
static lv_obj_t *s_pending;
static lv_obj_t *s_pending_bubble;
static bool s_busy;

/* The model answers in Markdown but LVGL draws it literally, so **bold** and
 * `code` used to show up as stray asterisks and backticks. Drop the markers
 * and keep the words. */
static char *plain_text(const char *src)
{
    if (src == NULL) {
        return NULL;
    }
    size_t len = strlen(src);
    char *out = malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t o = 0;
    bool line_start = true;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '*' || c == '`') {
            continue;
        }
        /* "#" heading markers at the start of a line */
        if (c == '#' && line_start) {
            while (i + 1 < len && src[i + 1] == '#') {
                i++;
            }
            if (i + 1 < len && src[i + 1] == ' ') {
                i++;
            }
            continue;
        }
        out[o++] = c;
        line_start = (c == '\n');
    }
    out[o] = '\0';
    return out;
}

static void scroll_bottom(void)
{
    uint32_t count = lv_obj_get_child_count(s_chat);
    if (count > 0) {
        lv_obj_scroll_to_view(lv_obj_get_child(s_chat, (int32_t)count - 1), LV_ANIM_OFF);
    }
}

/* Measure the text as a single line (LV_COORD_MAX = no wrapping), then cap the
 * label at BUBBLE_MAX and size the bubble from the label. Measuring with a max
 * width already applied made LVGL report the width of a mid-word break, which
 * is why long replies used to come out in a very narrow column; sizing the
 * bubble from its own content produced a 130 px bubble that clipped every
 * line. 10 px pad + 1 px border on each side. */
static void fit_bubble(lv_obj_t *label, lv_obj_t *bubble, const char *text)
{
    lv_label_set_text(label, text);
    lv_point_t size;
    lv_text_get_size(&size, text, lv_obj_get_style_text_font(label, LV_PART_MAIN), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int32_t w = size.x > BUBBLE_MAX ? BUBBLE_MAX : size.x + 2;
    lv_obj_set_width(label, w);
    lv_obj_set_width(bubble, w + 2 * 10 + 2 * 1);
}

static lv_obj_t *add_bubble(const char *text, bool user)
{
    lv_obj_t *row = lv_obj_create(s_chat);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_style_radius(bubble, 14, 0);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(user ? 0xE4E4E4 : 0xFFFFFF), 0);
    lv_obj_set_style_border_width(bubble, 1, 0);
    lv_obj_set_style_border_color(bubble, lv_color_hex(0xC8C8C8), 0);
    lv_obj_set_style_pad_all(bubble, 10, 0);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(bubble);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_line_space(label, 4, 0);
    fit_bubble(label, bubble, text);
    return label;
}

typedef struct {
    char *reply;
    bool error;
} reply_t;

static void reply_done(void *arg)
{
    reply_t *r = (reply_t *)arg;
    char *text = plain_text(r->reply ? r->reply : "(空)");
    if (text == NULL) {
        text = strdup("(空)");
    }
    lvgl_port_lock(0);
    if (s_pending) {
        /* The placeholder was sized for "正在思考...", so re-measure: without
         * this the real answer was drawn inside a ~70 px wide bubble. */
        fit_bubble(s_pending, s_pending_bubble, text ? text : "(空)");
        s_pending = NULL;
        s_pending_bubble = NULL;
    } else {
        add_bubble(text ? text : "(空)", false);
    }
    s_busy = false;
    lv_obj_clear_state(s_send, LV_STATE_DISABLED);
    scroll_bottom();
    lvgl_port_unlock();
    free(text);
    free(r->reply);
    free(r);
}

static void ai_task(void *arg)
{
    char *message = (char *)arg;
    char *reply = pp_ai_send(message);
    bool error = pp_ai_last_error();
    if (reply && !error) {
        pp_ai_history_push("user", message);
        pp_ai_history_push("assistant", reply);
    }
    free(message);
    reply_t *r = calloc(1, sizeof(reply_t));
    if (r) {
        r->reply = reply;
        r->error = error;
        lv_async_call(reply_done, r);
    } else {
        free(reply);
    }
    vTaskDelete(NULL);
}

static void hide_keyboard(void)
{
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_input_row, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void do_send(void)
{
    const char *text = lv_textarea_get_text(s_input);
    if (text == NULL || text[0] == '\0' || s_busy) {
        return;
    }
    if (!pp_ai_configured()) {
        pp_ui_toast("请先在网页端配置 API Key");
        return;
    }
    char *copy = strdup(text);
    if (copy == NULL) {
        return;
    }
    lvgl_port_lock(0);
    add_bubble(text, true);
    lv_textarea_set_text(s_input, "");
    hide_keyboard();
    lv_obj_add_state(s_send, LV_STATE_DISABLED);
    lv_obj_t *label = add_bubble("正在思考...", false);
    s_pending = label;
    s_pending_bubble = lv_obj_get_parent(label);
    s_busy = true;
    scroll_bottom();
    lvgl_port_unlock();

    if (xTaskCreate(ai_task, "ai_chat", 12288, copy, 5, NULL) != pdPASS) {
        free(copy);
        pp_ui_toast("任务创建失败");
    }
}

static void send_clicked(lv_event_t *e)
{
    (void)e;
    do_send();
}

static void input_clicked(lv_event_t *e)
{
    (void)e;
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_input_row, LV_ALIGN_BOTTOM_MID, 0, -KB_H);
    lv_keyboard_set_textarea(s_kb, s_input);
    lv_obj_move_foreground(s_kb);
}

/* Debug helper for the boot tour: raise the keyboard so a capture covers the
 * symbol keys (Enter and backspace were the glyphs missing from the font). */
void pp_ai_demo_keyboard(void)
{
    if (s_kb == NULL || s_input == NULL) {
        return;
    }
    lvgl_port_lock(0);
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_input_row, LV_ALIGN_BOTTOM_MID, 0, -KB_H);
    lv_keyboard_set_textarea(s_kb, s_input);
    lv_obj_move_foreground(s_kb);
    lvgl_port_unlock();
}

static void kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        hide_keyboard();
        do_send();
    } else if (code == LV_EVENT_CANCEL) {
        hide_keyboard();
    }
}

static void clear_clicked(lv_event_t *e)
{
    (void)e;
    lvgl_port_lock(0);
    lv_obj_clean(s_chat);
    pp_ai_history_clear();
    s_pending = NULL;
    s_pending_bubble = NULL;
    s_busy = false;
    lv_obj_clear_state(s_send, LV_STATE_DISABLED);
    lvgl_port_unlock();
    pp_ui_toast("对话已清空");
}

void pp_ai_refresh(void)
{
    if (s_root == NULL) {
        return;
    }
    /* nothing dynamic beyond the bubbles */
}

void pp_ai_create(lv_obj_t *parent)
{
    s_root = parent;
    lv_obj_set_style_bg_color(s_root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(s_root);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, 52);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_left(bar, 16, 0);
    lv_obj_set_style_pad_right(bar, 12, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(bar);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    if (pp_ai_configured()) {
        lv_label_set_text(title, pp_config()->ai_model);
    } else {
        lv_label_set_text(title, "未配置 API Key");
        lv_obj_set_style_text_color(title, lv_color_hex(C_DIM), 0);
    }

    lv_obj_t *clear = lv_button_create(bar);
    lv_obj_set_size(clear, 72, 36);
    lv_obj_set_style_radius(clear, 10, 0);
    lv_obj_set_style_bg_color(clear, lv_color_hex(0xE4E4E4), 0);
    lv_obj_set_style_shadow_width(clear, 0, 0);
    lv_obj_add_event_cb(clear, clear_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(clear);
    lv_label_set_text(cl, "清空");
    lv_obj_set_style_text_color(cl, lv_color_hex(C_DIM), 0);
    lv_obj_center(cl);

    s_chat = lv_obj_create(s_root);
    lv_obj_set_width(s_chat, lv_pct(100));
    lv_obj_set_flex_grow(s_chat, 1);
    lv_obj_set_style_bg_opa(s_chat, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chat, 0, 0);
    lv_obj_set_style_pad_left(s_chat, 10, 0);
    lv_obj_set_style_pad_right(s_chat, 10, 0);
    lv_obj_set_style_pad_top(s_chat, 10, 0);
    lv_obj_set_style_pad_bottom(s_chat, 10, 0);
    lv_obj_set_style_pad_row(s_chat, 8, 0);
    lv_obj_set_flex_flow(s_chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_chat, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_chat, LV_DIR_VER);

    if (pp_ai_history_count() > 0) {
        /* Replay the cached conversation (it lives in ai_chat.json, see
         * pp_data_on_sd()), otherwise start with the greeting. */
        for (int i = 0; i < pp_ai_history_count(); i++) {
            const char *role = pp_ai_history_role(i);
            const char *body = pp_ai_history_text(i);
            if (body == NULL) {
                continue;
            }
            char *text = plain_text(body);
            add_bubble(text ? text : body, role != NULL && strcmp(role, "user") == 0);
            free(text);
        }
    } else {
        add_bubble("你好，我是 PocketPlan 的助手。\n可以问我问题，或者让我帮你理清今天的计划。",
                   false);
    }

    s_input_row = lv_obj_create(s_root);
    lv_obj_set_size(s_input_row, lv_pct(100), 60);
    lv_obj_align(s_input_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_input_row, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_input_row, 0, 0);
    lv_obj_set_style_pad_left(s_input_row, 10, 0);
    lv_obj_set_style_pad_right(s_input_row, 10, 0);
    lv_obj_set_style_pad_column(s_input_row, 8, 0);
    lv_obj_set_flex_flow(s_input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_input_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_input_row, LV_OBJ_FLAG_SCROLLABLE);

    s_input = lv_textarea_create(s_input_row);
    lv_obj_set_flex_grow(s_input, 1);
    lv_obj_set_height(s_input, 44);
    lv_obj_set_style_bg_color(s_input, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_input, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_input, 1, 0);
    lv_obj_set_style_border_color(s_input, lv_color_hex(0x000000), 0);
    lv_obj_set_style_radius(s_input, 10, 0);
    lv_obj_set_style_text_color(s_input, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_color(s_input, lv_color_hex(0x707070),
                                LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_pad_left(s_input, 10, 0);
    lv_obj_set_style_pad_right(s_input, 10, 0);
    lv_textarea_set_one_line(s_input, true);
    lv_textarea_set_placeholder_text(s_input, "说点什么…");
    lv_obj_add_event_cb(s_input, input_clicked, LV_EVENT_CLICKED, NULL);

    s_send = lv_button_create(s_input_row);
    lv_obj_set_size(s_send, 80, 44);
    lv_obj_set_style_radius(s_send, 10, 0);
    lv_obj_set_style_bg_color(s_send, lv_color_hex(0xE4E4E4), 0);
    lv_obj_set_style_border_width(s_send, 1, 0);
    lv_obj_set_style_border_color(s_send, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_width(s_send, 0, 0);
    lv_obj_add_event_cb(s_send, send_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(s_send);
    lv_label_set_text(sl, "发送");
    lv_obj_set_style_text_color(sl, lv_color_hex(0x000000), 0);
    lv_obj_center(sl);

    s_kb = lv_keyboard_create(s_root);
    lv_obj_set_size(s_kb, lv_pct(100), KB_H);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, kb_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_event, LV_EVENT_CANCEL, NULL);

    scroll_bottom();
}
