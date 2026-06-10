#ifndef BT_MANAGER_H
#define BT_MANAGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BT_MANAGER_MAX_DEVICES 20
#define BT_MANAGER_NAME_MAX    32

/**
 * @brief Enable or disable the Bluetooth controller + Bluedroid stack.
 *
 * The stack is brought up lazily on the first enable. Disabling tears it down
 * to free memory (Bluetooth and WiFi compete for RAM on the ESP32).
 *
 * @param enable true to power on Bluetooth, false to shut it down.
 */
void bt_manager_set_enabled(bool enable);

/** @brief True when the Bluetooth stack is currently running. */
bool bt_manager_is_enabled(void);

/**
 * @brief Start a GAP inquiry to discover nearby Bluetooth devices.
 *
 * Results accumulate asynchronously; read them with ::bt_manager_device_count
 * and ::bt_manager_device_name. No-op when Bluetooth is disabled.
 */
void bt_manager_start_scan(void);

/** @brief True while an inquiry is in progress. */
bool bt_manager_is_scanning(void);

/** @brief Number of devices found by the current/last inquiry. */
int bt_manager_device_count(void);

/**
 * @brief Display name (or address fallback) of a discovered device.
 * @return The name, or an empty string when @p index is out of range.
 */
const char *bt_manager_device_name(int index);

/**
 * @brief Remember a discovered device as the preferred audio sink and begin
 * connecting to it over A2DP.
 *
 * Persists the selection so it survives a reboot. Once the sink accepts the
 * connection, decoded audio is routed to it automatically.
 */
void bt_manager_select(int index);

/** @brief Name of the remembered device, or an empty string if none. */
const char *bt_manager_selected_name(void);

/**
 * @brief Connect the A2DP audio stream to the remembered device.
 *
 * No-op when Bluetooth is disabled or no device has been selected.
 */
void bt_manager_connect(void);

/** @brief Tear down the active A2DP audio connection. */
void bt_manager_disconnect(void);

/** @brief True when an A2DP sink is connected and receiving audio. */
bool bt_manager_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // BT_MANAGER_H
