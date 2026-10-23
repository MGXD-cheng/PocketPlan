/*
 * Clock: the ESP32 has no battery-backed time, the board has a PCF85063 at
 * 0x51. On boot the system time is restored from the RTC, and after the first
 * SNTP sync the RTC is written back so the next offline boot is correct too.
 */
#include "clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "config.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "clock";

#define RTC_ADDR 0x51

static i2c_master_dev_handle_t s_rtc;
static bool s_rtc_probed;
static bool s_sntp_started;

static uint8_t bcd2bin(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

static uint8_t bin2bcd(uint8_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static esp_err_t rtc_open(void)
{
    if (s_rtc) {
        return ESP_OK;
    }
    if (s_rtc_probed) {
        return ESP_ERR_NOT_FOUND;
    }
    s_rtc_probed = true;
    i2c_master_bus_handle_t bus = pp_board_i2c_bus();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &dev, &s_rtc) != ESP_OK) {
        return ESP_FAIL;
    }
    if (i2c_master_probe(bus, RTC_ADDR, 100) != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 not answering at 0x%02X", RTC_ADDR);
        i2c_master_bus_rm_device(s_rtc);
        s_rtc = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t pp_clock_read_rtc(struct tm *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = rtc_open();
    if (err != ESP_OK) {
        return err;
    }
    uint8_t reg = 0x04;
    uint8_t b[7];
    err = i2c_master_transmit_receive(s_rtc, &reg, 1, b, sizeof(b), 200);
    if (err != ESP_OK) {
        return err;
    }
    memset(out, 0, sizeof(*out));
    out->tm_sec = bcd2bin(b[0] & 0x7F);
    out->tm_min = bcd2bin(b[1] & 0x7F);
    out->tm_hour = bcd2bin(b[2] & 0x3F);
    out->tm_mday = bcd2bin(b[3] & 0x3F);
    out->tm_wday = b[4] & 0x07;
    out->tm_mon = bcd2bin(b[5] & 0x1F) - 1;
    out->tm_year = bcd2bin(b[6]) + 100; /* 2000 + year */
    if (out->tm_year < 124 || out->tm_mon < 0 || out->tm_mon > 11 || out->tm_mday < 1 ||
        out->tm_mday > 31 || out->tm_hour > 23 || out->tm_min > 59 || out->tm_sec > 59) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t pp_clock_write_rtc(const struct tm *in)
{
    if (in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = rtc_open();
    if (err != ESP_OK) {
        return err;
    }
    uint8_t buf[8];
    buf[0] = 0x04;
    buf[1] = bin2bcd((uint8_t)in->tm_sec);
    buf[2] = bin2bcd((uint8_t)in->tm_min);
    buf[3] = bin2bcd((uint8_t)in->tm_hour);
    buf[4] = bin2bcd((uint8_t)in->tm_mday);
    buf[5] = (uint8_t)(in->tm_wday & 0x07);
    buf[6] = bin2bcd((uint8_t)(in->tm_mon + 1));
    buf[7] = bin2bcd((uint8_t)((in->tm_year + 1900) % 100));
    return i2c_master_transmit(s_rtc, buf, sizeof(buf), 200);
}

void pp_clock_today(char *out, size_t out_size)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(out, out_size, "%Y-%m-%d", &tm_now);
}

void pp_clock_format(char *out, size_t out_size, const char *fmt)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(out, out_size, fmt, &tm_now);
}

bool pp_clock_valid(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    return tm_now.tm_year + 1900 >= 2024;
}

/* SNTP works in UTC; the RTC chip holds UTC as well, so the local offset is
 * applied only when formatting for the screen. */
static void sntp_sync_cb(struct timeval *tv)
{
    struct tm tm_utc;
    time_t now = tv->tv_sec;
    gmtime_r(&now, &tm_utc);
    if (pp_clock_write_rtc(&tm_utc) == ESP_OK) {
        ESP_LOGI(TAG, "RTC updated from SNTP");
    }
}

void pp_clock_start_sntp(void)
{
    if (s_sntp_started) {
        return;
    }
    s_sntp_started = true;
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started");
}

void pp_clock_apply_tz(void)
{
    const char *tz = pp_config()->tz;
    setenv("TZ", tz && tz[0] ? tz : "CST-8", 1);
    tzset();
}

void pp_clock_init(void)
{
    const char *tz = pp_config()->tz;
    pp_clock_apply_tz();

    struct tm rtc;
    if (pp_clock_read_rtc(&rtc) == ESP_OK) {
        time_t t = timegm(&rtc);
        if (t > 1700000000) {
            struct timeval tv = {.tv_sec = t, .tv_usec = 0};
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "system time restored from RTC: %04d-%02d-%02d %02d:%02d",
                     rtc.tm_year + 1900, rtc.tm_mon + 1, rtc.tm_mday, rtc.tm_hour, rtc.tm_min);
        }
    }
    char now[32];
    pp_clock_format(now, sizeof(now), "%Y-%m-%d %H:%M:%S");
    ESP_LOGI(TAG, "local time %s (tz %s)", now, tz ? tz : "");
}
