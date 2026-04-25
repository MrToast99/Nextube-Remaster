cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "components")

# ── Read firmware version from version.json ──────────────────────────
# Tell CMake to re-run configure automatically whenever version.json changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/version.json")

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.json" _VER_JSON)
# Use a robust semver pattern rather than [[:space:]] (POSIX classes can
# misbehave on Windows where file(READ) preserves CRLF line endings).
string(REGEX MATCH "\"firmware_version\"[^\"]*\"([0-9]+[.][0-9]+[.][0-9]+)\"" _ "${_VER_JSON}")
set(FW_VERSION "${CMAKE_MATCH_1}")
if(NOT FW_VERSION)
    message(WARNING "Could not parse firmware_version from version.json – defaulting to 0.0.0")
    set(FW_VERSION "0.0.0")
endif()

string(REGEX MATCH "\"fs_version\"[^\"]*\"([0-9]+[.][0-9]+[.][0-9]+)\"" _ "${_VER_JSON}")
set(FS_VERSION "${CMAKE_MATCH_1}")
if(NOT FS_VERSION)
    message(WARNING "Could not parse fs_version from version.json – defaulting to ${FW_VERSION}")
    set(FS_VERSION "${FW_VERSION}")
endif()

# Write LittleFS version into the web tree for runtime mismatch detection.
# The firmware reports FW_VERSION_STR; the web UI reports this file's content.
# Bumping only fs_version (data/ changes) never triggers the mismatch banner.
# Bumping only firmware_version (code changes) will trigger it until littlefs.bin is re-flashed.
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/data/web/version.txt" "${FS_VERSION}")

# Generate fw_version.h as a #ifndef-guarded header (belt-and-suspenders
# fallback in case a translation unit includes it without the -D flag).
# The authoritative injection is target_compile_definitions(PUBLIC) in
# components/config_mgr/CMakeLists.txt — that propagates to every consumer.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/main/fw_version.h.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/config_mgr/include/fw_version.h"
    @ONLY
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(nextube-fw)

message(STATUS "Nextube-Remaster firmware v${FW_VERSION}  LittleFS v${FS_VERSION}")

# Embed the LittleFS web filesystem into the firmware
littlefs_create_partition_image(littlefs data FLASH_IN_PROJECT)
