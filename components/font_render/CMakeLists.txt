# components/font_render/CMakeLists.txt
#
# stb_truetype-backed glyph renderer with PSRAM cache.
#
# BEFORE BUILDING: download stb_truetype.h from
#   https://github.com/nothings/stb/blob/master/stb_truetype.h
# and place it at  components/font_render/stb_truetype.h
#
# Fonts are uploaded to the device at /spiffs/fonts/<name>.ttf via
# the web UI (System → Web UI Update or a custom upload endpoint).

cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS         "font_render.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."          # exposes stb_truetype.h to font_render.c only
    REQUIRES     esp_psram
)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    target_compile_options(__idf_font_render PRIVATE
        -Wno-unused-function
        -Wno-missing-field-initializers
    )
endif()
