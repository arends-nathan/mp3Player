#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "u8g2_esp32_hal.h"
#include "u8g2.h"

#include "audio_driver.h"
#include "buttons.h"
#include "music_library.h"
#include "sync_manager.h"
#include "bt_manager.h"

static const char *TAG = "AURASYNC_UI";

#define FIRMWARE_VERSION "1.0.0"

// Character set offered by the on-screen password keyboard.
static const char PASSWORD_CHARSET[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !@#$%^&*()-_=+.,?";

// ===========================================================================
// UI state machine
// ===========================================================================
typedef enum {
    UI_STATE_WELCOME,
    UI_STATE_NOW_PLAYING,
    UI_STATE_MENU,
    UI_STATE_LIBRARY,
    UI_STATE_VOLUME,
    UI_STATE_WIFI,          /*!< WiFi settings hub */
    UI_STATE_WIFI_SCAN,     /*!< Pick a network from a scan */
    UI_STATE_WIFI_PASSWORD, /*!< On-screen password entry */
    UI_STATE_BLUETOOTH,     /*!< Bluetooth settings hub */
    UI_STATE_BT_SCAN,       /*!< Pick a discovered device */
    UI_STATE_INFO,
} ui_state_t;

typedef struct {
    const char *label;
    ui_state_t target;
} menu_item_t;

static const menu_item_t main_menu[] = {
    { "Now Playing", UI_STATE_NOW_PLAYING },
    { "Library",     UI_STATE_LIBRARY },
    { "Volume",      UI_STATE_VOLUME },
    { "WiFi",        UI_STATE_WIFI },
    { "Bluetooth",   UI_STATE_BLUETOOTH },
    { "Info",        UI_STATE_INFO },
};
#define MAIN_MENU_COUNT (sizeof(main_menu) / sizeof(main_menu[0]))

// Display handle owned by this module.
static u8g2_t s_u8g2;

// Navigation state.
static ui_state_t current_ui_state = UI_STATE_WELCOME;
static uint32_t state_timer_ticks = 0;
static int menu_cursor = 0;
static int menu_scroll_top = 0;
static int library_cursor = 0;
static int library_scroll_top = 0;
static int16_t scroll_x_offset = 128; // now-playing title marquee

// WiFi settings state.
static int  wifi_settings_cursor = 0;        // 0 = Scan, 1 = Sync now
static int  wifi_scan_cursor = 0;
static int  wifi_scan_scroll_top = 0;
static char selected_ssid[33] = "";
static bool selected_secure = false;
static char password_buf[65] = "";
static int  password_len = 0;
static int  charset_idx = 0;

// Bluetooth settings state.
static int  bt_settings_cursor = 0;          // 0 = toggle, 1 = Scan devices
static int  bt_scan_cursor = 0;
static int  bt_scan_scroll_top = 0;

// ===========================================================================
// Small drawing helpers
// ===========================================================================
static void draw_header(u8g2_t *u, const char *title) {
    u8g2_SetFont(u, u8g2_font_5x7_tf);
    u8g2_DrawStr(u, 4, 11, title);
    u8g2_DrawLine(u, 0, 15, 128, 15);
}

static void format_time(char *buf, size_t len, uint32_t seconds) {
    snprintf(buf, len, "%lu:%02lu", (unsigned long)(seconds / 60),
             (unsigned long)(seconds % 60));
}

// Generic scrolling list renderer.
static void draw_list(u8g2_t *u, const char *title, int count, int cursor,
                      int *scroll_top, const char *(*label_fn)(int, void *), void *ctx) {
    draw_header(u, title);
    const int rows_visible = 5;
    const int row_height = 10;
    const int first_y = 25;

    if (cursor < *scroll_top) {
        *scroll_top = cursor;
    } else if (cursor >= *scroll_top + rows_visible) {
        *scroll_top = cursor - rows_visible + 1;
    }

    u8g2_SetFont(u, u8g2_font_6x10_tf);
    if (count == 0) {
        u8g2_DrawStr(u, 6, 40, "(empty)");
        return;
    }

    for (int row = 0; row < rows_visible; row++) {
        int idx = *scroll_top + row;
        if (idx >= count) {
            break;
        }
        int y = first_y + row * row_height;
        if (idx == cursor) {
            u8g2_DrawBox(u, 0, y - 9, 128, row_height);
            u8g2_SetDrawColor(u, 0);
        }
        char line[24];
        snprintf(line, sizeof(line), "%.20s", label_fn(idx, ctx));
        u8g2_DrawStr(u, 4, y, line);
        if (idx == cursor) {
            u8g2_SetDrawColor(u, 1);
        }
    }
}

static const char *main_menu_label(int idx, void *ctx) {
    (void)ctx;
    return main_menu[idx].label;
}

static const char *library_label(int idx, void *ctx) {
    (void)ctx;
    return music_library_name(idx);
}

static char s_scan_line[28];
static const char *wifi_scan_label(int idx, void *ctx) {
    (void)ctx;
    const wifi_ap_info_t *ap = sync_manager_scan_ap(idx);
    if (!ap) {
        return "";
    }
    // Leading '#' marks a secured network.
    snprintf(s_scan_line, sizeof(s_scan_line), "%s%.18s", ap->secure ? "#" : " ", ap->ssid);
    return s_scan_line;
}

static const char *bt_device_label(int idx, void *ctx) {
    (void)ctx;
    return bt_manager_device_name(idx);
}

// ===========================================================================
// Screen renderers
// ===========================================================================
static void render_welcome_screen(u8g2_t *u) {
    u8g2_ClearBuffer(u);
    u8g2_SetFont(u, u8g2_font_6x10_tf);
    u8g2_DrawStr(u, 4, 12, "* BOOTING SYSTEM *");
    u8g2_DrawLine(u, 0, 15, 128, 15);

    u8g2_SetFont(u, u8g2_font_7x14B_tf);
    u8g2_DrawStr(u, 32, 38, "AURASYNC");

    int dot_count = (state_timer_ticks / 10) % 4;
    char status_msg[24] = "Initializing";
    for (int i = 0; i < dot_count; i++) {
        strcat(status_msg, ".");
    }
    u8g2_SetFont(u, u8g2_font_5x7_tf);
    u8g2_DrawStr(u, 30, 54, status_msg);

    u8g2_SendBuffer(u);
    state_timer_ticks++;

    if (state_timer_ticks >= 90) { // ~3 seconds
        current_ui_state = UI_STATE_NOW_PLAYING;
        u8g2_ClearBuffer(u);
    }
}

static void render_now_playing(u8g2_t *u) {
    audio_status_t status;
    audio_player_get_status(&status);

    u8g2_ClearBuffer(u);

    u8g2_SetFont(u, u8g2_font_5x7_tf);
    u8g2_DrawStr(u, 4, 11, "AURASYNC");
    const char *state_label = "[IDLE]";
    switch (status.state) {
        case AUDIO_STATE_PLAYING: state_label = "[PLAY]"; break;
        case AUDIO_STATE_PAUSED:  state_label = "[PAUSE]"; break;
        case AUDIO_STATE_STOPPED: state_label = "[STOP]"; break;
        case AUDIO_STATE_ERROR:   state_label = "[ERR]"; break;
        default: break;
    }
    u8g2_DrawStr(u, 92, 11, state_label);
    u8g2_DrawLine(u, 0, 15, 128, 15);

    const char *title = status.title[0] ? status.title : "No track loaded";
    u8g2_SetFont(u, u8g2_font_6x12_tf);
    u8g2_DrawStr(u, scroll_x_offset, 28, title);
    scroll_x_offset -= 1;
    int16_t text_width = u8g2_GetStrWidth(u, title);
    if (scroll_x_offset < -text_width) {
        scroll_x_offset = 128;
    }

    char vol_str[16];
    snprintf(vol_str, sizeof(vol_str), "Vol %u%%", status.volume);
    u8g2_SetFont(u, u8g2_font_5x8_tf);
    u8g2_DrawStr(u, 4, 42, vol_str);

    uint8_t bar_x = 4, bar_y = 48, bar_max_width = 120, bar_height = 5;
    u8g2_DrawFrame(u, bar_x, bar_y, bar_max_width, bar_height);
    if (status.total_seconds > 0) {
        uint8_t filled = (status.elapsed_seconds * bar_max_width) / status.total_seconds;
        if (filled > bar_max_width) filled = bar_max_width;
        u8g2_DrawBox(u, bar_x, bar_y, filled, bar_height);
    }

    char time_buf[16];
    format_time(time_buf, sizeof(time_buf), status.elapsed_seconds);
    u8g2_DrawStr(u, 4, 62, time_buf);
    format_time(time_buf, sizeof(time_buf), status.total_seconds);
    u8g2_DrawStr(u, 100, 62, time_buf);

    u8g2_SendBuffer(u);
}

static void render_menu(u8g2_t *u) {
    u8g2_ClearBuffer(u);
    draw_list(u, "MENU", MAIN_MENU_COUNT, menu_cursor, &menu_scroll_top,
              main_menu_label, NULL);
    u8g2_SendBuffer(u);
}

static void render_library(u8g2_t *u) {
    u8g2_ClearBuffer(u);
    draw_list(u, "LIBRARY", music_library_count(), library_cursor,
              &library_scroll_top, library_label, NULL);
    u8g2_SendBuffer(u);
}

static void render_volume(u8g2_t *u) {
    u8g2_ClearBuffer(u);
    draw_header(u, "VOLUME");
    uint8_t vol = audio_player_get_volume();

    char vol_str[16];
    snprintf(vol_str, sizeof(vol_str), "%u%%", vol);
    u8g2_SetFont(u, u8g2_font_7x14B_tf);
    u8g2_DrawStr(u, 52, 36, vol_str);

    uint8_t bar_x = 8, bar_y = 46, bar_w = 112, bar_h = 8;
    u8g2_DrawFrame(u, bar_x, bar_y, bar_w, bar_h);
    u8g2_DrawBox(u, bar_x, bar_y, (vol * bar_w) / 100, bar_h);

    u8g2_SetFont(u, u8g2_font_4x6_tf);
    u8g2_DrawStr(u, 8, 63, "UP/DOWN adjust  BACK exit");
    u8g2_SendBuffer(u);
}

static void render_wifi(u8g2_t *u) {
    u8g2_ClearBuffer(u);
    draw_header(u, "WIFI SETTINGS");
    u8g2_SetFont(u, u8g2_font_5x8_tf);

    char line[40];
    bool connected = sync_manager_wifi_connected();
    snprintf(line, sizeof(line), "%.14s %s", sync_manager_ssid(), connected ? "[OK]" : "[--]");
    u8g2_DrawStr(u, 4, 26, line);

    char ip[20];
    sync_manager_get_ip(ip, sizeof(ip));
    snprintf(line, sizeof(line), "IP %s", ip);
    u8g2_DrawStr(u, 4, 36, line);

    // Two action rows: Scan networks / Sync now.
    for (int i = 0; i < 2; i++) {
        int y = 49 + i * 11;
        if (i == wifi_settings_cursor) {
            u8g2_DrawBox(u, 0, y - 9, 128, 11);
            u8g2_SetDrawColor(u, 0);
        }
        if (i == 1) {
            const char *s = "Sync now";
            char b[28];
            switch (sync_manager_status()) {
                case SYNC_RUNNING: s = "Syncing..."; break;
                case SYNC_DONE:
                    snprintf(b, sizeof(b), "Sync now (%d new)", sync_manager_last_count());
                    s = b;
                    break;
                case SYNC_FAILED: s = "Sync failed"; break;
                default: break;
            }
            u8g2_DrawStr(u, 6, y, s);
        } else {
            u8g2_DrawStr(u, 6, y, "Scan networks");
        }
        if (i == wifi_settings_cursor) {
            u8g2_SetDrawColor(u, 1);
        }
    }
    u8g2_SendBuffer(u);
}

static void render_wifi_scan(u8g2_t *u) {
    u8g2_ClearBuffer(u);
    const char *title = (sync_manager_scan_state() == WIFI_SCAN_RUNNING)
                            ? "SCANNING..." : "SELECT NETWORK";
    draw_list(u, title, sync_manager_scan_count(), wifi_scan_cursor,
              &wifi_scan_scroll_top, wifi_scan_label, NULL);
    u8g2_SendBuffer(u);
}

static void render_wifi_password(u8g2_t *u) {
    u8g2_ClearBuffer(u);
    draw_header(u, "PASSWORD");
    u8g2_SetFont(u, u8g2_font_5x8_tf);

    char line[40];
    snprintf(line, sizeof(line), "%.20s", selected_ssid);
    u8g2_DrawStr(u, 4, 26, line);

    // Show the tail of what's been entered, with a caret.
    const char *p = password_buf;
    if (password_len > 20) {
        p = password_buf + (password_len - 20);
    }
    char shown[24];
    snprintf(shown, sizeof(shown), "%s_", p);
    u8g2_DrawStr(u, 4, 40, shown);

    char cur[16];
    snprintf(cur, sizeof(cur), "Add: [%c]", PASSWORD_CHARSET[charset_idx]);
    u8g2_DrawStr(u, 4, 52, cur);

    u8g2_SetFont(u, u8g2_font_4x6_tf);
    u8g2_DrawStr(u, 4, 62, "U/D pick  SEL add  BK del  HOLD ok");
    u8g2_SendBuffer(u);
}

static void render_bluetooth(u8g2_t *u) {
    u8g2_ClearBuffer(u);
    draw_header(u, "BLUETOOTH");
    u8g2_SetFont(u, u8g2_font_5x8_tf);

    const char *sel = bt_manager_selected_name();
    char line[40];
    snprintf(line, sizeof(line), "Paired: %.14s", sel[0] ? sel : "none");
    u8g2_DrawStr(u, 4, 26, line);

    const char *status = bt_manager_is_connected() ? "Status: Connected"
                                                    : "Status: Not connected";
    u8g2_DrawStr(u, 4, 35, status);

    char toggle[24];
    snprintf(toggle, sizeof(toggle), "Bluetooth: %s", bt_manager_is_enabled() ? "ON" : "OFF");
    const char *rows[2] = { toggle, "Scan devices" };
    for (int i = 0; i < 2; i++) {
        int y = 47 + i * 9;
        if (i == bt_settings_cursor) {
            u8g2_DrawBox(u, 0, y - 8, 128, 10);
            u8g2_SetDrawColor(u, 0);
        }
        u8g2_DrawStr(u, 6, y, rows[i]);
        if (i == bt_settings_cursor) {
            u8g2_SetDrawColor(u, 1);
        }
    }
    u8g2_SendBuffer(u);
}

static void render_bt_scan(u8g2_t *u) {
    u8g2_ClearBuffer(u);
    const char *title = bt_manager_is_scanning() ? "SCANNING..." : "SELECT DEVICE";
    draw_list(u, title, bt_manager_device_count(), bt_scan_cursor,
              &bt_scan_scroll_top, bt_device_label, NULL);
    u8g2_SendBuffer(u);
}

static void render_info(u8g2_t *u) {
    u8g2_ClearBuffer(u);
    draw_header(u, "DEVICE INFO");
    u8g2_SetFont(u, u8g2_font_5x8_tf);

    char line[32];
    snprintf(line, sizeof(line), "FW: v%s", FIRMWARE_VERSION);
    u8g2_DrawStr(u, 4, 26, line);

    snprintf(line, sizeof(line), "Heap: %lu KB",
             (unsigned long)(esp_get_free_heap_size() / 1024));
    u8g2_DrawStr(u, 4, 37, line);

    snprintf(line, sizeof(line), "Tracks: %d", music_library_count());
    u8g2_DrawStr(u, 4, 48, line);

    snprintf(line, sizeof(line), "Up: %llus",
             (unsigned long long)(esp_timer_get_time() / 1000000));
    u8g2_DrawStr(u, 4, 59, line);

    u8g2_SendBuffer(u);
}

// ===========================================================================
// Input handling per screen
// ===========================================================================
static void handle_menu_input(button_event_t ev) {
    switch (ev) {
        case BTN_UP:
            menu_cursor = (menu_cursor - 1 + MAIN_MENU_COUNT) % MAIN_MENU_COUNT;
            break;
        case BTN_DOWN:
            menu_cursor = (menu_cursor + 1) % MAIN_MENU_COUNT;
            break;
        case BTN_SELECT:
            current_ui_state = main_menu[menu_cursor].target;
            if (current_ui_state == UI_STATE_LIBRARY) {
                music_library_refresh();
            }
            break;
        case BTN_BACK:
            current_ui_state = UI_STATE_NOW_PLAYING;
            break;
        default:
            break;
    }
}

static void handle_library_input(button_event_t ev) {
    int count = music_library_count();
    switch (ev) {
        case BTN_UP:
            if (count > 0) {
                library_cursor = (library_cursor - 1 + count) % count;
            }
            break;
        case BTN_DOWN:
            if (count > 0) {
                library_cursor = (library_cursor + 1) % count;
            }
            break;
        case BTN_SELECT:
            music_library_play(library_cursor);
            scroll_x_offset = 128;
            current_ui_state = UI_STATE_NOW_PLAYING;
            break;
        case BTN_BACK:
            current_ui_state = UI_STATE_MENU;
            break;
        default:
            break;
    }
}

static void handle_now_playing_input(button_event_t ev) {
    switch (ev) {
        case BTN_SELECT:
            audio_player_toggle_pause();
            break;
        case BTN_SELECT_LONG:
            current_ui_state = UI_STATE_MENU;
            break;
        case BTN_UP:
            music_library_play_relative(-1);
            scroll_x_offset = 128;
            break;
        case BTN_DOWN:
            music_library_play_relative(1);
            scroll_x_offset = 128;
            break;
        default:
            break;
    }
}

static void handle_volume_input(button_event_t ev) {
    uint8_t vol = audio_player_get_volume();
    switch (ev) {
        case BTN_UP:
            audio_player_set_volume(vol <= 95 ? vol + 5 : 100);
            break;
        case BTN_DOWN:
            audio_player_set_volume(vol >= 5 ? vol - 5 : 0);
            break;
        case BTN_BACK:
            current_ui_state = UI_STATE_MENU;
            break;
        default:
            break;
    }
}

static void handle_wifi_input(button_event_t ev) {
    switch (ev) {
        case BTN_UP:
        case BTN_DOWN:
            wifi_settings_cursor = (wifi_settings_cursor + 1) % 2;
            break;
        case BTN_SELECT:
            if (wifi_settings_cursor == 0) {
                sync_manager_request_scan();
                wifi_scan_cursor = 0;
                wifi_scan_scroll_top = 0;
                current_ui_state = UI_STATE_WIFI_SCAN;
            } else {
                sync_manager_request();
            }
            break;
        case BTN_BACK:
            current_ui_state = UI_STATE_MENU;
            break;
        default:
            break;
    }
}

static void handle_wifi_scan_input(button_event_t ev) {
    int count = sync_manager_scan_count();
    switch (ev) {
        case BTN_UP:
            if (count > 0) {
                wifi_scan_cursor = (wifi_scan_cursor - 1 + count) % count;
            }
            break;
        case BTN_DOWN:
            if (count > 0) {
                wifi_scan_cursor = (wifi_scan_cursor + 1) % count;
            }
            break;
        case BTN_SELECT: {
            const wifi_ap_info_t *ap = sync_manager_scan_ap(wifi_scan_cursor);
            if (!ap) {
                break;
            }
            strncpy(selected_ssid, ap->ssid, sizeof(selected_ssid) - 1);
            selected_ssid[sizeof(selected_ssid) - 1] = '\0';
            selected_secure = ap->secure;
            if (selected_secure) {
                password_buf[0] = '\0';
                password_len = 0;
                charset_idx = 0;
                current_ui_state = UI_STATE_WIFI_PASSWORD;
            } else {
                sync_manager_connect(selected_ssid, "");
                current_ui_state = UI_STATE_WIFI;
            }
            break;
        }
        case BTN_BACK:
            current_ui_state = UI_STATE_WIFI;
            break;
        default:
            break;
    }
}

static void handle_wifi_password_input(button_event_t ev) {
    int charset_len = (int)(sizeof(PASSWORD_CHARSET) - 1);
    switch (ev) {
        case BTN_UP:
            charset_idx = (charset_idx - 1 + charset_len) % charset_len;
            break;
        case BTN_DOWN:
            charset_idx = (charset_idx + 1) % charset_len;
            break;
        case BTN_SELECT:
            if (password_len < (int)sizeof(password_buf) - 1) {
                password_buf[password_len++] = PASSWORD_CHARSET[charset_idx];
                password_buf[password_len] = '\0';
            }
            break;
        case BTN_SELECT_LONG:
            sync_manager_connect(selected_ssid, password_buf);
            current_ui_state = UI_STATE_WIFI;
            break;
        case BTN_BACK:
            if (password_len > 0) {
                password_buf[--password_len] = '\0';
            } else {
                current_ui_state = UI_STATE_WIFI_SCAN;
            }
            break;
        default:
            break;
    }
}

static void handle_bluetooth_input(button_event_t ev) {
    switch (ev) {
        case BTN_UP:
        case BTN_DOWN:
            bt_settings_cursor = (bt_settings_cursor + 1) % 2;
            break;
        case BTN_SELECT:
            if (bt_settings_cursor == 0) {
                bool turning_on = !bt_manager_is_enabled();
                bt_manager_set_enabled(turning_on);
                // Reconnect to the remembered sink automatically when powering on.
                if (turning_on) {
                    bt_manager_connect();
                }
            } else if (bt_manager_is_enabled()) {
                bt_manager_start_scan();
                bt_scan_cursor = 0;
                bt_scan_scroll_top = 0;
                current_ui_state = UI_STATE_BT_SCAN;
            }
            break;
        case BTN_BACK:
            current_ui_state = UI_STATE_MENU;
            break;
        default:
            break;
    }
}

static void handle_bt_scan_input(button_event_t ev) {
    int count = bt_manager_device_count();
    switch (ev) {
        case BTN_UP:
            if (count > 0) {
                bt_scan_cursor = (bt_scan_cursor - 1 + count) % count;
            }
            break;
        case BTN_DOWN:
            if (count > 0) {
                bt_scan_cursor = (bt_scan_cursor + 1) % count;
            }
            break;
        case BTN_SELECT:
            bt_manager_select(bt_scan_cursor);
            current_ui_state = UI_STATE_BLUETOOTH;
            break;
        case BTN_BACK:
            current_ui_state = UI_STATE_BLUETOOTH;
            break;
        default:
            break;
    }
}

static void handle_info_input(button_event_t ev) {
    if (ev == BTN_BACK) {
        current_ui_state = UI_STATE_MENU;
    }
}

// ===========================================================================
// UI task
// ===========================================================================
static void ui_task(void *pvParameters) {
    ESP_LOGI(TAG, "AuraSync UI thread running on Core %d", xPortGetCoreID());

    while (1) {
        button_event_t ev = (current_ui_state == UI_STATE_WELCOME)
                                ? BTN_NONE : buttons_poll();

        switch (current_ui_state) {
            case UI_STATE_WELCOME:       render_welcome_screen(&s_u8g2); break;
            case UI_STATE_NOW_PLAYING:   handle_now_playing_input(ev);   render_now_playing(&s_u8g2); break;
            case UI_STATE_MENU:          handle_menu_input(ev);          render_menu(&s_u8g2); break;
            case UI_STATE_LIBRARY:       handle_library_input(ev);       render_library(&s_u8g2); break;
            case UI_STATE_VOLUME:        handle_volume_input(ev);        render_volume(&s_u8g2); break;
            case UI_STATE_WIFI:          handle_wifi_input(ev);          render_wifi(&s_u8g2); break;
            case UI_STATE_WIFI_SCAN:     handle_wifi_scan_input(ev);     render_wifi_scan(&s_u8g2); break;
            case UI_STATE_WIFI_PASSWORD: handle_wifi_password_input(ev); render_wifi_password(&s_u8g2); break;
            case UI_STATE_BLUETOOTH:     handle_bluetooth_input(ev);     render_bluetooth(&s_u8g2); break;
            case UI_STATE_BT_SCAN:       handle_bt_scan_input(ev);       render_bt_scan(&s_u8g2); break;
            case UI_STATE_INFO:          handle_info_input(ev);          render_info(&s_u8g2); break;
        }

        vTaskDelay(pdMS_TO_TICKS(33)); // ~30 FPS
    }
}

// ===========================================================================
// Public API
// ===========================================================================
void ui_init(int sda_pin, int scl_pin) {
    ESP_LOGI(TAG, "Initializing U8g2 Hardware Abstraction Layer...");

    u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
    hal.bus.i2c.sda = sda_pin;
    hal.bus.i2c.scl = scl_pin;
    u8g2_esp32_hal_init(hal);

    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&s_u8g2, U8G2_R0,
                                           u8g2_esp32_i2c_byte_cb,
                                           u8g2_esp32_gpio_and_delay_cb);
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);
}

void ui_start_task(void) {
    xTaskCreatePinnedToCore(ui_task, "UI_Task", 4096, NULL, 4, NULL, 1);
}
