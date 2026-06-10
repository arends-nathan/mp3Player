#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "u8g2_esp32_hal.h" // Hardware Mapping Wrapper for ESP-IDF
#include "u8g2.h"

// Hardware Pin Configuration
#define I2C_SDA_PIN          21
#define I2C_SCL_PIN          22

static const char *TAG = "AURASYNC_UI";
u8g2_t u8g2; // Global handle for the display instance

// Central state tracking engine variables
typedef enum {
    UI_STATE_WELCOME,
    UI_STATE_MAIN_SCREEN
} ui_state_t;

static ui_state_t current_ui_state = UI_STATE_WELCOME;
static uint32_t state_timer_ticks = 0;

// Media data structure for managing track metadata
typedef struct {
    const char *title;
    const char *artist;
    uint32_t current_seconds;
    uint32_t total_seconds;
} media_track_t;

// Global instance populated with a testing track
static media_track_t active_track = {
    .title = "Something in the Orange",
    .artist = "Zach Bryan",
    .current_seconds = 45,
    .total_seconds = 228 // 3 mins, 48 secs
};

// Tracking variable for our horizontal text scrolling offset
static int16_t scroll_x_offset = 128; 

// --- UI HELPERS ---

// Welcome Splash - Structured for Color Separation (Yellow Top, Blue Bottom)
void render_welcome_screen(u8g2_t *u8g2_ptr) {
    u8g2_ClearBuffer(u8g2_ptr);
    
    // Top Accent Header Bar (Fits neatly in the 16px Yellow zone)
    u8g2_SetFont(u8g2_ptr, u8g2_font_6x10_tf);
    u8g2_DrawStr(u8g2_ptr, 4, 12, "★ BOOTING SYSTEM ★");
    u8g2_DrawLine(u8g2_ptr, 0, 15, 128, 15); // Physical color boundary line split

    // Big Title Center Stage (Renders in the deep Blue zone)
    u8g2_SetFont(u8g2_ptr, u8g2_font_7x14B_tf);
    u8g2_DrawStr(u8g2_ptr, 32, 38, "AURASYNC");
    
    // Animated loading progress string generation
    int dot_count = (state_timer_ticks / 10) % 4;
    char status_msg[24] = "Initializing";
    for(int i = 0; i < dot_count; i++) {
        strcat(status_msg, ".");
    }
    u8g2_SetFont(u8g2_ptr, u8g2_font_5x7_tf);
    u8g2_DrawStr(u8g2_ptr, 30, 54, status_msg);
    
    u8g2_SendBuffer(u8g2_ptr);
    state_timer_ticks++;
    
    // Hold splash screen for roughly ~3 seconds before transitioning
    if (state_timer_ticks >= 90) {
        current_ui_state = UI_STATE_MAIN_SCREEN;
        u8g2_ClearBuffer(u8g2_ptr);
    }
}

