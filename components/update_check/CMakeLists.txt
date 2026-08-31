idf_component_register(
    SRCS "update_check.c"
    INCLUDE_DIRS "include"
    REQUIRES config_mgr periodic_net_poll display esp_http_client
    PRIV_REQUIRES mbedtls
)
