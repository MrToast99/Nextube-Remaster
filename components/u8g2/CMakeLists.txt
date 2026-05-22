# components/u8g2/CMakeLists.txt
#
# Fetches the olikraus/u8g2 source from GitHub at CMake configure time and
# registers it as a local IDF component.  This avoids any dependency on the
# ESP Component Registry (where olikraus/u8g2 is not published).
#
# IMPORTANT: ESP-IDF scans component CMakeLists files in CMake script mode
# (cmake -P component_get_requirements.cmake) to extract REQUIRES lists.
# FetchContent internally calls define_property which is not scriptable and
# would abort that phase.  Everything except idf_component_register() is
# therefore guarded with if(NOT CMAKE_SCRIPT_MODE_FILE) so the requirements
# scan sees an empty-source component (fine — it only needs REQUIRES), while
# the real build pass downloads and compiles the sources normally.
#
# The download is a shallow clone cached in the CMake build directory under
# _deps/u8g2_upstream-src/.  Subsequent cmake runs reuse the cache; only the
# first clean build requires network access.

cmake_minimum_required(VERSION 3.16)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    include(FetchContent)

    FetchContent_Declare(
        u8g2_upstream
        GIT_REPOSITORY https://github.com/olikraus/u8g2.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
    )

    FetchContent_GetProperties(u8g2_upstream)
    if(NOT u8g2_upstream_POPULATED)
        message(STATUS "U8g2: fetching from GitHub (shallow clone, master)...")
        FetchContent_Populate(u8g2_upstream)
        message(STATUS "U8g2: source at ${u8g2_upstream_SOURCE_DIR}")
    endif()

    file(GLOB U8G2_SRCS "${u8g2_upstream_SOURCE_DIR}/csrc/*.c")

    if(NOT U8G2_SRCS)
        message(FATAL_ERROR "U8g2: no source files found under ${u8g2_upstream_SOURCE_DIR}/csrc/ — check the FetchContent step above.")
    endif()
endif()

idf_component_register(
    SRCS         ${U8G2_SRCS}
    INCLUDE_DIRS "${u8g2_upstream_SOURCE_DIR}/csrc"
)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    # Suppress warnings from U8g2's upstream C code that we do not own.
    target_compile_options(__idf_u8g2 PRIVATE
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-implicit-fallthrough
        -Wno-missing-field-initializers
    )

    # Use 16-bit coordinates throughout U8g2 (u8g2_uint_t = uint16_t).
    # Without this, u8g2_uint_t defaults to uint8_t in 8-bit mode.  When a
    # buffer-relative coordinate is negative (e.g. bSun = sy − y0 < 0), the
    # cast to uint8_t wraps to ~252; arithmetic on that value (252 + radius)
    # wraps again to a small positive number that passes the clip check and
    # renders phantom pixels — visible as artifact lines in wx_sun_anim_frame.
    # With U8G2_16BIT, negative values become ~65532 and are safely outside
    # the 0–63 clip range, so they are discarded without rendering.
    target_compile_definitions(__idf_u8g2 PUBLIC U8G2_16BIT)
endif()
