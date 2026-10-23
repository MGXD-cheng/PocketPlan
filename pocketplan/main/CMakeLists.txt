idf_component_register(
    SRCS
        "app_main.c"
        "ai.c"
        "alarms.c"
        "autosleep.c"
        "board.c"
        "clock.c"
        "clockmode.c"
        "config.c"
        "countdowns.c"
        "shot.c"
        "fonts/pp_font_16.c"
        "fonts/pp_font_20.c"
        "fonts/pp_font_32.c"
        "fonts/pp_font_96.c"
        "gbk.c"
        "page_ai.c"
        "page_countdown.c"
        "page_files.c"
        "page_home.c"
        "page_focus.c"
        "page_music.c"
        "page_reader.c"
        "page_settings.c"
        "plans.c"
        "reader.c"
        "store.c"
        "ui.c"
        "webapi.c"
        "webdav.c"
        "wifi.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_lcd
        esp_lcd_st7701
        esp_lcd_panel_io_additions
        esp_lcd_touch
        esp_lcd_touch_gt911
        esp_io_expander
        esp_io_expander_tca9554
        esp_lvgl_port
        lvgl__lvgl
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_spi
        esp_driver_sdmmc
        sdmmc
        fatfs
        vfs
        nvs_flash
        esp_wifi
        esp_netif
        esp_http_server
        esp_event
        esp_timer
        lwip
        app_update
        cjson
        spiffs
        esp_http_client
        esp-tls
        mbedtls
)

# Paths are built with snprintf into fixed buffers and truncation is harmless
# (the longest possible card path is far below PATHBUF); keep -Werror useful for
# everything else.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)

# The web app is embedded as a binary blob: the HTML is far too big to inline
# as a C string, and this keeps it editable as a normal file.
target_add_binary_data(${COMPONENT_LIB} "www/index.html" BINARY)
