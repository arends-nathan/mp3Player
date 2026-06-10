#include "wifi_download.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "NET_DOWNLOADER";
#define DOWNLOAD_BUFFER_SIZE 1024

// ---------------------------------------------------------------------------
// WiFi station management
// ---------------------------------------------------------------------------
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRY      8

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_count = 0;
static volatile bool s_wifi_connected = false;
static bool s_wifi_started = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        if (s_retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "Retrying WiFi connection (%d/%d)", s_retry_count, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_init(const char *ssid, const char *password) {
    if (!ssid) {
        return ESP_ERR_INVALID_ARG;
    }

    // NVS is required by the WiFi driver for calibration data.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    if (!s_wifi_started) {
        s_wifi_event_group = xEventGroupCreate();
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler, NULL, NULL));
        s_wifi_started = true;
    }

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    s_retry_count = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Failed to connect to SSID: %s", ssid);
    return ESP_FAIL;
}

bool wifi_manager_is_connected(void) {
    return s_wifi_connected;
}

// ---------------------------------------------------------------------------
// Single-file download
// ---------------------------------------------------------------------------

esp_err_t download_mp3_from_api(const char *api_url, const char *output_file_path) {
    char *buffer = malloc(DOWNLOAD_BUFFER_SIZE);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for download stream buffer");
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(output_file_path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open target storage path: %s", output_file_path);
        free(buffer);
        return ESP_FAIL;
    }

    esp_http_client_config_t config = {
        .url = api_url,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach, // Enables secure TLS/HTTPS natively
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to instantiate HTTP config core handle");
        fclose(f);
        free(buffer);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to resolve server handshakes: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        fclose(f);
        free(buffer);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "Server returned invalid response state: %d", status_code);
        esp_http_client_cleanup(client);
        fclose(f);
        free(buffer);
        return ESP_FAIL;
    }

    int read_bytes = 0;
    while (1) {
        read_bytes = esp_http_client_read(client, buffer, DOWNLOAD_BUFFER_SIZE);
        
        if (read_bytes < 0) {
            ESP_LOGE(TAG, "Error encountered while decoding stream payload data chunk");
            break;
        } else if (read_bytes == 0) {
            ESP_LOGI(TAG, "Network stream reached EOF. Download completely finalized!");
            break; 
        }

        fwrite(buffer, 1, read_bytes, f);
        vTaskDelay(pdMS_TO_TICKS(1)); // Feeds Core 0 Watchdog Timer during long transfers
    }

    fclose(f);
    free(buffer);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return (read_bytes >= 0) ? ESP_OK : ESP_FAIL;
}

// ---------------------------------------------------------------------------
// Manifest fetch + selective sync
// ---------------------------------------------------------------------------

// Accumulator for an in-memory HTTP GET (the manifest JSON is small).
typedef struct {
    char *data;
    int len;
    int capacity;
} http_accumulator_t;

static esp_err_t manifest_http_event(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        http_accumulator_t *acc = (http_accumulator_t *)evt->user_data;
        if (acc->len + evt->data_len + 1 > acc->capacity) {
            int new_cap = (acc->capacity == 0) ? 4096 : acc->capacity * 2;
            while (new_cap < acc->len + evt->data_len + 1) {
                new_cap *= 2;
            }
            char *grown = realloc(acc->data, new_cap);
            if (!grown) {
                return ESP_FAIL;
            }
            acc->data = grown;
            acc->capacity = new_cap;
        }
        memcpy(acc->data + acc->len, evt->data, evt->data_len);
        acc->len += evt->data_len;
        acc->data[acc->len] = '\0';
    }
    return ESP_OK;
}

// True when a local file already matches the manifest entry's size.
static bool local_file_matches(const char *path, long expected_size) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return (long)st.st_size == expected_size;
}

esp_err_t wifi_sync_from_manifest(const char *base_url, const char *dest_dir, int *out_downloaded) {
    if (!base_url || !dest_dir) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_downloaded) {
        *out_downloaded = 0;
    }

    char manifest_url[256];
    snprintf(manifest_url, sizeof(manifest_url), "%s/manifest", base_url);

    http_accumulator_t acc = { 0 };
    esp_http_client_config_t config = {
        .url = manifest_url,
        .method = HTTP_METHOD_GET,
        .event_handler = manifest_http_event,
        .user_data = &acc,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(acc.data);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || acc.data == NULL) {
        ESP_LOGE(TAG, "Failed to fetch manifest (err=%s, status=%d)", esp_err_to_name(err), status);
        free(acc.data);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(acc.data);
    free(acc.data);
    if (!root) {
        ESP_LOGE(TAG, "Manifest JSON parse failed");
        return ESP_FAIL;
    }

    cJSON *files = cJSON_GetObjectItem(root, "files");
    if (!cJSON_IsArray(files)) {
        ESP_LOGE(TAG, "Manifest missing 'files' array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int downloaded = 0;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, files) {
        cJSON *name = cJSON_GetObjectItem(entry, "name");
        cJSON *url = cJSON_GetObjectItem(entry, "url");
        cJSON *size = cJSON_GetObjectItem(entry, "size");
        if (!cJSON_IsString(name) || !cJSON_IsString(url)) {
            continue;
        }

        long expected_size = cJSON_IsNumber(size) ? (long)size->valuedouble : -1;

        char local_path[300];
        snprintf(local_path, sizeof(local_path), "%s/%s", dest_dir, name->valuestring);

        if (expected_size >= 0 && local_file_matches(local_path, expected_size)) {
            ESP_LOGI(TAG, "Up to date: %s", name->valuestring);
            continue;
        }

        char file_url[384];
        snprintf(file_url, sizeof(file_url), "%s%s", base_url, url->valuestring);

        ESP_LOGI(TAG, "Downloading: %s", name->valuestring);
        if (download_mp3_from_api(file_url, local_path) == ESP_OK) {
            downloaded++;
        } else {
            ESP_LOGE(TAG, "Download failed: %s", name->valuestring);
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Manifest sync complete: %d new file(s)", downloaded);
    if (out_downloaded) {
        *out_downloaded = downloaded;
    }
    return ESP_OK;
}