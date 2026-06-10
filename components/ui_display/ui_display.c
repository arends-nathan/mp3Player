#include "ui_display.h"
#include <string.h>
#include <stdio.h>

u8g2_t u8g2; // Definition of global handle

typedef enum {
    UI_STATE_WELCOME,
    UI_STATE_MAIN_SCREEN
} ui_state_t;

static ui_state_t current_ui_state = UI_STATE_WELCOME;
static uint32_t state_timer_ticks = 0;
static int16_t scroll_x_offset = 128;

typedef struct {
    const char *title;
    const char *artist;
    uint32_t current_seconds;
    uint32_t total_seconds;
} media_track_t;

static media_track_t active_track = {
    .title = "Something in the Orange",
    .artist = "Zach Bryan",
    .current_seconds = 45,
    .total_seconds = 228
};

static void render_welcome_screen(void) {
    u8g2_ClearBuffer(&u8g2);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(&u8g2, 4, 12, "★ BOOTING SYSTEM ★");
    u8g2_DrawLine(&u8g2, 0, 15, 128, 15);

    u8g2_SetFont(&u8g2, u8g2_font_7x14B_tf);
    u8g2_DrawStr(&u8g2, 32, 38, "AURASYNC");
    
    int dot_count = (state_timer_ticks / 10) % 4;
    char status_msg[24] = "Initializing";
    for(int i = 0; i < dot_count; i++) strcat(status_msg, ".");
    
    u8g2_SetFont(&u8g2, u8g2_font_5x7_tf);
    u8g2_DrawStr(&u8g2, 36, 54, status_msg);
    u8g2_SendBuffer(&u8g2);
    
    state_timer_ticks++;
    if (state_timer_ticks >= 90) {
        current_ui_state = UI_STATE_MAIN_SCREEN;
    }
}

static void render_main_screen(void) {
    u8g2_ClearBuffer(&u8g2);
    
    // Yellow top status banner
    u8g2_SetFont(&u8g2, u8g2_font_5x7_tf);
    u8g2_DrawStr(&u8g2, 4, 11, "AURASYNC");
    u8g2_DrawStr(&u8g2, 94, 11, "[PLAYING]");
    u8g2_DrawLine(&u8g2, 0, 15, 128, 15);

    // Blue bottom text marquee
    u8g2_SetFont(&u8g2, u8g2_font_6x12_tf); 
    u8g2_DrawStr(&u8g2, scroll_x_offset, 28, active_track.title);
    scroll_x_offset -= 1;
    int16_t text_width = u8g2_GetStrWidth(&u8g2, active_track.title);
    if (scroll_x_offset < -text_width) scroll_x_offset = 128;

    // Artist details
    u8g2_SetFont(&u8g2, u8g2_font_5x8_tf);
    u8g2_DrawStr(&u8g2, 4, 42, active_track.artist);

    // Timeline slider
    uint8_t bar_max_width = 120;
    u8g2_DrawFrame(&u8g2, 4, 48, bar_max_width, 5);
    uint8_t filled_width = (active_track.current_seconds * bar_max_width) / active_track.total_seconds;
    u8g2_DrawBox(&u8g2, 4, 48, filled_width > bar_max_width ? bar_max_width : filled_width, 5);

    // Clock handles
    char time_buf[16];
    sprintf(time_buf, "%01lu:%02lu", (long)active_track.current_seconds / 60, (long)active_track.current_seconds % 60);
    u8g2_DrawStr(&u8g2, 4, 62, time_buf);
    sprintf(time_buf, "%01lu:%02lu", (long)active_track.total_seconds / 60, (long)active_track.total_seconds % 60);
    u8g2_DrawStr(&u8g2, 102, 62, time_buf);

    u8g2_SendBuffer(&u8g2);

    static uint8_t tick_scaler = 0;
    if (++tick_scaler >= 30) {
        tick_scaler = 0;
        if (++active_track.current_seconds > active_track.total_seconds) active_track.current_seconds = 0;
    }
}

void ui_display_init(void) {
    current_ui_state = UI_STATE_WELCOME;
    state_timer_ticks = 0;
}

void ui_display_update_frame(void) {
    if (current_ui_state == UI_STATE_WELCOME) {
        render_welcome_screen();
    } else {
        render_main_screen();
    }
}