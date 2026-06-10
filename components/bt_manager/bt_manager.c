#include "bt_manager.h"

#include <string.h>
#include <stdio.h>

#include "sdkconfig.h"
#include "esp_log.h"

static const char *TAG = "BT_MGR";

// Bluetooth classic + Bluedroid must be enabled in menuconfig for discovery to
// work. When they are not, the component still builds but the API is inert so
// the rest of the firmware is unaffected.
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_CLASSIC_ENABLED)

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "nvs.h"

#include "audio_driver.h"

#define BT_NVS_NAMESPACE "bt"
#define BT_INQUIRY_LEN   8 // units of 1.28 s (~10 s)

typedef struct {
    esp_bd_addr_t bda;
    char          name[BT_MANAGER_NAME_MAX];
} bt_device_entry_t;

static bool s_enabled = false;
static volatile bool s_scanning = false;
static bt_device_entry_t s_devices[BT_MANAGER_MAX_DEVICES];
static int s_device_count = 0;
static char s_selected_name[BT_MANAGER_NAME_MAX] = "";
static esp_bd_addr_t s_selected_bda = {0};
static bool s_has_selected = false;
static volatile bool s_connected = false;
static volatile bool s_media_started = false;
static SemaphoreHandle_t s_lock = NULL;

// Pull a readable name out of the EIR blob, falling back to the formatted BDA.
static void extract_name(esp_bt_gap_cb_param_t *param, char *out, size_t out_len) {
    uint8_t *eir = NULL;
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
        if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
            int len = p->len < (int)out_len - 1 ? p->len : (int)out_len - 1;
            memcpy(out, p->val, len);
            out[len] = '\0';
            return;
        }
        if (p->type == ESP_BT_GAP_DEV_PROP_EIR) {
            eir = (uint8_t *)p->val;
        }
    }

    if (eir) {
        uint8_t len = 0;
        uint8_t *name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
        if (!name) {
            name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
        }
        if (name && len > 0) {
            int n = len < (int)out_len - 1 ? len : (int)out_len - 1;
            memcpy(out, name, n);
            out[n] = '\0';
            return;
        }
    }

    // No advertised name: show the MAC address.
    uint8_t *b = param->disc_res.bda;
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             b[0], b[1], b[2], b[3], b[4], b[5]);
}

// ---------------------------------------------------------------------------
// A2DP source: streams decoded PCM from the audio driver to the connected
// Bluetooth sink. The stack pulls audio through s_a2d_data_cb on its own task.
// ---------------------------------------------------------------------------

// Called by the A2DP stack whenever it needs more PCM. Must return exactly the
// number of bytes written; the audio driver zero-pads underruns for us.
static int32_t a2d_data_callback(uint8_t *buf, int32_t len) {
    if (len <= 0 || buf == NULL) {
        return 0;
    }
    return audio_driver_read_pcm(buf, (int)len);
}

static void a2d_event_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                s_connected = true;
                ESP_LOGI(TAG, "A2DP connected; routing audio to Bluetooth");
                audio_driver_set_output(AUDIO_OUTPUT_BT);
                // Ask the sink whether it is ready to receive media.
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                s_connected = false;
                s_media_started = false;
                ESP_LOGI(TAG, "A2DP disconnected; routing audio to I2S");
                audio_driver_set_output(AUDIO_OUTPUT_I2S);
            }
            break;
        case ESP_A2D_MEDIA_CTRL_ACK_EVT:
            if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
                param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                // Sink is ready; begin streaming.
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            } else if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START &&
                       param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                s_media_started = true;
                ESP_LOGI(TAG, "A2DP media stream started");
            }
            break;
        default:
            break;
    }
}

static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            char name[BT_MANAGER_NAME_MAX];
            extract_name(param, name, sizeof(name));

            xSemaphoreTake(s_lock, portMAX_DELAY);
            // De-duplicate by address.
            bool known = false;
            for (int i = 0; i < s_device_count; i++) {
                if (memcmp(s_devices[i].bda, param->disc_res.bda, sizeof(esp_bd_addr_t)) == 0) {
                    known = true;
                    break;
                }
            }
            if (!known && s_device_count < BT_MANAGER_MAX_DEVICES) {
                memcpy(s_devices[s_device_count].bda, param->disc_res.bda, sizeof(esp_bd_addr_t));
                strncpy(s_devices[s_device_count].name, name, BT_MANAGER_NAME_MAX - 1);
                s_devices[s_device_count].name[BT_MANAGER_NAME_MAX - 1] = '\0';
                s_device_count++;
                ESP_LOGI(TAG, "Found device: %s", name);
            }
            xSemaphoreGive(s_lock);
            break;
        }
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            s_scanning = (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED);
            ESP_LOGI(TAG, "Discovery %s", s_scanning ? "started" : "stopped");
            break;
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Paired with %s", param->auth_cmpl.device_name);
            } else {
                ESP_LOGW(TAG, "Pairing failed (status %d)", param->auth_cmpl.stat);
            }
            break;
#if (defined(CONFIG_BT_SSP_ENABLED) && CONFIG_BT_SSP_ENABLED)
        case ESP_BT_GAP_CFM_REQ_EVT:
            // "Just works" pairing: auto-confirm the numeric comparison.
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;
        case ESP_BT_GAP_KEY_NOTIF_EVT:
        case ESP_BT_GAP_KEY_REQ_EVT:
            break;
#endif
        default:
            break;
    }
}

