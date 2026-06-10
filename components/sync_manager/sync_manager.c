#include "sync_manager.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif.h"

#include "wifi_download.h"
#include "music_library.h"
#include "bt_manager.h"

static const char *TAG = "SYNC_MGR";

static char s_ssid[33] = "";
static char s_password[65] = "";
static char s_backend_url[128] = "";
static char s_local_dir[64] = "";

static volatile bool s_sync_requested = false;
static volatile sync_status_t s_status = SYNC_IDLE;
static volatile int s_last_count = 0;

// WiFi scan state.
static volatile bool s_scan_requested = false;
static volatile wifi_scan_state_t s_scan_state = WIFI_SCAN_IDLE;
static wifi_ap_info_t s_scan_aps[WIFI_SCAN_MAX_AP];
static volatile int s_scan_count = 0;

// Pending connect request (credentials staged by the UI).
static volatile bool s_connect_requested = false;
static volatile bool s_await_connect = false;
static char s_pending_ssid[WIFI_SSID_MAX_LEN] = "";
static char s_pending_pass[WIFI_PASS_MAX_LEN] = "";

// Drives WiFi association and manifest syncs without blocking the UI thread.
static void network_task(void *arg) {
    ESP_LOGI(TAG, "Network task started on Core %d", xPortGetCoreID());

    if (wifi_manager_init(s_ssid, s_password) == ESP_OK) {
        s_sync_requested = true; // initial sync at boot
    } else {
        ESP_LOGW(TAG, "Initial WiFi connection failed; sync available later via menu");
    }

    while (1) {
        // 1. Handle a pending connect request from the settings UI.
        if (s_connect_requested) {
            s_connect_requested = false;
            strncpy(s_ssid, s_pending_ssid, sizeof(s_ssid) - 1);
            strncpy(s_password, s_pending_pass, sizeof(s_password) - 1);
            wifi_creds_save(s_ssid, s_password);
            if (wifi_manager_connect(s_ssid, s_password) == ESP_OK) {
                s_await_connect = true;
            }
        }

        // 2. Once a requested connection lands, kick off a sync.
        if (s_await_connect && wifi_manager_is_connected()) {
            s_await_connect = false;
            s_sync_requested = true;
        }

        // 3. Handle a network scan request.
        if (s_scan_requested) {
            s_scan_requested = false;
            s_scan_state = WIFI_SCAN_RUNNING;
            int found = 0;
            if (wifi_manager_scan(s_scan_aps, WIFI_SCAN_MAX_AP, &found) == ESP_OK) {
                s_scan_count = found;
            } else {
                s_scan_count = 0;
            }
            s_scan_state = WIFI_SCAN_DONE;
        }

        // 4. Handle a manifest sync request.
        if (s_sync_requested) {
            s_sync_requested = false;
            if (wifi_manager_is_connected()) {
                s_status = SYNC_RUNNING;

                // Bluetooth and WiFi share the radio and a tight RAM budget on
                // the ESP32. They never need to run together, so power BT down
                // for the duration of the (idle-time) download and restore it
                // afterwards.
                bool bt_was_on = bt_manager_is_enabled();
                if (bt_was_on) {
                    ESP_LOGI(TAG, "Suspending Bluetooth during sync");
                    bt_manager_set_enabled(false);
                }

                int downloaded = 0;
                esp_err_t err = wifi_sync_from_manifest(s_backend_url, s_local_dir, &downloaded);
                if (err == ESP_OK) {
                    s_last_count = downloaded;
                    s_status = SYNC_DONE;
                    music_library_refresh();
                } else {
                    s_status = SYNC_FAILED;
                }

                if (bt_was_on) {
                    ESP_LOGI(TAG, "Restoring Bluetooth after sync");
                    bt_manager_set_enabled(true);
                    bt_manager_connect();
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

void sync_manager_request_scan(void) {
    if (s_scan_state != WIFI_SCAN_RUNNING) {
        s_scan_state = WIFI_SCAN_RUNNING;
        s_scan_requested = true;
    }
}

wifi_scan_state_t sync_manager_scan_state(void) {
    return s_scan_state;
}

int sync_manager_scan_count(void) {
    return s_scan_count;
}

const wifi_ap_info_t *sync_manager_scan_ap(int index) {
    if (index < 0 || index >= s_scan_count) {
        return NULL;
    }
    return &s_scan_aps[index];
}

void sync_manager_connect(const char *ssid, const char *password) {
    if (!ssid || ssid[0] == '\0') {
        return;
    }
    strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid) - 1);
    s_pending_ssid[sizeof(s_pending_ssid) - 1] = '\0';
    strncpy(s_pending_pass, password ? password : "", sizeof(s_pending_pass) - 1);
    s_pending_pass[sizeof(s_pending_pass) - 1] = '\0';
    s_connect_requested = true;
}
