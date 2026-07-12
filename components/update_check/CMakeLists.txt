idf_component_register(
    SRCS "update_check.c"
    INCLUDE_DIRS "include"
    REQUIRES config_mgr wifi_manager display esp_http_client
    PRIV_REQUIRES esp_timer mbedtls
)
