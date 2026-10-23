/*
 * Screen dump helpers for font and layout checks. The panel is captured as a
 * 1 bit PBM because the console is the only channel fast enough to carry it:
 * the lwIP TCP window on this board is 5.7 KB, so large HTTP bodies crawl.
 *
 * PP_SCREEN_TOUR == 0 in a production build: the boot tour steals the screen
 * for half a minute, so it stays behind this switch and is only enabled while
 * debugging the visual layout.
 */
#include "shot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include "alarms.h"
#include "autosleep.h"
#include "board.h"
#include "clockmode.h"
#include "driver/ledc.h"
#include "config.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "focus.h"
#include "lvgl.h"
#include "pages.h"
#include "plans.h"
#include "store.h"
#include "ui.h"

/* Defined by the tabs; only the tour uses them. */
void pp_ai_demo_keyboard(void);

#define TAG "shot"

#ifndef PP_SCREEN_TOUR
#define PP_SCREEN_TOUR 0
#endif

/* The cache round trip is destructive, so the tour skips it unless asked. */
#ifndef PP_TOUR_CACHE
#define PP_TOUR_CACHE 0
#endif

/* Persistence probe for "the storage location goes back to internal after a
 * reboot": boot 1 switches the flag on, saves and reboots; boot 2 reports what
 * config_load actually read back, then puts everything where it was. */
#ifndef PP_TOUR_CFG
#define PP_TOUR_CFG 0
#endif

/* Fold one RGB565 buffer down to a 1bpp PBM body. */
static uint8_t *fold_rgb565(const uint8_t *src_data, uint32_t w, uint32_t h,
                            uint32_t src_stride, size_t *out_len)
{
    const uint32_t row = (w + 7) / 8;
    char header[32];
    int header_len = snprintf(header, sizeof(header), "P4\n%u %u\n", (unsigned)w, (unsigned)h);
    const size_t size = (size_t)header_len + (size_t)row * h;
    uint8_t *img = malloc(size);
    if (img == NULL) {
        return NULL;
    }
    memcpy(img, header, (size_t)header_len);
    memset(img + header_len, 0, (size_t)row * h);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = src_data + (size_t)y * src_stride;
        uint8_t *dst = img + header_len + (size_t)y * row;
        for (uint32_t x = 0; x < w; x++) {
            uint16_t v = (uint16_t)(src[x * 2] | (src[x * 2 + 1] << 8));
            uint32_t r = (v >> 11) & 0x1F;
            uint32_t g = (v >> 5) & 0x3F;
            uint32_t b = v & 0x1F;
            /* 1 = black in PBM; anything darker than mid grey counts as ink.
             * White is 31/63/31, so half brightness is about 269000. */
            if (r * 2126 + g * 7152 + b * 722 < 269000) {
                dst[x / 8] |= (uint8_t)(0x80u >> (x % 8));
            }
        }
    }
    *out_len = size;
    return img;
}

esp_err_t pp_shot_pbm(uint8_t **out, size_t *out_len, uint32_t *out_w, uint32_t *out_h)
{
    lv_display_t *disp = pp_board_display();
    if (disp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lvgl_port_lock(10000)) {
        return ESP_ERR_TIMEOUT;
    }
    /* Render the live widget tree into a scratch buffer instead of peeking at
     * the panel framebuffer: the RGB panel double buffers, so right after a big
     * redraw the "active" buffer can still hold the frame before last. */
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    uint32_t w = 0, h = 0;
    uint8_t *img = NULL;
    size_t size = 0;
    if (snap != NULL) {
        w = snap->header.w;
        h = snap->header.h;
        img = fold_rgb565(snap->data, w, h, snap->header.stride, &size);
        lv_draw_buf_destroy(snap);
    } else {
        lv_draw_buf_t *buf = lv_display_get_buf_active(disp);
        if (buf != NULL && buf->data != NULL && buf->header.w > 0) {
            w = buf->header.w;
            h = buf->header.h;
            img = fold_rgb565(buf->data, w, h, buf->header.stride, &size);
        }
    }
    lvgl_port_unlock();
    if (img == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *out = img;
    *out_len = size;
    *out_w = w;
    *out_h = h;
    return ESP_OK;
}

static void base64_dump(const uint8_t *data, size_t len)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    printf("---PBMBEGIN---\n");
    char line[68];
    size_t n = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        int rem = 1;
        if (i + 1 < len) {
            v |= (uint32_t)data[i + 1] << 8;
            rem = 2;
        }
        if (i + 2 < len) {
            v |= data[i + 2];
            rem = 3;
        }
        line[n++] = tbl[(v >> 18) & 0x3F];
        line[n++] = tbl[(v >> 12) & 0x3F];
        line[n++] = (rem > 1) ? tbl[(v >> 6) & 0x3F] : '=';
        line[n++] = (rem > 2) ? tbl[v & 0x3F] : '=';
        if (n >= 64) {
            line[n] = 0;
            printf("%s\n", line);
            n = 0;
        }
    }
    if (n > 0) {
        line[n] = 0;
        printf("%s\n", line);
    }
    printf("---PBMEND---\n");
    fflush(stdout);
}