// Main Running View - Shifted and Scaled to fit color bands perfectly
void render_main_screen(u8g2_t *u8g2_ptr) {
    u8g2_ClearBuffer(u8g2_ptr);
    
    // 1. TOP STATUS ROW: Sits entirely inside the Yellow block (Y: 0 to 14)
    u8g2_SetFont(u8g2_ptr, u8g2_font_5x7_tf);
    u8g2_DrawStr(u8g2_ptr, 4, 11, "AURASYNC");
    u8g2_DrawStr(u8g2_ptr, 83, 11, "[PLAYING]");
    u8g2_DrawLine(u8g2_ptr, 0, 15, 128, 15); // Clean dividing separator line

    // 2. SCROLLING SONG TITLE: Deep Blue upper canvas (Y: 16 to 34)
    u8g2_SetFont(u8g2_ptr, u8g2_font_6x12_tf); 
    u8g2_DrawStr(u8g2_ptr, scroll_x_offset, 28, active_track.title);
    
    // Move the marquee text 1 pixel left every frame update step
    scroll_x_offset -= 1;
    
    // Calculate string pixel length to loop the marquee safely
    int16_t text_width = u8g2_GetStrWidth(u8g2_ptr, active_track.title);
    if (scroll_x_offset < -text_width) {
        scroll_x_offset = 128; // Reset back to right edge
    }

    // 3. ARTIST NAME: Deep Blue center canvas (Y: 35 to 45)
    u8g2_SetFont(u8g2_ptr, u8g2_font_5x8_tf);
    u8g2_DrawStr(u8g2_ptr, 4, 42, active_track.artist);

    // 4. PROGRESS BAR & DURATION: Deep Blue lower canvas (Y: 46 to 64)
    uint8_t bar_x = 4;
    uint8_t bar_y = 48;
    uint8_t bar_max_width = 120;
    uint8_t bar_height = 5;

    // Outer box boundary frame for the timeline slider
    u8g2_DrawFrame(u8g2_ptr, bar_x, bar_y, bar_max_width, bar_height);

    // Calculate filled width ratio: (current / total) * max_width
    uint8_t filled_width = (active_track.current_seconds * bar_max_width) / active_track.total_seconds;
    if (filled_width > bar_max_width) filled_width = bar_max_width;

    // Draw the active progress representation block inside the container
    u8g2_DrawBox(u8g2_ptr, bar_x, bar_y, filled_width, bar_height);

    // Track Time Strings buffer management formatting
    char time_string_buffer[16];
    
    // Current running elapsed clock positioning (Left string alignment)
    sprintf(time_string_buffer, "%01ld:%02ld", (long)active_track.current_seconds / 60, (long)active_track.current_seconds % 60);
    u8g2_DrawStr(u8g2_ptr, 4, 62, time_string_buffer);

    // Total track capacity limits (Right string alignment)
    sprintf(time_string_buffer, "%01ld:%02ld", (long)active_track.total_seconds / 60, (long)active_track.total_seconds % 60);
    u8g2_DrawStr(u8g2_ptr, 102, 62, time_string_buffer);

    u8g2_SendBuffer(u8g2_ptr);

    // Increment track mock progress variable every 30 display ticks (~1 real second elapsed)
    static uint8_t runtime_tick_scaler = 0;
    runtime_tick_scaler++;
    if (runtime_tick_scaler >= 30) {
        runtime_tick_scaler = 0;
        active_track.current_seconds++;
        if (active_track.current_seconds > active_track.total_seconds) {
            active_track.current_seconds = 0; // Loop tracking timeline back to start
        }
    }
}

// --- RUNTIME DUAL-CORE TASKS ---

// CORE 1: ANIMATION, SCROLLING & UI ENGINE
void ui_core_task(void *pvParameters) {
    ESP_LOGI(TAG, "AuraSync UI Master Thread running on Core %d", xPortGetCoreID());

    while (1) {
        // Evaluate structural interface state layout maps
        switch (current_ui_state) {
            case UI_STATE_WELCOME:
                render_welcome_screen(&u8g2);
                break;
                
            case UI_STATE_MAIN_SCREEN:
                render_main_screen(&u8g2);
                break;
        }

        // Keep core loop execution locked to a steady 30 FPS (~33ms intervals)
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}

// CORE 0: HIGH-PRIORITY AUDIO PROCESSING PIPELINE (HOLDS TASK ISOLATION PROTECTION)
void audio_core_task(void *pvParameters) {
    ESP_LOGI(TAG, "Audio task spinning up on Core %d", xPortGetCoreID());
    while (1) {
        // Your future decoding buffers and I2S stream pipelines will operate flat-out here
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }
}

// --- SYSTEM INITIALIZATION SETUP ---
void app_main(void) {
    ESP_LOGI(TAG, "Initializing U8g2 Hardware Abstraction Layer...");

    // 1. Map physical I2C pins using the nested dot traversal found inside your branch header
    u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
    hal.bus.i2c.sda = I2C_SDA_PIN;
    hal.bus.i2c.scl = I2C_SCL_PIN;
    u8g2_esp32_hal_init(hal);

    // 2. Bind driver instructions for your 128x64 I2C SSD1306 display hardware module
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&u8g2, U8G2_R0, 
                                           u8g2_esp32_i2c_byte_cb, 
                                           u8g2_esp32_gpio_and_delay_cb);

    // 3. Complete internal hardware register mapping sequence execution
    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0); // Wake up display panel from its power-on sleep state

    // 4. Fire up the independent parallel task frames pinned to their respective processor cores
    xTaskCreatePinnedToCore(audio_core_task, "Audio_Task", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(ui_core_task,    "UI_Task",    4096, NULL, 4, NULL, 1);
}