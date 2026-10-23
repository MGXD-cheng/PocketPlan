/* Music library. Playback needs the PCM5102A DAC which is not wired yet, so
 * this page is a browser plus a clear status banner.
 *
 * Both halves of the old freeze are gone:
 *  - the scan stats every file on the card (115 files here), so it runs on its
 *    own task while the page shows "读取中...";
 *  - creating a row is not cheap in LVGL and the cost grows with the number of
 *    siblings (the same O(n^2) cliff the file browser hit), so the rows are
 *    appended in small chunks from an lv_timer instead of one blocking loop.
 *    The list is still the complete library, it just fills in progressively.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "pages.h"
#include "ui.h"

#define C_BG 0xFFFFFF
#define C_CARD 0xFFFFFF
#define C_TEXT 0x000000
#define C_DIM 0x000000
#define C_ACCENT 0x000000

#define MUSIC_DIR "/sdcard/music"
#define MAX_TRACKS 200
#define CHUNK 40       /* rows per timer tick */
#define TICK_MS 20
#define ROW_H 64       /* hand-placed row pitch: 2 text lines + padding */

static const char *TAG = "music";

typedef struct {
    char name[128];
    unsigned long long size;
} track_t;

static lv_obj_t *s_root;
static lv_obj_t *s_list;
static lv_obj_t *s_banner;
static lv_obj_t *s_status;
static track_t s_tracks[MAX_TRACKS];
static int s_count;
static volatile bool s_scanning;
static volatile bool s_rescan;
static lv_timer_t *s_render_timer;
static int s_render_i;
static int64_t s_render_t0;

static bool is_audio(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return false;
    }
    return strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".flac") == 0 ||
           strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".m4a") == 0 ||
           strcasecmp(dot, ".aac") == 0 || strcasecmp(dot, ".ogg") == 0;
}

/* Runs on the scan task, never on the LVGL task. */
static void scan_music(void)
{
    s_count = 0;
    DIR *dir = opendir(MUSIC_DIR);
    if (dir == NULL) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && s_count < MAX_TRACKS) {
        if (de->d_name[0] == '.' || !is_audio(de->d_name)) {
            continue;
        }
        char full[400];
        snprintf(full, sizeof(full), "%s/%s", MUSIC_DIR, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || S_ISDIR(st.st_mode)) {
            continue;
        }
        snprintf(s_tracks[s_count].name, sizeof(s_tracks[s_count].name), "%s", de->d_name);
        s_tracks[s_count].size = (unsigned long long)st.st_size;
        s_count++;
    }
    closedir(dir);
}

static void track_clicked(lv_event_t *e)
{
    (void)e;
    if (!pp_config()->music_ready) {
        pp_ui_toast("音频输出未接线，暂时只能在网页端管理");
        return;
    }
    pp_ui_toast("播放功能即将上线");
}

static void set_status(const char *text)
{
    if (s_status != NULL) {
        lv_label_set_text(s_status, text);
    }
}

static lv_obj_t *add_note(const char *text, int pad_top)
{
    lv_obj_t *label = lv_label_create(s_list);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_pad_top(label, pad_top, 0);
    return label;
}

/* One library row. Caller holds the LVGL lock.
 *
 * One label per track on purpose: the card layout (container + 3 children)
 * cost 6 s to build 115 rows, LVGL object creation getting slower with every
 * sibling, so the row count is now also the object count. */
static void build_row(int i)
{
    char size[16];
    unsigned long long kb = (s_tracks[i].size + 1023) / 1024;
    if (kb >= 1024) {
        snprintf(size, sizeof(size), "%.1f MB", (double)kb / 1024.0);
    } else {
        snprintf(size, sizeof(size), "%llu KB", kb);
    }

    char text[220];
    snprintf(text, sizeof(text), LV_SYMBOL_AUDIO " %s\n%s", s_tracks[i].name, size);

    lv_obj_t *row = lv_label_create(s_list);
    lv_obj_set_pos(row, 0, i * ROW_H);
    lv_obj_set_size(row, lv_pct(100), ROW_H);
    lv_obj_set_style_text_color(row, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_pad_top(row, 7, 0);
    lv_obj_set_style_pad_bottom(row, 7, 0);
    lv_obj_set_style_text_line_space(row, 2, 0);
    lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
    lv_label_set_text(row, text);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, track_clicked, LV_EVENT_CLICKED, NULL);
}

static void start_scan(void);

/* Fills the list a few rows per tick so the page stays scrollable while the
 * library is being drawn. */
