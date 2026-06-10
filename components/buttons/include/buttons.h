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
    BTN_SELECT_LONG, /*!< Select held past the long-press threshold */
    BTN_BACK,
} button_event_t;

/**
 * @brief Install the GPIO interrupts and start the button service task.
 *
 * Buttons are edge-triggered: an ISR timestamps each transition and a small
 * service task debounces it and classifies short vs. long presses. Must be
 * called once during boot before ::buttons_poll.
 */
void buttons_init(void);

/**
 * @brief Return the next pending button event, or ::BTN_NONE if none.
 *
 * Non-blocking. Events are produced asynchronously by the interrupt-driven
 * service task and buffered in a queue, so this never misses a press between
 * calls.
 */
button_event_t buttons_poll(void);

#ifdef __cplusplus
}
#endif

#endif // BUTTONS_H
