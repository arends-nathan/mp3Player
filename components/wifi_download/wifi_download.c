#include "wifi_download.h"
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "NET_DOWNLOADER";
#define DOWNLOAD_BUFFER_SIZE 1024

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