static void render_tick(lv_timer_t *timer)
{
    lvgl_port_lock(0);
    int end = s_render_i + CHUNK;
    if (end > s_count) {
        end = s_count;
    }
    for (; s_render_i < end; s_render_i++) {
        build_row(s_render_i);
    }
    bool done = (s_render_i >= s_count);
    if (!done) {
        char buf[48];
        snprintf(buf, sizeof(buf), "读取中 %d/%d", s_render_i, s_count);
        set_status(buf);
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "共 %d 首", s_count);
        set_status(buf);
    }
    if (done) {
        lv_obj_clear_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
    if (!done) {
        return;
    }
    lv_timer_delete(timer);
    s_render_timer = NULL;
    ESP_LOGI(TAG, "%d rows built in %lld ms", s_count,
             (long long)((esp_timer_get_time() - s_render_t0) / 1000));
    if (s_count == 0) {
        lvgl_port_lock(0);
        lv_obj_clear_flag(s_list, LV_OBJ_FLAG_HIDDEN);
        add_note("还没有音乐\n\n把 mp3 放进 SD 卡的 music 文件夹", 50);
        lvgl_port_unlock();
    }
    if (s_rescan) {
        /* The tab was entered again while the scan was running. */
        s_rescan = false;
        start_scan();
    }
}

/* Called from the LVGL task through lv_async_call once the scan is done. */
static void begin_render(void *unused)
{
    (void)unused;
    if (s_root == NULL) {
        return;
    }
    lvgl_port_lock(0);
    lv_obj_clean(s_list);
    /* Keep the list hidden while it is filled: an invisible object skips
     * invalidation, and every invalidation here costs a redraw of the whole
     * list area, which grows with the number of rows already added. */
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    s_render_i = 0;
    if (s_count == 0) {
        add_note("还没有音乐\n\n把 mp3 放进 SD 卡的 music 文件夹", 50);
        set_status("共 0 首");
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "读取中 0/%d", s_count);
        set_status(buf);
    }
    if (s_render_timer != NULL) {
        lv_timer_delete(s_render_timer);
        s_render_timer = NULL;
    }
    if (s_count > 0) {
        s_render_t0 = esp_timer_get_time();
        s_render_timer = lv_timer_create(render_tick, TICK_MS, NULL);
        if (s_render_timer != NULL) {
            lv_timer_set_repeat_count(s_render_timer, -1);
        }
    }
    lvgl_port_unlock();
}

static void scan_task(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();
    scan_music();
    ESP_LOGI(TAG, "scanned %d tracks in %lld ms", s_count,
             (long long)((esp_timer_get_time() - t0) / 1000));
    s_scanning = false;
    if (s_root != NULL) {
        lv_async_call(begin_render, NULL);
    }
    vTaskDelete(NULL);
}

static void start_scan(void)
{
    if (s_scanning) {
        s_rescan = true;
        return;
    }
    s_scanning = true;
    if (xTaskCreate(scan_task, "pp_music", 4096, NULL, 2, NULL) != pdPASS) {
        s_scanning = false;
        begin_render(NULL);
    }
}

void pp_music_refresh(void)
{
    if (s_root == NULL) {
        return;
    }
    lvgl_port_lock(0);
    if (!pp_config()->music_ready) {
        lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    }
    if (!s_scanning) {
        if (s_render_timer != NULL) {
            lv_timer_delete(s_render_timer);
            s_render_timer = NULL;
        }
        lv_obj_clean(s_list);
        add_note("读取中...", 60);
        set_status("读取中...");
    }
    lvgl_port_unlock();
    start_scan();
}

void pp_music_create(lv_obj_t *parent)
{
    s_root = parent;
    lv_obj_set_style_bg_color(s_root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    s_banner = lv_label_create(s_root);
    lv_obj_set_width(s_banner, lv_pct(100));
    lv_label_set_text(s_banner, "音频输出（PCM5102A / I2S）尚未接线，\n当前只能浏览和通过网页管理音乐文件。");
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(0xE4E4E4), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_banner, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(s_banner, 12, 0);
    lv_obj_set_style_radius(s_banner, 0, 0);
    lv_label_set_long_mode(s_banner, LV_LABEL_LONG_WRAP);

    s_status = lv_label_create(s_root);
    lv_label_set_text(s_status, "读取中...");
    lv_obj_set_style_text_color(s_status, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_pad_left(s_status, 14, 0);
    lv_obj_set_style_pad_top(s_status, 6, 0);

    s_list = lv_obj_create(s_root);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_left(s_list, 12, 0);
    lv_obj_set_style_pad_right(s_list, 12, 0);
    lv_obj_set_style_pad_top(s_list, 4, 0);
    lv_obj_set_style_pad_bottom(s_list, 16, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    /* No flex here: rows are placed by hand (see build_row). Re-laying out the
     * whole library on every row added is what made the tab take seconds. */
    lv_obj_set_layout(s_list, LV_LAYOUT_NONE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    pp_music_refresh();
}
