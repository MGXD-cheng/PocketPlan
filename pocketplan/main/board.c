/*
 * Board support for the Waveshare ESP32-S3-Touch-LCD-2.8B:
 *   I2C bus, TCA9554PWR IO expander, ST7701S 480x640 RGB panel (3-wire SPI
 *   init), GT911 touch and the SDMMC 1-bit TF card slot.
 */
#include "board.h"

#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_st7701.h"
#include "esp_io_expander_tca9554.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c_bus;
static esp_io_expander_handle_t s_expander;
static lv_display_t *s_disp;
static bool s_sd_mounted;
static sdmmc_card_t *s_card;

/* ------------------------------------------------------------------ */
/* Panel initialisation sequence, copied verbatim from the vendor demo. */
/* ------------------------------------------------------------------ */
static const st7701_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},

    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x4F, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x10, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x07, 0x02}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},

    {0xB0, (uint8_t[]){0x00, 0x10, 0x17, 0x0D, 0x11, 0x06, 0x05, 0x08, 0x07, 0x1F, 0x04, 0x11, 0x0E, 0x29, 0x30, 0x1F}, 16, 0},
    {0xB1, (uint8_t[]){0x00, 0x0D, 0x14, 0x0E, 0x11, 0x06, 0x04, 0x08, 0x08, 0x20, 0x05, 0x13, 0x13, 0x26, 0x30, 0x1F}, 16, 0},

    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x65}, 1, 0},
    {0xB1, (uint8_t[]){0x71}, 1, 0},
    {0xB2, (uint8_t[]){0x82}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x42}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x20}, 1, 0},
    {0xC0, (uint8_t[]){0x09}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},
    {0xEE, (uint8_t[]){0x42}, 1, 0},

    {0xE0, (uint8_t[]){0x00, 0x00, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x04, 0xA0, 0x06, 0xA0, 0x05, 0xA0, 0x07, 0xA0, 0x00, 0x44, 0x44}, 11, 0},
    {0xE2, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 12, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x22, 0x22}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x0C, 0x90, 0xA0, 0xA0, 0x0E, 0x92, 0xA0, 0xA0, 0x08, 0x8C, 0xA0, 0xA0, 0x0A, 0x8E, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x22, 0x22}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x0D, 0x91, 0xA0, 0xA0, 0x0F, 0x93, 0xA0, 0xA0, 0x09, 0x8D, 0xA0, 0xA0, 0x0B, 0x8F, 0xA0, 0xA0}, 16, 0},

    {0x36, (uint8_t[]){0x00}, 1, 0},
    {0xEB, (uint8_t[]){0x00, 0x00, 0xE4, 0xE4, 0x44, 0x00, 0x40}, 7, 0},
    {0xED, (uint8_t[]){0xFF, 0xF5, 0x47, 0x6F, 0x0B, 0xA1, 0xAB, 0xFF, 0xFF, 0xBA, 0x1A, 0xB0, 0xF6, 0x74, 0x5F, 0xFF}, 16, 0},
    {0xEF, (uint8_t[]){0x08, 0x08, 0x08, 0x40, 0x3F, 0x64}, 6, 0},

    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xE6, (uint8_t[]){0x16, 0x7C}, 2, 0},
    {0xE8, (uint8_t[]){0x00, 0x0E}, 2, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},

    {0x11, (uint8_t[]){0x00}, 0, 200},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xE8, (uint8_t[]){0x00, 0x0C}, 2, 150},
    {0xE8, (uint8_t[]){0x00, 0x00}, 2, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x29, (uint8_t[]){0x00}, 0, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},

    {0x11, (uint8_t[]){0x00}, 0, 200},
    {0x29, (uint8_t[]){0x00}, 0, 0},
    {0x29, (uint8_t[]){0x00}, 0, 100},
};

/* ------------------------------- backlight ------------------------------ */
void pp_board_backlight(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (1023 * percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void pp_board_buzzer(bool on)
{
    if (s_expander == NULL) {
        return;
    }
    esp_io_expander_set_level(s_expander, PP_EXIO_BUZZER, on ? 1 : 0);
}

bool pp_board_buzzer_ready(void)
{
    return s_expander != NULL;
}

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        return err;
    }
    ledc_channel_config_t channel = {
        .gpio_num = PP_BACKLIGHT_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&channel);
}

/* --------------------------------- I2C --------------------------------- */
static esp_err_t i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PP_I2C_SDA_PIN,
        .scl_io_num = PP_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

