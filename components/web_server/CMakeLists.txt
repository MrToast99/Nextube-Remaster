idf_component_register(
    SRCS "web_server.c" "auth.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."   # auth.h — component-private, used by web_server.c only
    REQUIRES esp_http_server esp_http_client json config_mgr wifi_manager ntp_time weather subscribers display leds audio microphone app_update esp_wifi esp_timer sht30 joltwallet__littlefs nvs_flash mbedtls
)
