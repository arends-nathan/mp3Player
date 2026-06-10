#ifndef BUTTONS_H
#define BUTTONS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Discrete navigation events produced by the front-panel buttons.
 */
typedef enum {
    BTN_NONE,
    BTN_UP,
    BTN_DOWN,
    BTN_SELECT,
    BTN_BACK,
} button_event_t;

/**
 * @brief Configure the button GPIOs as inputs with internal pull-ups.
 *
 * Must be called once during boot before ::buttons_poll.
 */
void buttons_init(void);

/**
 * @brief Sample the buttons and return the first newly-pressed one.
 *
 * Uses release-to-press edge detection, which doubles as debounce when polled
 * at the UI frame rate. Returns ::BTN_NONE when nothing new was pressed.
 */
button_event_t buttons_poll(void);

#ifdef __cplusplus
}
#endif

#endif // BUTTONS_H
