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
#include "driver/i2c.h"

#include "audio_driver.h"
#include "buttons.h"
#include "music_library.h"
#include "sync_manager.h"

static const char *TAG = "AURASYNC_UI";

#define FIRMWARE_VERSION "1.0.0"

// ===========================================================================
// UI state machine
// ===========================================================================
typedef enum {
    UI_STATE_WELCOME,
    UI_STATE_NOW_PLAYING,
    UI_STATE_MENU,
    UI_STATE_LIBRARY,
    UI_STATE_VOLUME,
    UI_STATE_WIFI,
    UI_STATE_BLUETOOTH,
    UI_STATE_INFO,
} ui_state_t;

typedef struct {
    const char *label;
    ui_state_t target;
} menu_item_t;

static const menu_item_t main_menu[] = {
    { "Now Playing",   UI_STATE_NOW_PLAYING },
    { "Library",       UI_STATE_LIBRARY },
    { "Sync (WiFi)",   UI_STATE_WIFI },
    { "Volume",        UI_STATE_VOLUME },
    { "WiFi Settings", UI_STATE_WIFI },
    { "Bluetooth",     UI_STATE_BLUETOOTH },
    { "Info",          UI_STATE_INFO },
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
static bool bluetooth_enabled = false;

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
        current_ui_state = UI_STATE_MENU;
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
    u8g2_SetFont(u, u8g2_font_4x6_tf);
    u8g2_DrawStr(u, 4, 62, "SEL play  BACK menu");
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
    draw_header(u, "WIFI / SYNC");
    u8g2_SetFont(u, u8g2_font_5x8_tf);

    char line[32];
    snprintf(line, sizeof(line), "SSID: %.20s", sync_manager_ssid());
    u8g2_DrawStr(u, 4, 26, line);

    bool connected = sync_manager_wifi_connected();
    snprintf(line, sizeof(line), "Status: %s", connected ? "Connected" : "Offline");
    u8g2_DrawStr(u, 4, 37, line);

    char ip[20];
    sync_manager_get_ip(ip, sizeof(ip));
    snprintf(line, sizeof(line), "IP: %s", ip);
    u8g2_DrawStr(u, 4, 48, line);

    const char *sync_line = "SELECT: Sync now";
    switch (sync_manager_status()) {
        case SYNC_RUNNING: sync_line = "Syncing..."; break;
        case SYNC_DONE:
            snprintf(line, sizeof(line), "Synced (%d new)", sync_manager_last_count());
            sync_line = line;
            break;
        case SYNC_FAILED: sync_line = "Sync failed"; break;
        default: break;
    }
    u8g2_DrawStr(u, 4, 60, sync_line);

    u8g2_SendBuffer(u);
}

static void render_bluetooth(u8g2_t *u) {
    u8g2_ClearBuffer(u);
    draw_header(u, "BLUETOOTH");
    u8g2_SetFont(u, u8g2_font_5x8_tf);

    char line[32];
    snprintf(line, sizeof(line), "State: %s", bluetooth_enabled ? "ON" : "OFF");
    u8g2_DrawStr(u, 4, 28, line);
    u8g2_DrawStr(u, 4, 40, "Name: AuraSync");

    u8g2_SetFont(u, u8g2_font_4x6_tf);
    u8g2_DrawStr(u, 4, 52, "A2DP streaming: planned");
    u8g2_DrawStr(u, 4, 62, "SELECT toggle   BACK exit");
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
        case BTN_UP:
            music_library_play_relative(-1);
            scroll_x_offset = 128;
            break;
        case BTN_DOWN:
            music_library_play_relative(1);
            scroll_x_offset = 128;
            break;
        case BTN_BACK:
            current_ui_state = UI_STATE_MENU;
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
        case BTN_SELECT:
            sync_manager_request();
            break;
        case BTN_BACK:
            current_ui_state = UI_STATE_MENU;
            break;
        default:
            break;
    }
}

static void handle_bluetooth_input(button_event_t ev) {
    switch (ev) {
        case BTN_SELECT:
            bluetooth_enabled = !bluetooth_enabled;
            break;
        case BTN_BACK:
            current_ui_state = UI_STATE_MENU;
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
            case UI_STATE_WELCOME:     render_welcome_screen(&s_u8g2); break;
            case UI_STATE_NOW_PLAYING: handle_now_playing_input(ev); render_now_playing(&s_u8g2); break;
            case UI_STATE_MENU:        handle_menu_input(ev);        render_menu(&s_u8g2); break;
            case UI_STATE_LIBRARY:     handle_library_input(ev);     render_library(&s_u8g2); break;
            case UI_STATE_VOLUME:      handle_volume_input(ev);      render_volume(&s_u8g2); break;
            case UI_STATE_WIFI:        handle_wifi_input(ev);        render_wifi(&s_u8g2); break;
            case UI_STATE_BLUETOOTH:   handle_bluetooth_input(ev);   render_bluetooth(&s_u8g2); break;
            case UI_STATE_INFO:        handle_info_input(ev);        render_info(&s_u8g2); break;
        }

        vTaskDelay(pdMS_TO_TICKS(33)); // ~30 FPS
    }
}

// ===========================================================================
// Public API
// ===========================================================================
// Quick I2C scanner used at startup to detect device addresses.
// Returns first found 7-bit address, or 0 if none found.
static int i2c_scan_test(int sda, int scl) {
    i2c_port_t port = I2C_MASTER_NUM;
    i2c_config_t conf = {0};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = sda;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_io_num = scl;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 100000;
    i2c_param_config(port, &conf);
    esp_err_t rc = i2c_driver_install(port, conf.mode, 0, 0, 0);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "I2C scan: driver_install failed: %d", rc);
        return 0;
    }

    ESP_LOGI(TAG, "I2C scan on SDA=%d SCL=%d", sda, scl);
    int found = 0;
    for (int addr = 1; addr < 127; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
        i2c_master_stop(cmd);
        esp_err_t r = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
            found = addr;
            break; // return the first device we find
        }
    }
    i2c_driver_delete(port);
    return found;
}

void ui_init(int sda_pin, int scl_pin) {
    ESP_LOGI(TAG, "Initializing U8g2 Hardware Abstraction Layer...");

    u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
    hal.bus.i2c.sda = sda_pin;
    hal.bus.i2c.scl = scl_pin;
    u8g2_esp32_hal_init(hal);

    // Run a quick I2C bus scan to detect attached devices and choose address.
    int found_addr = i2c_scan_test(sda_pin, scl_pin);

    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&s_u8g2, U8G2_R0,
                                           u8g2_esp32_i2c_byte_cb,
                                           u8g2_esp32_gpio_and_delay_cb);
    if (found_addr > 0) {
        ESP_LOGI(TAG, "Using detected I2C address 0x%02X", found_addr);
        u8g2_SetI2CAddress(&s_u8g2, found_addr);
    } else {
        ESP_LOGI(TAG, "No I2C device found during scan; defaulting to 0x3C");
        u8g2_SetI2CAddress(&s_u8g2, 0x3C);
    }
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);
}

void ui_start_task(void) {
    xTaskCreatePinnedToCore(ui_task, "UI_Task", 4096, NULL, 4, NULL, 1);
}
