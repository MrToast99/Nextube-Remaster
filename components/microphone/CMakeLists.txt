idf_component_register(
    SRCS "microphone.c"
    INCLUDE_DIRS "include"
    REQUIRES driver board
    PRIV_REQUIRES esp_timer
)
