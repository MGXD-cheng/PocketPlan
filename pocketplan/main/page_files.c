/* SD card browser: tap a folder to enter, tap a text file to read it. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "pages.h"
#include "ui.h"

#define C_BG 0xFFFFFF
#define C_CARD 0xFFFFFF
#define C_LINE 0xD0D0D0
#define C_TEXT 0x000000
#define C_DIM 0x000000
#define C_ACCENT 0x000000

/* Solid page icon (Font Awesome fa-file-alt, U+F15C). The stock LV_SYMBOL_FILE
 * is the outlined glyph, which at 16 px 1bpp reads as an empty box and looks
 * like a missing font. */
#define SYMBOL_FILE_SOLID "\xEF\x85\x9C"

#define PATH_MAX_LEN 400
#define MAX_ENTRIES 400
/* LVGL widget creation cost grows super-linearly with the number of siblings,
 * so a 120-entry folder built in one go blocks the UI for seconds. Paging
 * keeps each build at a few hundred milliseconds. */
#define PAGE_SIZE 24

typedef struct {
    char *name;
    char *path;
    unsigned long long size;
    bool dir;
} entry_t;

static const char *TAG = "page_files";

static lv_obj_t *s_root;
static lv_obj_t *s_list;
static lv_obj_t *s_path;
static lv_obj_t *s_page_label;
static int s_page;
static entry_t s_entries[MAX_ENTRIES];
static int s_count;
static char s_cwd[PATH_MAX_LEN] = "/sdcard";

static bool is_text_file(const char *name)
{
    static const char *const exts[] = {".txt", ".md", ".markdown", ".log", ".csv",
                                       ".json", ".ini", ".conf", ".c", ".h", ".py"};
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        if (strcasecmp(dot, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

static const char *file_icon(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return SYMBOL_FILE_SOLID;
    }
    if (strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".flac") == 0 ||
        strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".m4a") == 0 ||
        strcasecmp(dot, ".aac") == 0) {
        return LV_SYMBOL_AUDIO;
    }
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0 ||
        strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".bmp") == 0) {
        return LV_SYMBOL_IMAGE;
    }
    if (strcasecmp(dot, ".mp4") == 0 || strcasecmp(dot, ".avi") == 0 ||
        strcasecmp(dot, ".mkv") == 0) {
        return LV_SYMBOL_VIDEO;
    }
    return SYMBOL_FILE_SOLID;
}

static void format_size(unsigned long long size, char *out, size_t out_size)
{
    if (size >= 1024ULL * 1024ULL) {
        snprintf(out, out_size, "%.1f MB", (double)size / (1024.0 * 1024.0));
    } else if (size >= 1024ULL) {
        snprintf(out, out_size, "%.0f KB", (double)size / 1024.0);
    } else {
        snprintf(out, out_size, "%llu B", size);
    }
}

static void free_entries(void)
{
    for (int i = 0; i < s_count; i++) {
        free(s_entries[i].name);
        free(s_entries[i].path);
    }
    s_count = 0;
}

static int entry_cmp(const void *a, const void *b)
{
    const entry_t *ea = (const entry_t *)a;
    const entry_t *eb = (const entry_t *)b;
    if (ea->dir != eb->dir) {
        return ea->dir ? -1 : 1;
    }
    return strcasecmp(ea->name, eb->name);
}

static void scan_dir(const char *path)
{
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && s_count < MAX_ENTRIES) {
        if (de->d_name[0] == '.') {
            continue;
        }
        char full[PATH_MAX_LEN];
        snprintf(full, sizeof(full), "%s/%s", strcmp(path, "/") == 0 ? "" : path, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        s_entries[s_count].name = strdup(de->d_name);
        s_entries[s_count].path = strdup(full);
        s_entries[s_count].size = (unsigned long long)st.st_size;
        s_entries[s_count].dir = S_ISDIR(st.st_mode);
        if (s_entries[s_count].name && s_entries[s_count].path) {
            s_count++;
        } else {
            free(s_entries[s_count].name);
            free(s_entries[s_count].path);
        }
    }
    closedir(dir);
    qsort(s_entries, s_count, sizeof(entry_t), entry_cmp);
}

static void row_clicked(lv_event_t *e)
{
    entry_t *entry = (entry_t *)lv_event_get_user_data(e);
    if (entry == NULL) {
        return;
    }
    if (entry->dir) {
        snprintf(s_cwd, sizeof(s_cwd), "%s", entry->path);
        s_page = 0;
        pp_files_refresh();
        return;
    }
    if (!is_text_file(entry->name)) {
        pp_ui_toast("暂不支持打开该文件");
        return;
    }
    pp_reader_ui_open(entry->path);
}

static void up_clicked(lv_event_t *e)
{
    (void)e;
    if (strcmp(s_cwd, "/sdcard") == 0) {
        return;
    }
    char *slash = strrchr(s_cwd, '/');
    if (slash != NULL && slash > s_cwd + 6) {
        *slash = '\0';
    } else {
        snprintf(s_cwd, sizeof(s_cwd), "/sdcard");
    }
    pp_files_refresh();
}

static void page_prev(lv_event_t *e)
{
    (void)e;
    if (s_page > 0) {
        s_page--;
        pp_files_refresh();
    }
}

static void page_next(lv_event_t *e)
{
    (void)e;
    int pages = (s_count + PAGE_SIZE - 1) / PAGE_SIZE;
    if (s_page + 1 < pages) {
        s_page++;
        pp_files_refresh();
    }
}

