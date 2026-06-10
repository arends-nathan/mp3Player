#include "esp_log.h"
#include "sdkconfig.h"
#include "nvs_flash.h"

#include "audio_driver.h"
#include "buttons.h"
#include "music_library.h"
#include "sync_manager.h"
#include "wifi_download.h"
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
#define CONFIG_ESP_WIFI_SSID "myssid"
#endif
#ifndef CONFIG_ESP_WIFI_PASSWORD
#define CONFIG_ESP_WIFI_PASSWORD "mypassword"
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

    // Initialize NVS early so saved WiFi credentials can be read before the
    // network task starts. (wifi_manager_init later treats this as a no-op.)
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Prefer credentials saved from the WiFi settings menu; fall back to the
    // compile-time defaults from menuconfig on first boot.
    static char ssid[33];
    static char password[65];
    const char *use_ssid = CONFIG_ESP_WIFI_SSID;
    const char *use_pass = CONFIG_ESP_WIFI_PASSWORD;
    if (wifi_creds_load(ssid, sizeof(ssid), password, sizeof(password))) {
        ESP_LOGI(TAG, "Using saved WiFi credentials for SSID: %s", ssid);
        use_ssid = ssid;
        use_pass = password;
    }

    // Background networking: WiFi association + manifest sync.
    sync_manager_start(use_ssid, use_pass,
                       CONFIG_SYNC_BACKEND_URL, CONFIG_SYNC_LOCAL_DIR);

    // Display + menu UI (rendering task pinned to core 1).
    ui_init(I2C_SDA_PIN, I2C_SCL_PIN);
    ui_start_task();
}
