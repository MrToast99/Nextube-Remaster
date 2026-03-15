idf_component_register(
    SRCS "wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif config_mgr nvs_flash espressif__mdns esp_timer
)