#ifndef UI_DISPLAY_H
#define UI_DISPLAY_H

#include "u8g2.h"

// Expose the global display handle for system configuration
extern u8g2_t u8g2;

/**
 * @brief Initializes the structural layout engine state machine.
 */
void ui_display_init(void);

/**
 * @brief Progresses and renders the current screen layout frame based on state.
 */
void ui_display_update_frame(void);

#endif // UI_DISPLAY_H