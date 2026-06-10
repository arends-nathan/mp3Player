#include "buttons.h"

#include <stdbool.h>
#include "driver/gpio.h"

// Navigation buttons (active-low, internal pull-ups). Chosen to avoid the I2C
// bus (21/22) and the I2S amplifier pins (25/26/27).
#define BTN_UP_PIN     32
#define BTN_DOWN_PIN   33
#define BTN_SELECT_PIN 14
#define BTN_BACK_PIN   13

static const gpio_num_t button_pins[] = {
    BTN_UP_PIN, BTN_DOWN_PIN, BTN_SELECT_PIN, BTN_BACK_PIN,
};
static const button_event_t button_events[] = {
    BTN_UP, BTN_DOWN, BTN_SELECT, BTN_BACK,
};
#define BUTTON_COUNT (sizeof(button_pins) / sizeof(button_pins[0]))

static bool button_prev_pressed[BUTTON_COUNT] = { false };

void buttons_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BTN_UP_PIN) | (1ULL << BTN_DOWN_PIN) |
                        (1ULL << BTN_SELECT_PIN) | (1ULL << BTN_BACK_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

button_event_t buttons_poll(void) {
    button_event_t event = BTN_NONE;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        bool pressed = (gpio_get_level(button_pins[i]) == 0);
        if (pressed && !button_prev_pressed[i] && event == BTN_NONE) {
            event = button_events[i];
        }
        button_prev_pressed[i] = pressed;
    }
    return event;
}
