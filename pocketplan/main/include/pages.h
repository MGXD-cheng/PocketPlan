#pragma once

#include "lvgl.h"

/* Each tab owns one root object created under the UI content area. */
void pp_home_create(lv_obj_t *parent);
void pp_home_refresh(void);
void pp_home_tick(void);

void pp_countdown_create(lv_obj_t *parent);
void pp_countdown_refresh(void);

void pp_files_create(lv_obj_t *parent);
void pp_files_refresh(void);

void pp_music_create(lv_obj_t *parent);
void pp_music_refresh(void);

void pp_ai_create(lv_obj_t *parent);
void pp_ai_refresh(void);

void pp_settings_create(lv_obj_t *parent);
void pp_settings_refresh(void);
/* Tour hook: scroll the WebDAV switch into view so a capture can show it. */
void pp_settings_demo_webdav(void);

/* Full-screen reader overlay. */
void pp_reader_ui_init(void);
void pp_reader_ui_open(const char *path);
bool pp_reader_ui_open_now(void);
void pp_reader_ui_close(void);
