cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "components")

# ── Read firmware version from version.json ──────────────────────────
# Tell CMake to re-run configure automatically whenever version.json
# changes, so fw_version.h is always regenerated on the next build.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/version.json")

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.json" _VER_JSON)
string(REGEX MATCH "\"version\"[[:space:]]*:[[:space:]]*\"([^\"]+)\"" _ "${_VER_JSON}")
set(FW_VERSION "${CMAKE_MATCH_1}")
if(NOT FW_VERSION)
    set(FW_VERSION "0.0.0")
endif()

# Write version into the SPIFFS web tree so the UI can detect mismatches at runtime
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/data/web/version.txt" "${FW_VERSION}")

# Generate fw_version.h into config_mgr's public include dir.
# All components that need FW_VERSION_STR already REQUIRE config_mgr, so
# this header is automatically on their include path without any CMakeLists
# changes.  configure_file() runs at CMake configure time (before any
# compilation), so the file always exists when the compiler is invoked.
#
# NOTE: idf_build_set_property(COMPILE_DEFINITIONS) requires __idf_build_target
# which is only created inside project() / idf_build_init().  Calling it before
# project() silently no-ops, which is why the previous approach produced 0.0.0.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/main/fw_version.h.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/config_mgr/include/fw_version.h"
    @ONLY
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(nextube-fw)

message(STATUS "Nextube-Remaster firmware v${FW_VERSION}")

# Embed the SPIFFS web filesystem into the firmware
spiffs_create_partition_image(spiffs data FLASH_IN_PROJECT)
