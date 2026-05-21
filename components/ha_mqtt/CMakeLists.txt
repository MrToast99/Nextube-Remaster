idf_component_register(
    SRCS "ha_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES
        config_mgr
        sht30
        wifi_manager
        mqtt
)