static esp_err_t expander_init(void)
{
    ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_tca9554(s_i2c_bus, PP_I2C_ADDR_TCA9554, &s_expander),
                        TAG, "tca9554 not found");    /* The TCA9554 driver refuses to set a level on a pin that is an input, so
     * change the direction first and only drive the outputs afterwards. */
    uint32_t outputs = PP_EXIO_LCD_RST | PP_EXIO_TP_RST | PP_EXIO_LCD_CS | PP_EXIO_SD_CS | PP_EXIO_BUZZER;
    uint32_t inputs = PP_EXIO_IMU_INT1 | PP_EXIO_IMU_INT2 | PP_EXIO_RTC_INT;
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_expander, outputs, IO_EXPANDER_OUTPUT), TAG, "dir out");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_expander, inputs, IO_EXPANDER_INPUT), TAG, "dir in");

    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, PP_EXIO_BUZZER, 0), TAG, "buzzer");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, PP_EXIO_LCD_CS, 1), TAG, "lcd cs");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, PP_EXIO_SD_CS, 1), TAG, "sd cs");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, PP_EXIO_TP_RST, 1), TAG, "tp rst");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, PP_EXIO_LCD_RST, 1), TAG, "lcd rst");
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

/* -------------------------------- display ------------------------------- */
static esp_err_t display_init(void)
{
    /* hardware reset through the expander */
    esp_io_expander_set_level(s_expander, PP_EXIO_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_io_expander_set_level(s_expander, PP_EXIO_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    spi_line_config_t line = {
        .cs_io_type = IO_TYPE_EXPANDER,
        .cs_expander_pin = PP_LCD_SPI_CS,
        .scl_io_type = IO_TYPE_GPIO,
        .scl_gpio_num = PP_LCD_SPI_SCL,
        .sda_io_type = IO_TYPE_GPIO,
        .sda_gpio_num = PP_LCD_SPI_SDA,
        .io_expander = s_expander,
    };
    esp_lcd_panel_io_3wire_spi_config_t io_cfg = ST7701_PANEL_IO_3WIRE_SPI_CONFIG(line, 0);
    esp_lcd_panel_io_handle_t panel_io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_3wire_spi(&io_cfg, &panel_io), TAG, "3wire spi");

    esp_lcd_rgb_panel_config_t rgb = {
        .clk_src = LCD_CLK_SRC_PLL240M,
        .timings = {
            .pclk_hz = PP_LCD_PCLK_HZ,
            .h_res = PP_LCD_H_RES,
            .v_res = PP_LCD_V_RES,
            .hsync_pulse_width = PP_LCD_HPW,
            .hsync_back_porch = PP_LCD_HBP,
            .hsync_front_porch = PP_LCD_HFP,
            .vsync_pulse_width = PP_LCD_VPW,
            .vsync_back_porch = PP_LCD_VBP,
            .vsync_front_porch = PP_LCD_VFP,
            .flags = {
                .pclk_active_neg = false,
            },
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 2,
        .bounce_buffer_size_px = PP_LCD_H_RES * 20,
        .dma_burst_size = 64,
        .hsync_gpio_num = PP_LCD_HSYNC,
        .vsync_gpio_num = PP_LCD_VSYNC,
        .de_gpio_num = PP_LCD_DE,
        .pclk_gpio_num = PP_LCD_PCLK,
        .disp_gpio_num = PP_LCD_DISP,
        .data_gpio_nums = {
            PP_LCD_DATA0, PP_LCD_DATA1, PP_LCD_DATA2, PP_LCD_DATA3,
            PP_LCD_DATA4, PP_LCD_DATA5, PP_LCD_DATA6, PP_LCD_DATA7,
            PP_LCD_DATA8, PP_LCD_DATA9, PP_LCD_DATA10, PP_LCD_DATA11,
            PP_LCD_DATA12, PP_LCD_DATA13, PP_LCD_DATA14, PP_LCD_DATA15,
        },
        .flags = {
            .fb_in_psram = 1,
        },
    };

    st7701_vendor_config_t vendor = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .rgb_config = &rgb,
        .flags = {
            .mirror_by_cmd = 0,
            .auto_del_panel_io = 1,
        },
    };

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };

    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7701(panel_io, &panel_cfg, &panel), TAG, "st7701");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");

    /* clear the panel so the first LVGL frame does not show power-on garbage */
    static uint16_t blank_line[PP_LCD_H_RES];
    for (int x = 0; x < PP_LCD_H_RES; x++) {
        blank_line[x] = 0xFFFF;
    }
    for (int y = 0; y < PP_LCD_V_RES; y++) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, PP_LCD_H_RES, y + 1, blank_line);
    }

    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    port_cfg.timer_period_ms = 10;
    port_cfg.task_stack = 8192;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .buffer_size = (uint32_t)(PP_LCD_H_RES * 20),
        .double_buffer = true,
        .hres = PP_LCD_H_RES,
        .vres = PP_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 0,
            .full_refresh = 1,
            .direct_mode = 1,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        },
    };
    s_disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    return s_disp ? ESP_OK : ESP_FAIL;
}

