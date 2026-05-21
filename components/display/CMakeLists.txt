idf_component_register(
    SRCS "display.c"
    INCLUDE_DIRS "include"
    REQUIRES driver config_mgr board espressif__esp_jpeg ntp_time weather subscribers microphone wifi_manager sht30 u8g2
)

# IDF 5.x: managed component include dirs don't always propagate through REQUIRES.
# Use idf_component_get_property to manually add the esp_jpeg include path.
idf_component_get_property(esp_jpeg_dir espressif__esp_jpeg COMPONENT_DIR)
target_include_directories(__idf_display PRIVATE "${esp_jpeg_dir}/include")
