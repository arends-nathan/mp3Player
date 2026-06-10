# C:\Users\natea\mp3Player\components\ui_display\CMakeLists.txt

idf_component_register(SRCS "ui_display.c"
                       INCLUDE_DIRS "include"
                       REQUIRES u8g2) # FIX: Changed PRIV_REQUIRES to REQUIRES