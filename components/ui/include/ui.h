#ifndef UI_H
#define UI_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the OLED display and the UI state machine.
 *
 * Brings up the u8g2 driver over I2C on the given pins. Must be called after
 * the peripheral modules (buttons, music library, sync manager) are ready.
 *
 * @param sda_pin I2C data GPIO.
 * @param scl_pin I2C clock GPIO.
 */
void ui_init(int sda_pin, int scl_pin);

/**
 * @brief Launch the UI rendering task (pinned to core 1).
 */
void ui_start_task(void);

#ifdef __cplusplus
}
#endif

#endif // UI_H
