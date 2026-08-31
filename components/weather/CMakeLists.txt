idf_component_register(
    SRCS "weather.c"
    INCLUDE_DIRS "include"
    REQUIRES config_mgr periodic_net_poll esp_http_client json
    PRIV_REQUIRES esp_timer mbedtls
)