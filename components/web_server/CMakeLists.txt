idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json config_mgr wifi_manager ntp_time weather youtube_bili display leds audio microphone app_update esp_wifi esp_timer sht30 joltwallet__littlefs
)
