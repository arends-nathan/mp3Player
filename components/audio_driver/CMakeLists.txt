idf_component_register(SRCS "audio_driver.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver
                       PRIV_REQUIRES chmorgan__esp-libhelix-mp3)
