#ifndef SYNC_MANAGER_H
#define SYNC_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

#include "wifi_download.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Progress of the most recent (or current) backend sync.
 */
typedef enum {
    SYNC_IDLE,
    SYNC_RUNNING,
    SYNC_DONE,
    SYNC_FAILED,
} sync_status_t;

/**
 * @brief Progress of an on-demand WiFi network scan.
 */
typedef enum {
    WIFI_SCAN_IDLE,
    WIFI_SCAN_RUNNING,
    WIFI_SCAN_DONE,
} wifi_scan_state_t;

/**
 * @brief Start the background networking task.
 *
 * Connects to WiFi, performs an initial manifest sync, then services on-demand
 * sync requests. All work happens off the UI thread.
 *
 * @param ssid        WiFi network name.
 * @param password    WiFi password (may be empty for open networks).
 * @param backend_url Sync API base URL, no trailing slash.
 * @param local_dir   Directory to mirror downloaded tracks into.
 */
void sync_manager_start(const char *ssid, const char *password,
                        const char *backend_url, const char *local_dir);

/** @brief Queue a manifest sync (ignored if one is already running). */
void sync_manager_request(void);

/** @brief Current sync status for display. */
sync_status_t sync_manager_status(void);

/** @brief Number of files pulled during the last successful sync. */
int sync_manager_last_count(void);

/** @brief True while the station holds an IP lease. */
bool sync_manager_wifi_connected(void);

/** @brief The configured SSID (for display). */
const char *sync_manager_ssid(void);

/**
 * @brief Copy the current station IP address as a string into @p buf.
 * Writes "---" when no address is assigned.
 */
void sync_manager_get_ip(char *buf, size_t len);

// ---------------------------------------------------------------------------
// WiFi settings (scan + connect). All radio work runs on the network task.
// ---------------------------------------------------------------------------

/** @brief Queue a WiFi network scan (ignored if one is already running). */
void sync_manager_request_scan(void);

/** @brief Progress of the most recent scan. */
wifi_scan_state_t sync_manager_scan_state(void);

/** @brief Number of access points found by the last scan. */
int sync_manager_scan_count(void);

/**
 * @brief Access an access point from the last scan.
 * @return Pointer to the entry, or NULL if @p index is out of range.
 */
const wifi_ap_info_t *sync_manager_scan_ap(int index);

/**
 * @brief Connect to a network and persist the credentials.
 *
 * The connection attempt and NVS save happen on the network task; on success a
 * manifest sync is triggered automatically.
 */
void sync_manager_connect(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif

#endif // SYNC_MANAGER_H
