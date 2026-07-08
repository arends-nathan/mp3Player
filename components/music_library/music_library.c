#include "music_library.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_spiffs.h"

#include "audio_driver.h"

static const char *TAG = "MUSIC_LIB";

#define MAX_NAME_LEN 64

// SPI bus wiring for the WWZMDiB Micro SD / TF card adapter. These default
// VSPI pins are free: they avoid the I2C OLED (21/22), the I2S amp (25/26/27)
// and the navigation buttons (32/33/14/13).
#define SD_PIN_MOSI 23
#define SD_PIN_MISO 19
#define SD_PIN_CLK  18
#define SD_PIN_CS   5

static char s_dir[64] = "/sdcard";
static char s_library[MUSIC_LIBRARY_MAX_TRACKS][MAX_NAME_LEN];
static int  s_count = 0;
static int  s_current = -1;
static sdmmc_card_t *s_card = NULL;

static bool has_mp3_extension(const char *name) {
    size_t len = strlen(name);
    return len > 4 && strcasecmp(name + len - 4, ".mp3") == 0;
}

bool music_library_init(const char *dir) {
    if (dir && dir[0] != '\0') {
        strncpy(s_dir, dir, sizeof(s_dir) - 1);
        s_dir[sizeof(s_dir) - 1] = '\0';
    }

    // The nightly sync rewrites the whole library, so we never format here;
    // a missing/blank card simply yields an empty library until inserted.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    // Keep the card at a very conservative clock during probing. Some SPI SD
    // adapters are marginal at the default probe rate.
    host.max_freq_khz = 400;

    ESP_LOGI(TAG, "SD probe config: mount=%s host_slot=%d freq_khz=%d mosi=%d miso=%d sclk=%d cs=%d",
             s_dir, host.slot, host.max_freq_khz,
             SD_PIN_MOSI, SD_PIN_MISO, SD_PIN_CLK, SD_PIN_CS);

    // Give the card and adapter a moment to settle before probing. Some SPI
    // SD adapters need a short power-up delay to answer CMD8/CMD52 reliably.
    vTaskDelay(pdMS_TO_TICKS(100));

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 512, // lower transfer size for flaky adapters
    };
    // Disable DMA for SDSPI initialization to increase compatibility with some SD adapters.
    esp_err_t err = spi_bus_initialize(host.slot, &bus_cfg, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to init SPI bus (%s); library will be empty", esp_err_to_name(err));
        ESP_LOGW(TAG, "SPI bus detail: host=%d mosi=%d miso=%d sclk=%d max_transfer_sz=%d",
                 host.slot, bus_cfg.mosi_io_num, bus_cfg.miso_io_num,
                 bus_cfg.sclk_io_num, bus_cfg.max_transfer_sz);
        s_count = 0;
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_CS;
    slot_config.host_id = host.slot;

    ESP_LOGI(TAG, "SD slot config: host=%d cs=%d", slot_config.host_id,
             slot_config.gpio_cs);

    ESP_LOGI(TAG, "Attempting SD mount at %s", s_dir);
    err = esp_vfs_fat_sdspi_mount(s_dir, &host, &slot_config, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Initial SD mount failed (%s); retrying once at probing speed", esp_err_to_name(err));
        ESP_LOGW(TAG, "Card init diagnostics: if CMD8/CMD52 fail, check 3.3V/VIN, CS=%d, MOSI=%d, MISO=%d, SCLK=%d, and card orientation",
                 SD_PIN_CS, SD_PIN_MOSI, SD_PIN_MISO, SD_PIN_CLK);
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_LOGI(TAG, "Retrying SD mount at %s", s_dir);
        err = esp_vfs_fat_sdspi_mount(s_dir, &host, &slot_config, &mount_config, &s_card);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (%s); attempting SPIFFS fallback", esp_err_to_name(err));
        // Try mounting SPIFFS as a fallback so the device can operate without an SD card.
        esp_vfs_spiffs_conf_t spiffs_conf = {
            .base_path = "/spiffs",
            .partition_label = NULL,
            .max_files = 5,
            .format_if_mount_failed = true,
        };
        esp_err_t sp_err = esp_vfs_spiffs_register(&spiffs_conf);
        if (sp_err == ESP_OK) {
            ESP_LOGI(TAG, "SPIFFS mounted at /spiffs (fallback)");
            strncpy(s_dir, "/spiffs", sizeof(s_dir) - 1);
            s_dir[sizeof(s_dir) - 1] = '\0';
            music_library_refresh();
            return true;
        } else {
            ESP_LOGW(TAG, "SPIFFS mount failed (%s)", esp_err_to_name(sp_err));
            ESP_LOGW(TAG, "Fallback diagnostics: spiffs partition should exist and be formatted; check build/flash completed with updated partitions.csv");
            s_count = 0;
            return false;
        }
    }
    ESP_LOGI(TAG, "SD card mounted at %s", s_dir);
    sdmmc_card_print_info(stdout, s_card);
    music_library_refresh();
    return true;
}

void music_library_refresh(void) {
    s_count = 0;
    ESP_LOGI(TAG, "Scanning mount point: %s", s_dir);
    DIR *dir = opendir(s_dir);
    if (!dir) {
        ESP_LOGW(TAG, "Could not open %s", s_dir);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_count < MUSIC_LIBRARY_MAX_TRACKS) {
        if (has_mp3_extension(ent->d_name)) {
            strncpy(s_library[s_count], ent->d_name, MAX_NAME_LEN - 1);
            s_library[s_count][MAX_NAME_LEN - 1] = '\0';
            s_count++;
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "Library refreshed: %d track(s)", s_count);

    if (s_current >= s_count) {
        s_current = s_count > 0 ? s_count - 1 : -1;
    }
}

int music_library_count(void) {
    return s_count;
}

const char *music_library_name(int index) {
    if (index < 0 || index >= s_count) {
        return "";
    }
    return s_library[index];
}

int music_library_current(void) {
    return s_current;
}

void music_library_play(int index) {
    if (index < 0 || index >= s_count) {
        ESP_LOGW(TAG, "Play request ignored: index=%d count=%d", index, s_count);
        return;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", s_dir, s_library[index]);
    ESP_LOGI(TAG, "Playing track[%d/%d]: %s", index + 1, s_count, path);
    s_current = index;
    audio_player_play(path);
}

void music_library_play_relative(int delta) {
    if (s_count == 0 || s_current < 0) {
        return;
    }
    int next = ((s_current + delta) % s_count + s_count) % s_count;
    music_library_play(next);
}

const char *music_library_get_mount_point(void) {
    return s_dir;
}
