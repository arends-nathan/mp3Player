# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

idf_component_register(
    SRCS main.c
    INCLUDE_DIRS "."
    REQUIRES audio_driver buttons music_library sync_manager ui
             wifi_download nvs_flash
)
