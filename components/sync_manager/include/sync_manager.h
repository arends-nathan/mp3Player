#ifndef SYNC_MANAGER_H
#define SYNC_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

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

#ifdef __cplusplus
}
#endif

#endif // SYNC_MANAGER_H
