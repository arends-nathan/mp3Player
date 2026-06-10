#include "sync_manager.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif.h"

#include "wifi_download.h"
#include "music_library.h"

static const char *TAG = "SYNC_MGR";

static char s_ssid[33] = "";
static char s_password[65] = "";
static char s_backend_url[128] = "";
static char s_local_dir[64] = "";

static volatile bool s_sync_requested = false;
static volatile sync_status_t s_status = SYNC_IDLE;
static volatile int s_last_count = 0;

// Drives WiFi association and manifest syncs without blocking the UI thread.
static void network_task(void *arg) {
    ESP_LOGI(TAG, "Network task started on Core %d", xPortGetCoreID());

    if (wifi_manager_init(s_ssid, s_password) == ESP_OK) {
        s_sync_requested = true; // initial sync at boot
    } else {
        ESP_LOGW(TAG, "Initial WiFi connection failed; sync available later via menu");
    }

    while (1) {
        if (s_sync_requested) {
            s_sync_requested = false;
            if (wifi_manager_is_connected()) {
                s_status = SYNC_RUNNING;
                int downloaded = 0;
                esp_err_t err = wifi_sync_from_manifest(s_backend_url, s_local_dir, &downloaded);
                if (err == ESP_OK) {
                    s_last_count = downloaded;
                    s_status = SYNC_DONE;
                    music_library_refresh();
                } else {
                    s_status = SYNC_FAILED;
                }
            } else {
                s_status = SYNC_FAILED;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void sync_manager_start(const char *ssid, const char *password,
                        const char *backend_url, const char *local_dir) {
    strncpy(s_ssid, ssid ? ssid : "", sizeof(s_ssid) - 1);
    strncpy(s_password, password ? password : "", sizeof(s_password) - 1);
    strncpy(s_backend_url, backend_url ? backend_url : "", sizeof(s_backend_url) - 1);
    strncpy(s_local_dir, local_dir ? local_dir : "", sizeof(s_local_dir) - 1);

    // Pin to core 0 (PRO_CPU), where the WiFi/lwIP stack runs. The nightly sync
    // is idle-time only, so it never competes with audio playback on core 0.
    xTaskCreatePinnedToCore(network_task, "Net_Task", 6144, NULL, 5, NULL, 0);
}

void sync_manager_request(void) {
    if (s_status != SYNC_RUNNING) {
        s_sync_requested = true;
    }
}

sync_status_t sync_manager_status(void) {
    return s_status;
}

int sync_manager_last_count(void) {
    return s_last_count;
}

bool sync_manager_wifi_connected(void) {
    return wifi_manager_is_connected();
}

const char *sync_manager_ssid(void) {
    return s_ssid;
}

void sync_manager_get_ip(char *buf, size_t len) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
    } else {
        snprintf(buf, len, "---");
    }
}
