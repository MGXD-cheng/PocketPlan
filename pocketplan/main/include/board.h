#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>

#include "lvgl.h"

/* Waveshare ESP32-S3-Touch-LCD-2.8B board support package. */

#define PP_LCD_H_RES 480
#define PP_LCD_V_RES 640

#define PP_I2C_SDA_PIN  GPIO_NUM_15
#define PP_I2C_SCL_PIN  GPIO_NUM_7
#define PP_I2C_ADDR_TCA9554 0x20

#define PP_TP_INT_PIN   GPIO_NUM_16

/* TCA9554 pins (EXIOx is P(x-1) on the chip).
 * IMPORTANT: esp_io_expander_set_dir()/set_level() take a bit MASK, not a pin
 * index, so every EXIO constant below is already left-shifted. */
#define PP_EXIO_LCD_RST  (1u << 0)
#define PP_EXIO_TP_RST   (1u << 1)
#define PP_EXIO_LCD_CS   (1u << 2)
#define PP_EXIO_SD_CS    (1u << 3)
#define PP_EXIO_IMU_INT1 (1u << 4)
#define PP_EXIO_IMU_INT2 (1u << 5)
#define PP_EXIO_RTC_INT  (1u << 6)
#define PP_EXIO_BUZZER   (1u << 7)

/* RGB panel signals */
#define PP_LCD_VSYNC   GPIO_NUM_39
#define PP_LCD_HSYNC   GPIO_NUM_38
#define PP_LCD_DE      GPIO_NUM_40
#define PP_LCD_PCLK    GPIO_NUM_41
#define PP_LCD_DISP    GPIO_NUM_NC

#define PP_LCD_DATA0   GPIO_NUM_5
#define PP_LCD_DATA1   GPIO_NUM_45
#define PP_LCD_DATA2   GPIO_NUM_48
#define PP_LCD_DATA3   GPIO_NUM_47
#define PP_LCD_DATA4   GPIO_NUM_21
#define PP_LCD_DATA5   GPIO_NUM_14
#define PP_LCD_DATA6   GPIO_NUM_13
#define PP_LCD_DATA7   GPIO_NUM_12
#define PP_LCD_DATA8   GPIO_NUM_11
#define PP_LCD_DATA9   GPIO_NUM_10
#define PP_LCD_DATA10  GPIO_NUM_9
#define PP_LCD_DATA11  GPIO_NUM_46
#define PP_LCD_DATA12  GPIO_NUM_3
#define PP_LCD_DATA13  GPIO_NUM_8
#define PP_LCD_DATA14  GPIO_NUM_18
#define PP_LCD_DATA15  GPIO_NUM_17

/* 3-wire SPI for the ST7701S: only CS lives on the expander */
#define PP_LCD_SPI_CS   PP_EXIO_LCD_CS
#define PP_LCD_SPI_SCL  GPIO_NUM_2
#define PP_LCD_SPI_SDA  GPIO_NUM_1

#define PP_LCD_PCLK_HZ  (30 * 1000 * 1000)
#define PP_LCD_HPW 10
#define PP_LCD_HBP 70
#define PP_LCD_HFP 60
#define PP_LCD_VPW 10
#define PP_LCD_VBP 20
#define PP_LCD_VFP 20

#define PP_BACKLIGHT_PIN GPIO_NUM_6

/* TF card, SDMMC 1-bit */
#define PP_SD_CLK GPIO_NUM_2
#define PP_SD_CMD GPIO_NUM_1
#define PP_SD_D0  GPIO_NUM_42

esp_err_t pp_board_init(void);
esp_err_t pp_board_sd_init(void);
bool      pp_board_sd_mounted(void);
void      pp_board_sd_usage(uint32_t *total_kb, uint32_t *free_kb);
void      pp_board_backlight(int percent);

/* The buzzer hangs off TCA9554 P7, so it can only be switched on or off:
 * there is no frequency control. */
void      pp_board_buzzer(bool on);
bool      pp_board_buzzer_ready(void);

/* Shared I2C master bus (GT911, TCA9554, PCF85063, QMI8658). */
i2c_master_bus_handle_t pp_board_i2c_bus(void);

lv_display_t *pp_board_display(void);
