#ifndef WIFI_DOWNLOAD_H
#define WIFI_DOWNLOAD_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// WiFi station management
// ---------------------------------------------------------------------------

/**
 * @brief Bring up the WiFi stack in station mode and connect to an AP.
 *
 * Initializes NVS, the network interface, the event loop and the WiFi driver
 * (all idempotent), then blocks until the connection succeeds or the retry
 * budget is exhausted.
 *
 * @param ssid     Network name to join.
 * @param password Network password (may be empty for open networks).
 * @return ESP_OK once an IP address is acquired, ESP_FAIL on timeout.
 */
esp_err_t wifi_manager_init(const char *ssid, const char *password);

/** @brief True if the station currently holds an IP lease. */
bool wifi_manager_is_connected(void);

// ---------------------------------------------------------------------------
// File / manifest sync
// ---------------------------------------------------------------------------

/**
 * @brief Stream a single file from a URL straight to local storage.
 *
 * @param api_url          Fully-qualified endpoint serving the file.
 * @param output_file_path Destination path (e.g. "/sdcard/track.mp3").
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t download_mp3_from_api(const char *api_url, const char *output_file_path);

/**
 * @brief Pull the backend manifest and download any tracks missing locally.
 *
 * Fetches "<base_url>/manifest", walks the JSON file list, and downloads every
 * entry that is not already present (matched by name + size) into @p dest_dir.
 *
 * @param base_url           Backend root, no trailing slash (e.g. "http://192.168.1.50:8000").
 * @param dest_dir           Local directory to mirror into (e.g. "/sdcard").
 * @param out_downloaded     Optional; receives the count of files fetched.
 * @return ESP_OK if the manifest was processed (individual file errors are logged).
 */
esp_err_t wifi_sync_from_manifest(const char *base_url, const char *dest_dir, int *out_downloaded);

#ifdef __cplusplus
}
#endif

#endif // WIFI_DOWNLOAD_H