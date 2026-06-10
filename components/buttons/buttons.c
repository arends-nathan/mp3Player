#include "buttons.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"

// Navigation buttons (active-low, internal pull-ups). Chosen to avoid the I2C
// bus (21/22) and the I2S amplifier pins (25/26/27).
#define BTN_UP_PIN     32
#define BTN_DOWN_PIN   33
#define BTN_SELECT_PIN 14
#define BTN_BACK_PIN   13

// Ignore transitions closer together than this (contact bounce).
#define DEBOUNCE_US      25000   // 25 ms
// Holding Select at least this long emits BTN_SELECT_LONG (opens the menu).
#define LONG_PRESS_US    700000  // 700 ms

typedef struct {
    gpio_num_t      pin;
    button_event_t  short_event;
    bool            supports_long;
} button_def_t;

static const button_def_t button_defs[] = {
    { BTN_UP_PIN,     BTN_UP,     false },
    { BTN_DOWN_PIN,   BTN_DOWN,   false },
    { BTN_SELECT_PIN, BTN_SELECT, true  },
    { BTN_BACK_PIN,   BTN_BACK,   false },
};
#define BUTTON_COUNT (sizeof(button_defs) / sizeof(button_defs[0]))

// Raw edge captured in the ISR and handed to the service task.
typedef struct {
    int     index;
    int64_t time_us;
    bool    pressed; // true = press (level low), false = release
} raw_edge_t;

static QueueHandle_t s_raw_queue   = NULL; // ISR -> service task
static QueueHandle_t s_event_queue = NULL; // service task -> consumer

// ---------------------------------------------------------------------------
// ISR: timestamp the edge and forward it. All classification happens in the
// service task so the ISR stays minimal.
// ---------------------------------------------------------------------------
static void IRAM_ATTR button_isr_handler(void *arg) {
    int index = (int)(intptr_t)arg;
    raw_edge_t edge = {
        .index = index,
        .time_us = esp_timer_get_time(),
        .pressed = (gpio_get_level(button_defs[index].pin) == 0),
    };
    BaseType_t hp_task_woken = pdFALSE;
    xQueueSendFromISR(s_raw_queue, &edge, &hp_task_woken);
    if (hp_task_woken) {
        portYIELD_FROM_ISR();
    }
}

// ---------------------------------------------------------------------------
// Service task: debounce edges, then classify short vs. long presses.
//
// Non-long buttons fire on press for snappy navigation. The Select button
// fires BTN_SELECT_LONG the moment the hold threshold elapses (while still
// held, so the menu pops up under the finger), or BTN_SELECT on release if it
// was only a tap.
// ---------------------------------------------------------------------------
static void button_service_task(void *arg) {
    bool    down[BUTTON_COUNT]         = { false };
    bool    long_fired[BUTTON_COUNT]   = { false };
    int64_t press_us[BUTTON_COUNT]     = { 0 };
    int64_t last_edge_us[BUTTON_COUNT] = { 0 };

    for (;;) {
        // Wake up no later than the nearest pending long-press deadline.
        TickType_t wait = portMAX_DELAY;
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < BUTTON_COUNT; i++) {
            if (button_defs[i].supports_long && down[i] && !long_fired[i]) {
                int64_t remaining = (press_us[i] + LONG_PRESS_US) - now;
                if (remaining <= 0) {
                    button_event_t ev = BTN_SELECT_LONG;
                    xQueueSend(s_event_queue, &ev, 0);
                    long_fired[i] = true;
                } else {
                    TickType_t t = pdMS_TO_TICKS(remaining / 1000 + 1);
                    if (t < wait) {
                        wait = t;
                    }
                }
            }
        }

        raw_edge_t edge;
        if (xQueueReceive(s_raw_queue, &edge, wait) != pdTRUE) {
            continue; // timed out: loop re-evaluates long-press deadlines
        }

        int i = edge.index;
        if (edge.time_us - last_edge_us[i] < DEBOUNCE_US) {
            continue; // bounce
        }
        last_edge_us[i] = edge.time_us;

        if (edge.pressed) {
            if (down[i]) {
                continue;
            }
            down[i] = true;
            long_fired[i] = false;
            press_us[i] = edge.time_us;
            if (!button_defs[i].supports_long) {
                button_event_t ev = button_defs[i].short_event;
                xQueueSend(s_event_queue, &ev, 0);
            }
        } else {
            if (!down[i]) {
                continue;
            }
            down[i] = false;
            // A long-capable button that was not already classified as a long
            // hold counts as a short tap on release.
            if (button_defs[i].supports_long && !long_fired[i]) {
                button_event_t ev = button_defs[i].short_event;
                xQueueSend(s_event_queue, &ev, 0);
            }
        }
    }
}

void buttons_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BTN_UP_PIN) | (1ULL << BTN_DOWN_PIN) |
                        (1ULL << BTN_SELECT_PIN) | (1ULL << BTN_BACK_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_conf);

    s_raw_queue = xQueueCreate(16, sizeof(raw_edge_t));
    s_event_queue = xQueueCreate(16, sizeof(button_event_t));

    gpio_install_isr_service(0);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        gpio_isr_handler_add(button_defs[i].pin, button_isr_handler, (void *)(intptr_t)i);
    }

    xTaskCreate(button_service_task, "btn_svc", 3072, NULL, 7, NULL);
}

button_event_t buttons_poll(void) {
    button_event_t ev;
    if (s_event_queue && xQueueReceive(s_event_queue, &ev, 0) == pdTRUE) {
        return ev;
    }
    return BTN_NONE;
}