static void load_selected(void) {
    nvs_handle_t handle;
    if (nvs_open(BT_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_selected_name);
    if (nvs_get_str(handle, "name", s_selected_name, &len) != ESP_OK) {
        s_selected_name[0] = '\0';
    }
    size_t bda_len = sizeof(s_selected_bda);
    if (nvs_get_blob(handle, "bda", s_selected_bda, &bda_len) == ESP_OK &&
        bda_len == sizeof(esp_bd_addr_t)) {
        s_has_selected = true;
    }
    nvs_close(handle);
}

void bt_manager_set_enabled(bool enable) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        load_selected();
    }

    if (enable == s_enabled) {
        return;
    }

    if (enable) {
        ESP_LOGI(TAG, "Enabling Bluetooth...");
        esp_bt_controller_mem_release(ESP_BT_MODE_BLE); // classic only, reclaim BLE RAM

        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        if (esp_bt_controller_init(&bt_cfg) != ESP_OK ||
            esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
            ESP_LOGE(TAG, "Controller init/enable failed");
            return;
        }
        if (esp_bluedroid_init() != ESP_OK || esp_bluedroid_enable() != ESP_OK) {
            ESP_LOGE(TAG, "Bluedroid init/enable failed");
            return;
        }

        esp_bt_gap_register_callback(gap_callback);
        esp_bt_gap_set_device_name("AuraSync");
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

#if (defined(CONFIG_BT_SSP_ENABLED) && CONFIG_BT_SSP_ENABLED)
        // "Just works" secure simple pairing (no display/keyboard on the device).
        esp_bt_io_cap_t io_cap = ESP_BT_IO_CAP_NONE;
        esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &io_cap, sizeof(io_cap));
#endif

        // Bring up the A2DP audio source so we can stream to a speaker/headphones.
        esp_a2d_register_callback(a2d_event_callback);
        esp_a2d_source_register_data_callback(a2d_data_callback);
        esp_a2d_source_init();

        s_enabled = true;
        ESP_LOGI(TAG, "Bluetooth enabled");
    } else {
        ESP_LOGI(TAG, "Disabling Bluetooth...");
        if (s_scanning) {
            esp_bt_gap_cancel_discovery();
        }
        if (s_connected) {
            esp_a2d_source_disconnect(s_selected_bda);
        }
        esp_a2d_source_deinit();
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();

        // Make sure local playback is restored if a sink was connected.
        audio_driver_set_output(AUDIO_OUTPUT_I2S);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_device_count = 0;
        xSemaphoreGive(s_lock);

        s_scanning = false;
        s_connected = false;
        s_media_started = false;
        s_enabled = false;
    }
}

bool bt_manager_is_enabled(void) {
    return s_enabled;
}

void bt_manager_start_scan(void) {
    if (!s_enabled) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_device_count = 0;
    xSemaphoreGive(s_lock);

    esp_bt_gap_start_discovery(ESP_BT_INQUIRY_MODE, BT_INQUIRY_LEN, 0);
}

bool bt_manager_is_scanning(void) {
    return s_scanning;
}

int bt_manager_device_count(void) {
    return s_device_count;
}

const char *bt_manager_device_name(int index) {
    if (index < 0 || index >= s_device_count) {
        return "";
    }
    return s_devices[index].name;
}

void bt_manager_select(int index) {
    if (index < 0 || index >= s_device_count) {
        return;
    }
    strncpy(s_selected_name, s_devices[index].name, sizeof(s_selected_name) - 1);
    s_selected_name[sizeof(s_selected_name) - 1] = '\0';
    memcpy(s_selected_bda, s_devices[index].bda, sizeof(esp_bd_addr_t));
    s_has_selected = true;

    nvs_handle_t handle;
    if (nvs_open(BT_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, "name", s_selected_name);
        nvs_set_blob(handle, "bda", s_devices[index].bda, sizeof(esp_bd_addr_t));
        nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "Selected device: %s", s_selected_name);

    // Stop discovery (radio cannot inquire and connect at the same time) and
    // immediately try to establish the audio link.
    if (s_scanning) {
        esp_bt_gap_cancel_discovery();
    }
    bt_manager_connect();
}

const char *bt_manager_selected_name(void) {
    return s_selected_name;
}

void bt_manager_connect(void) {
    if (!s_enabled || !s_has_selected || s_connected) {
        return;
    }
    ESP_LOGI(TAG, "Connecting A2DP to %s", s_selected_name);
    esp_a2d_source_connect(s_selected_bda);
}

void bt_manager_disconnect(void) {
    if (!s_enabled || !s_connected) {
        return;
    }
    esp_a2d_source_disconnect(s_selected_bda);
}

bool bt_manager_is_connected(void) {
    return s_connected;
}

#else // Bluetooth not enabled in menuconfig — inert stubs.

void bt_manager_set_enabled(bool enable) {
    (void)enable;
    ESP_LOGW(TAG, "Bluetooth disabled in menuconfig (enable BT + Classic BT)");
}
bool bt_manager_is_enabled(void) { return false; }
void bt_manager_start_scan(void) {}
bool bt_manager_is_scanning(void) { return false; }
int bt_manager_device_count(void) { return 0; }
const char *bt_manager_device_name(int index) { (void)index; return ""; }
void bt_manager_select(int index) { (void)index; }
const char *bt_manager_selected_name(void) { return ""; }
void bt_manager_connect(void) {}
void bt_manager_disconnect(void) {}
bool bt_manager_is_connected(void) { return false; }

#endif