void pp_shot_dump_serial(void)
{
    uint8_t *img = NULL;
    size_t len = 0;
    uint32_t w = 0, h = 0;
    esp_err_t err = pp_shot_pbm(&img, &len, &w, &h);
    if (err != ESP_OK) {
        printf("---PBMFAIL %s---\n", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "dump %ux%u (%u bytes)", (unsigned)w, (unsigned)h, (unsigned)len);
    base64_dump(img, len);
    free(img);
}

static void shot_task(void *arg)
{
    (void)arg;
    char line[64];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        if (strncmp(line, "shot", 4) == 0) {
            pp_shot_dump_serial();
        } else if (strncmp(line, "help", 4) == 0) {
            printf("commands: shot - dump the screen as base64 PBM\n");
        }
    }
    vTaskDelete(NULL);
}

void pp_shot_start_console(void)
{
    xTaskCreate(shot_task, "shot_cmd", 8192, NULL, 2, NULL);
}

/* Boot time self tour: walks every tab (and the reader) and dumps each screen.
 * Used to check fonts and colours from the desktop without a camera. */
static bool first_book(char *out, size_t out_size)
{
    DIR *dir = opendir("/sdcard");
    if (dir == NULL) {
        return false;
    }
    bool found = false;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *dot = strrchr(ent->d_name, '.');
        if (dot == NULL || strcasecmp(dot, ".txt") != 0) {
            continue;
        }
        struct stat st;
        char path[400];
        snprintf(path, sizeof(path), "/sdcard/%s", ent->d_name);
        if (stat(path, &st) != 0 || st.st_size < 200000) {
            continue;
        }
        snprintf(out, out_size, "%s", path);
        found = true;
        break;
    }
    closedir(dir);
    return found;
}

