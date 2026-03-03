idf_component_register(
    SRCS "youtube_bili.c"
    INCLUDE_DIRS "include"
    REQUIRES config_mgr esp_http_client json esp-tls
)