static void add_row(entry_t *entry, int index)
{
    /* Absolute positioning instead of a flex container: rows are uniform, and
     * this keeps widget creation free of layout passes. */
    lv_obj_t *row = lv_obj_create(s_list);

    lv_obj_set_size(row, 452, 52);
    lv_obj_set_pos(row, 14, 8 + index * 58);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, entry);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, entry->dir ? LV_SYMBOL_DIRECTORY : file_icon(entry->name));
    lv_obj_set_style_text_color(icon, lv_color_hex(0x000000), 0);
    lv_obj_set_pos(icon, 10, 17);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, entry->name);
    lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT), 0);
    lv_obj_set_width(name, 320);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(name, 36, 17);

    if (!entry->dir) {
        char size[24];
        format_size(entry->size, size, sizeof(size));
        lv_obj_t *meta = lv_label_create(row);
        lv_label_set_text(meta, size);
        lv_obj_set_style_text_color(meta, lv_color_hex(C_DIM), 0);
        lv_obj_set_width(meta, 70);
        lv_obj_set_style_text_align(meta, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(meta, 368, 17);
    }
}

void pp_files_refresh(void)
{
    if (s_root == NULL) {
        return;
    }
    int64_t t0 = esp_timer_get_time();
    lvgl_port_lock(0);
    lv_obj_clean(s_list);
    free_entries();
    scan_dir(s_cwd);
    int64_t t1 = esp_timer_get_time();
    lv_label_set_text(s_path, s_cwd);
    int pages = (s_count + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < 1) {
        pages = 1;
    }
    if (s_page >= pages) {
        s_page = pages - 1;
    }
    if (s_page < 0) {
        s_page = 0;
    }
    if (s_page_label != NULL) {
        lv_label_set_text_fmt(s_page_label, "%d/%d", s_page + 1, pages);
    }
    int first = s_page * PAGE_SIZE;
    for (int i = first; i < s_count && i < first + PAGE_SIZE; i++) {
        add_row(&s_entries[i], i - first);
    }
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    if (s_count == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, "空文件夹\n\n用网页端上传文件到 SD 卡");
        lv_obj_set_style_text_color(empty, lv_color_hex(C_DIM), 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty, lv_pct(100));
        lv_obj_set_style_pad_top(empty, 60, 0);
    }
    lvgl_port_unlock();
    ESP_LOGI(TAG, "%s: %d entries, page %d, scan %lld ms, rows %lld ms", s_cwd, s_count,
             s_page, (long long)(t1 - t0) / 1000, (long long)(esp_timer_get_time() - t1) / 1000);
}

void pp_files_create(lv_obj_t *parent)
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
    lv_obj_set_height(bar, 48);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_left(bar, 12, 0);
    lv_obj_set_style_pad_right(bar, 12, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *up = lv_button_create(bar);
    lv_obj_set_size(up, 60, 36);
    lv_obj_set_style_radius(up, 8, 0);
    lv_obj_set_style_bg_color(up, lv_color_hex(0xE4E4E4), 0);
    lv_obj_set_style_shadow_width(up, 0, 0);
    lv_obj_add_event_cb(up, up_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *up_label = lv_label_create(up);
    lv_label_set_text(up_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(up_label, lv_color_hex(C_ACCENT), 0);
    lv_obj_center(up_label);

    s_path = lv_label_create(bar);
    lv_label_set_text(s_path, s_cwd);
    lv_obj_set_style_text_color(s_path, lv_color_hex(C_DIM), 0);
    lv_obj_set_flex_grow(s_path, 1);
    lv_label_set_long_mode(s_path, LV_LABEL_LONG_DOT);

    s_page_label = lv_label_create(bar);
    lv_label_set_text(s_page_label, "1/1");
    lv_obj_set_style_text_color(s_page_label, lv_color_hex(C_DIM), 0);

    lv_obj_t *prev = lv_button_create(bar);
    lv_obj_set_size(prev, 34, 36);
    lv_obj_set_style_radius(prev, 8, 0);
    lv_obj_set_style_bg_color(prev, lv_color_hex(0xE4E4E4), 0);
    lv_obj_set_style_shadow_width(prev, 0, 0);
    lv_obj_set_style_pad_all(prev, 0, 0);
    lv_obj_add_event_cb(prev, page_prev, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = lv_label_create(prev);
    lv_label_set_text(pl, LV_SYMBOL_UP);
    lv_obj_set_style_text_color(pl, lv_color_hex(C_ACCENT), 0);
    lv_obj_center(pl);

    lv_obj_t *next = lv_button_create(bar);
    lv_obj_set_size(next, 34, 36);
    lv_obj_set_style_radius(next, 8, 0);
    lv_obj_set_style_bg_color(next, lv_color_hex(0xE4E4E4), 0);
    lv_obj_set_style_shadow_width(next, 0, 0);
    lv_obj_set_style_pad_all(next, 0, 0);
    lv_obj_add_event_cb(next, page_next, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(next);
    lv_label_set_text(nl, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_color(nl, lv_color_hex(C_ACCENT), 0);
    lv_obj_center(nl);

    s_list = lv_obj_create(s_root);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_left(s_list, 12, 0);
    lv_obj_set_style_pad_right(s_list, 12, 0);
    lv_obj_set_style_pad_top(s_list, 8, 0);
    lv_obj_set_style_pad_bottom(s_list, 16, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    pp_files_refresh();
}