
# Read firmware version from version.json so we can propagate it as a
# compile definition to every component that links against config_mgr.
#
# Why here and not in the root CMakeLists?
#   add_compile_definitions() at project root only affects the current CMake
#   directory scope — it does NOT reach component subdirectories.
#   target_compile_definitions(${COMPONENT_LIB} PUBLIC ...) propagates the
#   definition to every consumer via CMake's target dependency graph, which
#   is exactly what ESP-IDF components use.  Since every component that needs
#   FW_VERSION_STR already REQUIRES config_mgr, this one line covers them all.
#
# FW_VERSION is set by the root CMakeLists.txt before project() is called,
# so it is inherited into this directory scope when ESP-IDF processes
# components during idf_build_process().
if(NOT FW_VERSION)
    # Fallback: re-parse version.json if somehow FW_VERSION wasn't inherited
    file(READ "${CMAKE_SOURCE_DIR}/version.json" _VER_JSON)
    string(REGEX MATCH "\"version\"[^\"]*\"([0-9]+[.][0-9]+[.][0-9]+)\"" _ "${_VER_JSON}")
    set(FW_VERSION "${CMAKE_MATCH_1}")
    if(NOT FW_VERSION)
        set(FW_VERSION "0.0.0")
        message(WARNING "config_mgr: could not read version from version.json")
    endif()
endif()

idf_component_register(
    SRCS "config_mgr.c"
    INCLUDE_DIRS "include"
    REQUIRES json spiffs
)

# PUBLIC: applies to config_mgr itself AND to every component that REQUIRES
# config_mgr (main, web_server, weather, display, …).
# The #ifndef guard in fw_version.h prevents double-definition.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    "FW_VERSION_STR=\"${FW_VERSION}\""
)
