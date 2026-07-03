idf_component_register(
    SRCS "leds.c"
    INCLUDE_DIRS "include"
    REQUIRES driver config_mgr board microphone wled_sync ntp_time
)