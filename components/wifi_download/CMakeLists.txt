idf_component_register(SRCS "wifi_download.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_http_client mbedtls) # FIX: Changed esp_crt_bundle to mbedtls