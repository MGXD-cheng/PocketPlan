/*
 * PocketPlan internal storage: a 1 MB SPIFFS partition mounted at /lfs.
 * Everything here is small (a few KB), so the whole file is read into RAM.
 */
#include "store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board.h"
#include "config.h"
#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "store";

static bool s_ready;
static uint32_t s_total_kb;
static uint32_t s_free_kb;

esp_err_t pp_store_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = PP_LFS,
        .partition_label = "storage",
        .max_files = 8,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount failed: %s", esp_err_to_name(err));
        return err;
    }
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        s_total_kb = (uint32_t)(total / 1024);
        s_free_kb = (uint32_t)((total - used) / 1024);
    }
    s_ready = true;
    ESP_LOGI(TAG, "mounted at %s (%u KB total, %u KB free)", PP_LFS,
             (unsigned)s_total_kb, (unsigned)s_free_kb);
    return ESP_OK;
}

bool pp_store_ready(void)
{
    return s_ready;
}

uint32_t pp_store_total_kb(void)
{
    return s_total_kb;
}

uint32_t pp_store_free_kb(void)
{
    return s_free_kb;
}

/* --------------------------- where live data lives ----------------------- */
static bool s_sd_dir_ok;

bool pp_data_on_sd(void)
{
    if (!pp_config()->cache_sd || !pp_board_sd_mounted()) {
        return false;
    }
    if (!s_sd_dir_ok) {
        if (mkdir(PP_DATA_DIR_SD, 0777) == 0 || errno == EEXIST) {
            s_sd_dir_ok = true;
        } else {
            ESP_LOGW(TAG, "cannot create %s (errno %d)", PP_DATA_DIR_SD, errno);
            return false;
        }
    }
    return true;
}

void pp_data_path(char *out, size_t size, const char *name)
{
    snprintf(out, size, "%s/%s", pp_data_on_sd() ? PP_DATA_DIR_SD : PP_LFS, name);
}

char *pp_data_read(const char *name, size_t *len_out)
{
    char path[96];
    pp_data_path(path, sizeof(path), name);
    return pp_store_read(path, len_out);
}

esp_err_t pp_data_write(const char *name, const char *data, size_t len)
{
    if (pp_config()->cache_sd && !pp_data_on_sd()) {
        ESP_LOGW(TAG, "SD cache enabled but the card is unavailable, not saving %s", name);
        return ESP_ERR_INVALID_STATE;
    }
    char path[96];
    pp_data_path(path, sizeof(path), name);
    return pp_store_write_atomic(path, data, len);
}

esp_err_t pp_store_migrate_data(void)
{
    static const char *files[] = {"plans.json", "countdowns.json", "alarms.json",
                                  "ai_chat.json"};
    bool to_sd = pp_data_on_sd();
    const char *dst_dir = to_sd ? PP_DATA_DIR_SD : PP_LFS;
    const char *src_dir = to_sd ? PP_LFS : PP_DATA_DIR_SD;
    int moved = 0;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char src[80];
        char dst[96];
        snprintf(src, sizeof(src), "%s/%s", src_dir, files[i]);
        snprintf(dst, sizeof(dst), "%s/%s", dst_dir, files[i]);
        size_t len = 0;
        char *text = pp_store_read(src, &len);
        if (text == NULL) {
            continue;
        }
        esp_err_t err = pp_store_write_atomic(dst, text, len);
        free(text);
        if (err == ESP_OK) {
            /* Read the copy back before dropping the original: a truncated
             * write must never end up as the only surviving copy. */
            size_t check = 0;
            char *back = pp_store_read(dst, &check);
            if (back == NULL || check != len) {
                ESP_LOGE(TAG, "%s read back short after migration, keeping both", dst);
                free(back);
                continue;
            }
            free(back);
            pp_store_remove(src);
            moved++;
        } else {
            ESP_LOGE(TAG, "cannot move %s to %s: %s", files[i], dst_dir,
                     esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "moved %d data file(s) to %s", moved, dst_dir);
    return ESP_OK;
}

char *pp_store_read(const char *path, size_t *len_out)
{
    if (len_out) {
        *len_out = 0;
    }
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    size_t size = (size_t)st.st_size;
    char *buf = malloc(size + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, size, f);
    fclose(f);
    buf[got] = '\0';
    if (len_out) {
        *len_out = got;
    }
    return buf;
}

esp_err_t pp_store_write_atomic(const char *path, const char *data, size_t len)
{
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot open %s", tmp);
        return ESP_FAIL;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    if (ok) {
        ok = fflush(f) == 0;
    }
    if (ok) {
        fsync(fileno(f));
    }
    fclose(f);
    if (!ok) {
        unlink(tmp);
        return ESP_FAIL;
    }
    if (rename(tmp, path) != 0) {
        /* SPIFFS tolerates a failing rename when the target exists; fall back
         * to an in-place copy so the new content is not lost. */
        FILE *dst = fopen(path, "wb");
        if (dst == NULL) {
            unlink(tmp);
            return ESP_FAIL;
        }
        bool copied = fwrite(data, 1, len, dst) == len;
        fflush(dst);
        fsync(fileno(dst));
        fclose(dst);
        unlink(tmp);
        if (!copied) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_err_t pp_store_remove(const char *path)
{
    return unlink(path) == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}