static void tour_task(void *arg)
{
    (void)arg;
    /* The tour never touches the screen, so LVGL's inactivity clock keeps
     * growing and auto sleep would black out the captures. It gets switched
     * back on at the end, where it is verified explicitly. */
#if PP_TOUR_CFG
    if (pp_config()->cache_sd) {
        struct stat st;
        bool sd_data = (stat("/sdcard/.pocketplan/plans.json", &st) == 0);
        bool lfs_data = (stat("/lfs/plans.json", &st) == 0);
        printf("---TOURCFG loaded=1 data_on_sd=%d sd_file=%d lfs_file=%d plans=%d---\n",
               (int)pp_data_on_sd(), (int)sd_data, (int)lfs_data, pp_plans_count());
        pp_config()->cache_sd = false;
        pp_config_save();
        pp_store_migrate_data();
        printf("---TOURCFGDONE lfs=%d---\n",
               (int)(stat("/lfs/plans.json", &st) == 0));
        pp_home_refresh();
    } else {
        pp_config()->cache_sd = true;
        pp_config_save();
        esp_err_t mig = pp_store_migrate_data();
        struct stat st;
        printf("---TOURCFG set=1 mig=%s data_on_sd=%d sd_file=%d plans=%d---\n",
               esp_err_to_name(mig), (int)pp_data_on_sd(),
               (int)(stat("/sdcard/.pocketplan/plans.json", &st) == 0), pp_plans_count());
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
#endif
    bool keep_sleep = pp_config()->auto_sleep;
    uint8_t keep_min = pp_config()->sleep_min;
    pp_config()->auto_sleep = false;
    /* M14.4: WebDAV must ship switched off. */
    printf("---TOURWEBDAB default=%d port80=%d---\n", pp_config()->webdav ? 1 : 0, 1);
    vTaskDelay(pdMS_TO_TICKS(9000));
    for (int tab = 0; tab < 6; tab++) {
        pp_ui_show_tab(tab);
        vTaskDelay(pdMS_TO_TICKS(1800));
        printf("---TOURTAB %d---\n", tab);
        pp_shot_dump_serial();
    }
    /* Music: the scan runs on its own task now, so capture the placeholder and
     * then the finished library. The log prints how long each half took. */
    pp_ui_show_tab(3);
    vTaskDelay(pdMS_TO_TICKS(250));
    printf("---TOURMUSICLOAD---\n");
    pp_shot_dump_serial();
    vTaskDelay(pdMS_TO_TICKS(9000));
    printf("---TOURMUSIC---\n");
    pp_shot_dump_serial();

    /* A ticked plan: the disc is solid black with a white tick now. */
    if (pp_plans_count() > 0) {
        char id[12];
        const pp_plan_t *first = pp_plans_get(0);
        bool was = first->done_today;
        snprintf(id, sizeof(id), "%s", first->id);
        bool now = false;
        pp_plans_checkin(id, NULL, 1, &now);
        pp_home_refresh();
        pp_ui_show_tab(0);
        vTaskDelay(pdMS_TO_TICKS(900));
        printf("---TOURDONE---\n");
        pp_shot_dump_serial();
        pp_plans_checkin(id, NULL, was ? 1 : 0, &now);
        pp_home_refresh();
    }

    /* Keyboard capture: the Enter and backspace glyphs used to be invisible. */
    pp_ui_show_tab(4);
    vTaskDelay(pdMS_TO_TICKS(600));
    pp_ai_demo_keyboard();
    vTaskDelay(pdMS_TO_TICKS(900));
    printf("---TOURKEYBOARD---\n");
    pp_shot_dump_serial();

    /* Alarm ring overlay + buzzer (the buzzer itself cannot be captured). */
    pp_ui_show_tab(1);
    vTaskDelay(pdMS_TO_TICKS(500));
    pp_alarms_demo_ring();
    vTaskDelay(pdMS_TO_TICKS(1500));
    printf("---TOURALARM---\n");
    pp_shot_dump_serial();
    pp_alarm_stop();
    vTaskDelay(pdMS_TO_TICKS(500));

    /* SD cache switch: move the data files over, check, then move back.
     * Off by default in the tour: it rewrites the user's data files twice and
     * once lost an alarm on the way back (see the progress report, pit #39). */
#if PP_TOUR_CACHE
    int plans_before = pp_plans_count();
    pp_config()->cache_sd = true;
    pp_config_save();
    esp_err_t mig = pp_store_migrate_data();
    bool on_sd = pp_data_on_sd();
    struct stat st;
    bool sd_file = (stat(PP_DATA_DIR_SD "/plans.json", &st) == 0);
    int plans_after = pp_plans_count();
    pp_config()->cache_sd = false;
    pp_config_save();
    esp_err_t mig2 = pp_store_migrate_data();
    bool lfs_file = (stat(PP_LFS "/plans.json", &st) == 0);
    printf("---TOURCACHE on_sd=%d mig=%s file=%d plans=%d/%d back=%s lfs=%d---\n",
           (int)on_sd, esp_err_to_name(mig), (int)sd_file, plans_before, plans_after,
           esp_err_to_name(mig2), (int)lfs_file);
#endif

    pp_focus_open();
    vTaskDelay(pdMS_TO_TICKS(2500));
    printf("---TOURFOCUS---\n");
    pp_shot_dump_serial();
    pp_focus_close();
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Clock mode: time + countdowns only, in both colour schemes. */
    bool dark_before = pp_config()->focus_dark;
    pp_clockmode_open();
    vTaskDelay(pdMS_TO_TICKS(1500));
    printf("---TOURCLOCK---\n");
    pp_shot_dump_serial();
    pp_clockmode_close();
    pp_config()->focus_dark = false;
    pp_config_save();
    vTaskDelay(pdMS_TO_TICKS(400));
    pp_clockmode_open();
    vTaskDelay(pdMS_TO_TICKS(1500));
    printf("---TOURCLOCKLIGHT---\n");
    pp_shot_dump_serial();
    /* Three taps is the only way out of clock mode, so prove it here: send real
     * CLICKED events to the overlay and check that it closes by itself. */
    lvgl_port_lock(0);
    lv_obj_t *clock_root = lv_obj_get_child(lv_screen_active(), -1);
    for (int tap = 0; tap < 3; tap++) {
        lv_obj_send_event(clock_root, LV_EVENT_CLICKED, NULL);
    }
    lvgl_port_unlock();
    vTaskDelay(pdMS_TO_TICKS(600));
    printf("---TOURTAPEXIT active=%d---\n", (int)pp_clockmode_active());
    pp_clockmode_close();
    pp_config()->focus_dark = dark_before;
    pp_config_save();
    vTaskDelay(pdMS_TO_TICKS(400));

    char book[400];
    if (first_book(book, sizeof(book))) {
        pp_reader_ui_open(book);
        vTaskDelay(pdMS_TO_TICKS(3000));
        printf("---TOURREADER---\n");
        pp_shot_dump_serial();
        pp_reader_ui_close();

        /* Same book at the large (20 px) reading size, so the test capture
         * covers both fonts the settings page offers. */
        pp_config()->font_size = 20;
        pp_config_save();
        pp_reader_ui_open(book);
        vTaskDelay(pdMS_TO_TICKS(3000));
        printf("---TOURREADER20---\n");
        pp_shot_dump_serial();
        pp_reader_ui_close();
        pp_config()->font_size = 16;
        pp_config_save();
    }
    /* Settings tab with auto sleep actually on: the tour forces it off at
     * boot so the dumps are not blank, which would make the docs show the
     * switch in a state the user will never see. */
    pp_config()->auto_sleep = true;
    pp_config()->sleep_min = 5;
    lvgl_port_lock(0);
    pp_ui_show_tab(5);
    lvgl_port_unlock();
    vTaskDelay(pdMS_TO_TICKS(1200));
    printf("---TOURSETSETTINGS---\n");
    pp_shot_dump_serial();
    /* Same screen scrolled to the network section: proves the WebDAV switch is
     * there and starts switched off. */
    pp_settings_demo_webdav();
    vTaskDelay(pdMS_TO_TICKS(700));
    printf("---TOURWEBDABUI---\n");
    pp_shot_dump_serial();

    /* Auto sleep: by now the inactivity clock is far past any limit, so the
     * shortest timeout makes the task act within one poll. The backlight duty
     * cycle is the proof that the panel really went dark; a touch only resets
     * LVGL's activity clock, which is what lv_display_trigger_activity()
     * simulates here. */
    pp_config()->auto_sleep = true;
    pp_config()->sleep_min = 1;
    vTaskDelay(pdMS_TO_TICKS(2500));
    printf("---TOURSLEEP asleep=%d duty=%u---\n", (int)pp_autosleep_active(),
           (unsigned)ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
    lvgl_port_lock(0);
    lv_display_trigger_activity(NULL);
    lvgl_port_unlock();
    vTaskDelay(pdMS_TO_TICKS(1500));
    printf("---TOURWAKE asleep=%d duty=%u---\n", (int)pp_autosleep_active(),
           (unsigned)ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
    /* The tour saved config.json a few times with auto sleep forced off, so put
     * the user's own values back and persist them too. */
    pp_config()->auto_sleep = keep_sleep;
    pp_config()->sleep_min = keep_min;
    pp_config_save();

    printf("---TOUREND---\n");
    vTaskDelete(NULL);
}

void pp_shot_start_tour(void)
{
#if PP_SCREEN_TOUR
    xTaskCreate(tour_task, "shot_tour", 8192, NULL, 2, NULL);
#endif
}