/* --------------------------------- touch -------------------------------- */
static esp_err_t touch_init(void)
{
    /* GT911 latches its I2C address from the INT level while RST is released. */
    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << PP_TP_INT_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_cfg);
    gpio_set_level(PP_TP_INT_PIN, 0);

    esp_io_expander_set_level(s_expander, PP_EXIO_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_io_expander_set_level(s_expander, PP_EXIO_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(PP_TP_INT_PIN, 1);
    int_cfg.mode = GPIO_MODE_INPUT;
    gpio_config(&int_cfg);
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    if (i2c_master_probe(s_i2c_bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 100) != ESP_OK) {
        if (i2c_master_probe(s_i2c_bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 100) == ESP_OK) {
            addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        } else {
            ESP_LOGW(TAG, "GT911 not answering, touch disabled");
            return ESP_ERR_NOT_FOUND;
        }
    }
    ESP_LOGI(TAG, "GT911 at 0x%02X", addr);

    esp_lcd_panel_io_i2c_config_t tp_io_cfg = {
        .dev_addr = addr,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .flags = {
            .disable_control_phase = 1,
        },
    };
    tp_io_cfg.scl_speed_hz = 400 * 1000;
    esp_lcd_panel_io_handle_t tp_io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io), TAG, "tp io");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = PP_LCD_H_RES - 1,
        .y_max = PP_LCD_V_RES - 1,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    esp_lcd_touch_handle_t tp = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp), TAG, "gt911");

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = s_disp,
        .handle = tp,
    };
    lvgl_port_add_touch(&touch_cfg);
    return ESP_OK;
}

/* ---------------------------------- SD ---------------------------------- */
esp_err_t pp_board_sd_init(void)
{
    if (s_sd_mounted) {
        return ESP_OK;
    }
    /* SD_D3 doubles as the card-select line of the 1-bit interface. */
    esp_io_expander_set_level(s_expander, PP_EXIO_SD_CS, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = PP_SD_CLK;
    slot.cmd = PP_SD_CMD;
    slot.d0 = PP_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    /* Some cards need a second, slower attempt after power up. */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s), retrying at 400 kHz", esp_err_to_name(err));
        host.max_freq_khz = SDMMC_FREQ_PROBING;
        err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount_cfg, &s_card);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_sd_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

bool pp_board_sd_mounted(void)
{
    return s_sd_mounted;
}

void pp_board_sd_usage(uint32_t *total_kb, uint32_t *free_kb)
{
    uint64_t total = 0, freeb = 0;
    if (total_kb) {
        *total_kb = 0;
    }
    if (free_kb) {
        *free_kb = 0;
    }
    if (!s_sd_mounted) {
        return;
    }
    if (esp_vfs_fat_info("/sdcard", &total, &freeb) != ESP_OK) {
        return;
    }
    if (total_kb) {
        *total_kb = (uint32_t)(total / 1024);
    }
    if (free_kb) {
        *free_kb = (uint32_t)(freeb / 1024);
    }
}

lv_display_t *pp_board_display(void)
{
    return s_disp;
}

/* --------------------------------- boot --------------------------------- */
esp_err_t pp_board_init(void)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");
    pp_board_backlight(0);
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(expander_init(), TAG, "expander");
    ESP_RETURN_ON_ERROR(display_init(), TAG, "display");
    esp_err_t touch_err = touch_init();
    if (touch_err != ESP_OK && touch_err != ESP_ERR_NOT_FOUND) {
        return touch_err;
    }
    pp_board_backlight(80);
    return ESP_OK;
}
i2c_master_bus_handle_t pp_board_i2c_bus(void)
{
    return s_i2c_bus;
}