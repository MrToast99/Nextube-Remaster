idf_component_register(
    SRCS "periodic_net_poll.c"
    INCLUDE_DIRS "include"
    REQUIRES wifi_manager
    PRIV_REQUIRES esp_timer
)
