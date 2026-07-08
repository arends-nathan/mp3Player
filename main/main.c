#include "esp_log.h"
#include "sdkconfig.h"

#include "audio_driver.h"
#include "buttons.h"
#include "music_library.h"
#include "sync_manager.h"
#include "ui.h"

// ===========================================================================
// Board configuration
// ===========================================================================
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22

// Pulled from menuconfig (Kconfig.projbuild); fall back to sane defaults so the
// project still builds before the user runs `idf.py menuconfig`.
#ifndef CONFIG_SYNC_LOCAL_DIR
#define CONFIG_SYNC_LOCAL_DIR "/sdcard"
#endif
#ifndef CONFIG_SYNC_BACKEND_URL
#define CONFIG_SYNC_BACKEND_URL "http://192.168.1.50:8000"
#endif
#ifndef CONFIG_ESP_WIFI_SSID
#error "WiFi SSID not configured. Please configure it in menuconfig"
#endif
#ifndef CONFIG_ESP_WIFI_PASSWORD
#error "WiFi password not configured. Please configure it in menuconfig"
#endif

static const char *TAG = "AURASYNC";

// ===========================================================================
// Composition root: wire the independent components together.
// ===========================================================================
void app_main(void) {
    ESP_LOGI(TAG, "Booting AuraSync MP3 player...");

    // Front-panel navigation buttons.
    buttons_init();

    // Storage + library scan (mounts the music directory).
    music_library_init(CONFIG_SYNC_LOCAL_DIR);

    // Audio engine (I2S + MP3 decode task pinned to core 0).
    if (audio_driver_init() != ESP_OK) {
        ESP_LOGE(TAG, "Audio driver failed to initialize");
    }

    // Background networking: WiFi association + manifest sync. Use the actual
    // mount point returned by the music library (handles SPIFFS fallback).
    const char *mount = music_library_get_mount_point();
    sync_manager_start(CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_PASSWORD,
                       CONFIG_SYNC_BACKEND_URL, mount ? mount : CONFIG_SYNC_LOCAL_DIR);

    // Display + menu UI (rendering task pinned to core 1).
    ui_init(I2C_SDA_PIN, I2C_SCL_PIN);
    ui_start_task();
}
