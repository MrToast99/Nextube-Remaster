idf_component_register(
    SRCS "subscribers.c"
    INCLUDE_DIRS "include"
    REQUIRES config_mgr periodic_net_poll esp_http_client json esp-tls
)
