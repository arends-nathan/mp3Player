idf_component_register(SRCS "ui.c"
                       INCLUDE_DIRS "include"
                       REQUIRES u8g2 u8g2-hal-esp-idf audio_driver buttons
                                music_library sync_manager bt_manager esp_timer)
