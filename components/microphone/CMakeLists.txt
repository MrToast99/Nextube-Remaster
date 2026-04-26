idf_component_register(
    SRCS "microphone.c"
    INCLUDE_DIRS "include"
    REQUIRES driver board config_mgr
    PRIV_REQUIRES esp_timer esp_adc
)
