idf_component_register(
    SRCS "ntp_time.c"
    INCLUDE_DIRS "include"
    REQUIRES config_mgr esp_netif rtc
    PRIV_REQUIRES lwip esp_timer
)