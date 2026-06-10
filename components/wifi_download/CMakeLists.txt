idf_component_register(SRCS "wifi_download.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client esp_wifi esp_event esp_netif nvs_flash
                       PRIV_REQUIRES esp-tls json)