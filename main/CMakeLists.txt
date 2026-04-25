idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        board
        config_mgr
        display
        audio
        microphone
        leds
        touch
        rtc
        sht30
        wifi_manager
        web_server
        ntp_time
        weather
        youtube_bili
        app_update
)
