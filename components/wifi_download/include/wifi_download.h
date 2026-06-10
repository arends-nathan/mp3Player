#ifndef WIFI_DOWNLOAD_H
#define WIFI_DOWNLOAD_H

#include "esp_err.h"

/**
 * @brief Initialize the network client interface and pull down an MP3 file
 * from a specified endpoint straight to storage.
 * * @param api_url The web endpoint hosting the target audio track file
 * @param output_file_path The absolute directory target path (e.g., "/sdcard/track.mp3")
 * @return esp_err_t ESP_OK on total transmission success, or error configuration flags
 */
esp_err_t download_mp3_from_api(const char *api_url, const char *output_file_path);

#endif // WIFI_DOWNLOAD_H