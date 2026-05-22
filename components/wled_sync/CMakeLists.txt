idf_component_register(
    SRCS "wled_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES config_mgr wifi_manager lwip freertos
)
